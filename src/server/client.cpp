// The client of the control API over cpp-httplib's client. httplib.h comes first: on Windows it
// includes winsock2.h, which must come before windows.h. It is used header-only, without TLS or
// compression (the only CPPHTTPLIB_* definition is CPPHTTPLIB_SEND_FLAGS, MSG_NOSIGNAL on POSIX
// systems other than macOS, from praenora_httplib), and never resolves a name: a TCP address is
// an IP address, handed to the system as one.
#include <httplib.h>

#include "client.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>

namespace praenora::server {

namespace {

using nlohmann::json;

// No write of a request raises SIGPIPE, whatever the process's disposition of it: a server that
// closes or resets the connection fails the request, never the process. On macOS each socket is
// set never to raise it (SO_NOSIGPIPE, set_no_sigpipe()); on other POSIX systems every send()
// passes MSG_NOSIGNAL, which praenora_httplib defines as CPPHTTPLIB_SEND_FLAGS. Windows has no
// SIGPIPE.
#if defined(SO_NOSIGPIPE)
void set_no_sigpipe(socket_t descriptor) {
    static_cast<void>(httplib::set_socket_opt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, 1));
}
#elif !defined(_WIN32)
static_assert((CPPHTTPLIB_SEND_FLAGS & MSG_NOSIGNAL) != 0,
              "cpp-httplib's writes must pass MSG_NOSIGNAL: link praenora_httplib");
#endif

Error unreachable(const ListenAddress& address, const std::string& why) {
    return Error(ErrorCode::IO_ERROR,
                 "the control API at " + address.to_string() + " cannot be reached: " + why);
}

} // namespace

ClientAnswer get(const ListenAddress& address, const std::string& path,
                 const std::optional<std::string>& token) {
    std::unique_ptr<httplib::Client> client;
    if (address.family == ListenAddress::Family::unix_socket) {
#ifdef _WIN32
        throw Error(ErrorCode::UNSUPPORTED, "listen address '" + address.to_string() +
                                                "': Unix sockets are not supported on Windows");
#else
        // cpp-httplib takes a path that starts with '@' for Linux's abstract namespace: such a
        // relative path is given as ./@...
        std::string socket_path = address.path;
        if (!socket_path.empty() && socket_path.front() == '@') {
            socket_path = "./" + socket_path;
        }
        client = std::make_unique<httplib::Client>(socket_path, 80);
        client->set_address_family(AF_UNIX);
#endif
    } else {
        const std::string host = address.host();
        client = std::make_unique<httplib::Client>(host, address.port);
        // The host is an IP address: handed over as one (AI_NUMERICHOST), never resolved. The Host
        // header is the listener's address, in brackets for IPv6.
        client->set_direct_hostname_addr_map({{host, host}});
    }
    client->set_connection_timeout(kClientConnectSeconds, 0);
    client->set_read_timeout(kClientIoSeconds, 0);
    client->set_write_timeout(kClientIoSeconds, 0);
    client->set_keep_alive(false);
#if defined(SO_NOSIGPIPE)
    // A client has no socket options of its own to keep (cpp-httplib's default for a client is
    // none; SO_REUSEADDR and SO_REUSEPORT are a server's), on a Unix socket as over TCP.
    client->set_socket_options(set_no_sigpipe);
#endif
    httplib::Headers headers;
    if (token) {
        headers.emplace("Authorization", "Bearer " + *token);
    }
    httplib::Result result;
    try {
        result = client->Get(path, headers);
    } catch (const std::exception& error) {
        throw unreachable(address, error.what());
    }
    if (!result) {
        throw unreachable(address, httplib::to_string(result.error()));
    }
    ClientAnswer answer;
    answer.status = result->status;
    answer.content_type = result->get_header_value("Content-Type");
    answer.body = result->body;
    return answer;
}

Error answer_error(const ListenAddress& address, const ClientAnswer& answer) {
    const Error none(ErrorCode::IO_ERROR,
                     "the control API at " + address.to_string() + " answered HTTP status " +
                         std::to_string(answer.status) + " without an error document");
    const json document = json::parse(answer.body, nullptr, false);
    if (!document.is_object()) {
        return none;
    }
    try {
        const std::string name = document.value("name", std::string());
        const std::optional<ErrorCode> code = error_code_from_name(name);
        if (name.empty() || !document.contains("message")) {
            return none;
        }
        SourceLocation location;
        if (const auto found = document.find("location");
            found != document.end() && found->is_object()) {
            location.file = found->value("file", std::string());
            location.line = found->value("line", std::int64_t{0});
            location.column = found->value("column", std::int64_t{0});
            location.pointer = found->value("pointer", std::string());
        }
        std::string details = "{}";
        if (const auto found = document.find("details"); found != document.end()) {
            details = found->dump(-1, ' ', false, json::error_handler_t::replace);
        }
        return Error(code ? *code : ErrorCode::UNKNOWN, document.at("message").get<std::string>(),
                     location, details);
    } catch (const json::exception&) {
        return none; // a field of another type
    }
}

} // namespace praenora::server
