// The only source of the runtime that includes httplib.h, and first: on Windows it includes
// winsock2.h, which must come before windows.h. cpp-httplib is used header-only, without TLS,
// compression or its non-blocking name resolution; the only CPPHTTPLIB_* definition is
// CPPHTTPLIB_SEND_FLAGS, MSG_NOSIGNAL on POSIX systems other than macOS (praenora_httplib).
#include <httplib.h>

#include "observability/otlp/http_client.hpp"

#include <algorithm>
#include <ctime>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace praenora::detail {

namespace {

// No write of a request raises SIGPIPE, whatever this process's disposition of it (plain
// `praenora run`, an application that embeds the runtime): a collector that closes or resets the
// connection while the body is written fails the request, never the process. On macOS each
// socket is set never to raise it (SO_NOSIGPIPE, set_no_sigpipe()); on other POSIX systems every
// send() passes MSG_NOSIGNAL, which praenora_httplib defines as CPPHTTPLIB_SEND_FLAGS. Windows has
// no SIGPIPE.
#if defined(SO_NOSIGPIPE)
void set_no_sigpipe(socket_t descriptor) {
    static_cast<void>(httplib::set_socket_opt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, 1));
}
#elif !defined(_WIN32)
static_assert((CPPHTTPLIB_SEND_FLAGS & MSG_NOSIGNAL) != 0,
              "cpp-httplib's writes must pass MSG_NOSIGNAL: link praenora_httplib");
#endif

class HttpTransport final : public OtlpTransport {
public:
    HttpResponse post(const HttpEndpoint& endpoint, const HttpHeaders& headers,
                      const std::string& body, std::chrono::milliseconds timeout) override {
        HttpResponse response;
        try {
            httplib::Client client(endpoint.host, endpoint.port);
            const auto milliseconds = std::max<std::int64_t>(timeout.count(), 1);
            const auto seconds = static_cast<time_t>(milliseconds / 1000);
            const auto microseconds = static_cast<time_t>((milliseconds % 1000) * 1000);
            client.set_connection_timeout(seconds, microseconds);
            client.set_read_timeout(seconds, microseconds);
            client.set_write_timeout(seconds, microseconds);
            // Reading and writing end with the request's whole time, not each wait's.
            client.set_max_timeout(static_cast<time_t>(milliseconds));
            client.set_keep_alive(false);
#if defined(SO_NOSIGPIPE)
            // A client has no socket options of its own to keep (cpp-httplib's default for a
            // client is none; SO_REUSEADDR and SO_REUSEPORT are a server's).
            client.set_socket_options(set_no_sigpipe);
#endif
            httplib::Headers sent;
            for (const auto& [name, value] : headers) {
                sent.emplace(name, value);
            }
            const httplib::Result result =
                client.Post(endpoint.path, sent, body, "application/json");
            if (!result) {
                response.error = httplib::to_string(result.error());
                return response;
            }
            response.status = result->status;
            if (result->has_header("Retry-After")) {
                response.retry_after_seconds =
                    parse_retry_after(result->get_header_value("Retry-After"));
            }
        } catch (const std::exception& e) {
            response.status = 0;
            response.error = e.what();
        } catch (...) {
            response.status = 0;
            response.error = "the HTTP client failed";
        }
        return response;
    }
};

std::mutex factory_mutex;
std::function<std::unique_ptr<OtlpTransport>()> factory_for_testing; // guarded by factory_mutex

} // namespace

std::optional<std::int64_t> parse_retry_after(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return std::nullopt;
    }
    std::int64_t seconds = 0;
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const int digit = c - '0';
        seconds = seconds > (kMax - digit) / 10 ? kMax : seconds * 10 + digit;
    }
    return seconds;
}

std::unique_ptr<OtlpTransport> make_http_transport() { return std::make_unique<HttpTransport>(); }

std::unique_ptr<OtlpTransport> make_otlp_transport() {
    std::function<std::unique_ptr<OtlpTransport>()> factory;
    {
        const std::lock_guard<std::mutex> lock(factory_mutex);
        factory = factory_for_testing;
    }
    return factory ? factory() : make_http_transport();
}

void set_otlp_transport_factory_for_testing(
    std::function<std::unique_ptr<OtlpTransport>()> factory) {
    const std::lock_guard<std::mutex> lock(factory_mutex);
    factory_for_testing = std::move(factory);
}

} // namespace praenora::detail
