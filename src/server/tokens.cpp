#include "tokens.hpp"

#include <frontend.hpp>

#include <praenora/runtime/error.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace praenora::server {

namespace {

namespace fs = std::filesystem;

Error token_file_error(const std::string& path, const std::string& why) {
    return Error(ErrorCode::INVALID_ARGUMENT, "token file '" + path + "': " + why);
}

bool is_token_character(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=';
}

bool is_white_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// A host name or an IPv4 address: letters, digits, '-' and '.', neither first nor last a '.'
// or '-'.
bool is_host_name(std::string_view name) {
    if (name.empty() || name.front() == '.' || name.front() == '-' || name.back() == '.' ||
        name.back() == '-') {
        return false;
    }
    for (const char c : name) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

// The address of a bracketed IPv6 literal, "[2001:db8::1]", or nullopt.
std::optional<std::array<std::uint8_t, 16>> bracketed_ipv6(std::string_view name) {
    if (name.size() < 2 || name.front() != '[' || name.back() != ']') {
        return std::nullopt;
    }
    return parse_ipv6(name.substr(1, name.size() - 2));
}

} // namespace

std::string read_token_file(const std::string& path) {
    const fs::path file = frontend::path_from_utf8(path);
    std::error_code ec;
    const fs::file_status status = fs::status(file, ec);
    if (ec || !fs::exists(status)) {
        throw token_file_error(path, "it cannot be read" + (ec ? ": " + ec.message() : ""));
    }
    if (!fs::is_regular_file(status)) {
        throw token_file_error(path, "it is not a regular file");
    }
#ifndef _WIN32
    // The file is the credential: nobody but its owner may read or change it.
    const fs::perms shared = fs::perms::group_all | fs::perms::others_all;
    if ((status.permissions() & shared) != fs::perms::none) {
        throw token_file_error(path, "group or others may read or write it; make it readable "
                                     "by its owner only (chmod 600)");
    }
#endif
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || (!std::getline(in, line) && !in.eof())) {
        throw token_file_error(path, "it cannot be read");
    }
    while (!line.empty() && is_white_space(line.back())) {
        line.pop_back();
    }
    if (line.size() < kMinTokenLength) {
        throw token_file_error(path, "its first line must hold a token of at least " +
                                         std::to_string(kMinTokenLength) + " characters");
    }
    for (const char c : line) {
        if (!is_token_character(c)) {
            throw token_file_error(path, "its token may hold only the characters "
                                         "A-Z a-z 0-9 - . _ ~ + / =");
        }
    }
    return line;
}

Tokens load_tokens(const std::optional<std::string>& token_file,
                   const std::optional<std::string>& read_token_file_path) {
    Tokens tokens;
    if (token_file) {
        tokens.control = read_token_file(*token_file);
    }
    if (read_token_file_path) {
        tokens.read = read_token_file(*read_token_file_path);
    }
    if (!tokens.control.empty() && constant_time_equal(tokens.control, tokens.read)) {
        throw token_file_error(*read_token_file_path,
                               "its token is the one of --token-file '" + *token_file +
                                   "'; the read token and the control token must differ");
    }
    return tokens;
}

void check_credentials(const ListenAddress& address, const Tokens& tokens) {
    if (address.kind() == ListenerKind::unix_socket) {
        if (!tokens.empty()) {
            throw Error(ErrorCode::INVALID_ARGUMENT,
                        "--token-file and --read-token-file are refused with a Unix socket "
                        "listener (" +
                            address.to_string() + "): the socket's mode is the credential");
        }
        return;
    }
    if (tokens.empty() && !address.is_loopback()) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "a TCP listener on " + address.to_string() +
                        ", which is not a loopback address, needs --token-file or "
                        "--read-token-file");
    }
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    const std::size_t length = a.size() > b.size() ? a.size() : b.size();
    unsigned difference = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < length; ++i) {
        const auto x = static_cast<unsigned char>(i < a.size() ? a[i] : 0);
        const auto y = static_cast<unsigned char>(i < b.size() ? b[i] : 0);
        difference |= static_cast<unsigned>(x ^ y);
    }
    return difference == 0;
}

ScopeDecision decide_scopes(ListenerKind kind, const Tokens& tokens,
                            const std::vector<std::string>& authorization) {
    ScopeDecision decision;
    if (kind == ListenerKind::unix_socket) {
        decision.authenticated = true;
        decision.scopes = {true, true};
        return decision;
    }
    if (tokens.empty()) {
        decision.authenticated = true;
        decision.scopes.read = true;
        return decision;
    }
    if (authorization.size() != 1) {
        return decision;
    }
    // "Bearer" in any case, one or more spaces, the token, optional trailing white space.
    std::string_view value = authorization.front();
    constexpr std::string_view kScheme = "bearer";
    if (value.size() <= kScheme.size() || !equal_ignoring_case(value.substr(0, 6), kScheme) ||
        value[kScheme.size()] != ' ') {
        return decision;
    }
    value.remove_prefix(kScheme.size());
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_white_space(value.back())) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return decision;
    }
    // Both comparisons, every time: the time taken does not tell which token matched.
    const bool control = !tokens.control.empty() && constant_time_equal(value, tokens.control);
    const bool read = !tokens.read.empty() && constant_time_equal(value, tokens.read);
    if (control) {
        decision.authenticated = true;
        decision.scopes = {true, true};
    } else if (read) {
        decision.authenticated = true;
        decision.scopes.read = true;
    }
    return decision;
}

void check_allow_host(std::string_view name) {
    if (!is_host_name(name) && !bracketed_ipv6(name)) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "--allow-host '" + std::string(name) +
                        "' is not a host name, an IPv4 address or an IPv6 address in brackets "
                        "(such as [2001:db8::1]), without a port");
    }
}

bool host_allowed(const std::vector<std::string>& host, const ListenAddress& listener,
                  const std::vector<std::string>& allow_hosts) {
    if (host.size() != 1) {
        return false;
    }
    const std::string_view value = host.front();
    std::string_view name;
    std::string_view port;
    bool has_port = false;
    bool bracketed = false;
    if (!value.empty() && value.front() == '[') {
        const std::size_t close = value.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        name = value.substr(1, close - 1);
        const std::string_view rest = value.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return false;
            }
            port = rest.substr(1);
            has_port = true;
        }
        bracketed = true;
    } else {
        const std::size_t colon = value.find(':');
        name = value.substr(0, colon);
        if (colon != std::string_view::npos) {
            port = value.substr(colon + 1);
            has_port = true;
        }
    }
    if (has_port) {
        std::uint32_t number = 0;
        if (port.empty() || port.size() > 5) {
            return false;
        }
        for (const char c : port) {
            if (c < '0' || c > '9') {
                return false;
            }
            number = number * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (number != listener.port) {
            return false;
        }
    }
    if (bracketed) {
        const auto bytes = parse_ipv6(name);
        if (!bytes) {
            return false;
        }
        if (listener.family == ListenAddress::Family::ipv6 && *bytes == listener.bytes) {
            return true;
        }
        // Compared as addresses: [2001:DB8:0::1] is [2001:db8::1].
        for (const std::string& allowed : allow_hosts) {
            if (bracketed_ipv6(allowed) == bytes) {
                return true;
            }
        }
        return false;
    }
    if (const auto bytes = parse_ipv4(name)) {
        if (listener.family == ListenAddress::Family::ipv4 && (*bytes)[0] == listener.bytes[0] &&
            (*bytes)[1] == listener.bytes[1] && (*bytes)[2] == listener.bytes[2] &&
            (*bytes)[3] == listener.bytes[3]) {
            return true;
        }
    } else if (equal_ignoring_case(name, "localhost")) {
        return listener.is_loopback();
    }
    for (const std::string& allowed : allow_hosts) {
        if (equal_ignoring_case(name, allowed)) {
            return true;
        }
    }
    return false;
}

} // namespace praenora::server
