#include "address.hpp"

#include <praenora/runtime/error.hpp>

#include <cstdio>
#include <vector>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace praenora::server {

namespace {

constexpr std::string_view kUnixPrefix = "unix:";

Error malformed(std::string_view text, const std::string& why) {
    return Error(ErrorCode::INVALID_ARGUMENT,
                 "listen address '" + std::string(text) + "': " + why +
                     "; give unix:<path>, <IPv4>:<port> or [<IPv6>]:<port>");
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// The value of a hexadecimal digit, or -1.
int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// A port: one to five decimal digits, at most 65535.
std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty() || text.size() > 5) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (!is_digit(c)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// The 16-bit groups of `text`, groups of one to four hexadecimal digits separated by ':', the
// last of which may be an IPv4 address (two groups) when `ipv4_last`; nullopt for anything else.
// An empty text has no groups.
std::optional<std::vector<std::uint16_t>> parse_groups(std::string_view text, bool ipv4_last) {
    std::vector<std::uint16_t> groups;
    if (text.empty()) {
        return groups;
    }
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find(':', begin);
        const std::string_view part = text.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (part.find('.') != std::string_view::npos) {
            const auto ipv4 = parse_ipv4(part);
            if (!ipv4 || !ipv4_last || end != std::string_view::npos) {
                return std::nullopt;
            }
            groups.push_back(static_cast<std::uint16_t>(((*ipv4)[0] << 8) | (*ipv4)[1]));
            groups.push_back(static_cast<std::uint16_t>(((*ipv4)[2] << 8) | (*ipv4)[3]));
            return groups;
        }
        if (part.empty() || part.size() > 4) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char c : part) {
            const int digit = hex_value(c);
            if (digit < 0) {
                return std::nullopt;
            }
            value = value * 16 + static_cast<std::uint32_t>(digit);
        }
        groups.push_back(static_cast<std::uint16_t>(value));
        if (end == std::string_view::npos) {
            return groups;
        }
        begin = end + 1;
    }
}

// Whether `bytes` is an IPv4-mapped IPv6 address (::ffff:0:0/96).
bool is_ipv4_mapped(const std::array<std::uint8_t, 16>& bytes) {
    for (std::size_t i = 0; i < 10; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return bytes[10] == 0xff && bytes[11] == 0xff;
}

std::string ipv4_text(const std::uint8_t* bytes) {
    return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
           std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

// The text of an IPv6 address in the form of RFC 5952.
std::string ipv6_text(const std::array<std::uint8_t, 16>& bytes) {
    std::array<std::uint16_t, 8> groups{};
    for (std::size_t i = 0; i < 8; ++i) {
        groups[i] = static_cast<std::uint16_t>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    }
    // IPv4-mapped addresses (::ffff:0:0/96) end in their IPv4 address.
    const bool mapped = groups[0] == 0 && groups[1] == 0 && groups[2] == 0 && groups[3] == 0 &&
                        groups[4] == 0 && groups[5] == 0xffff;
    const std::size_t count = mapped ? 6 : 8;
    // The longest run of at least two zero groups, the first of equal ones, becomes "::".
    std::size_t best_start = count, best_length = 0;
    for (std::size_t i = 0; i < count;) {
        if (groups[i] != 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < count && groups[j] == 0) {
            ++j;
        }
        if (j - i > best_length) {
            best_start = i;
            best_length = j - i;
        }
        i = j;
    }
    if (best_length < 2) {
        best_start = count;
        best_length = 0;
    }
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        if (i == best_start) {
            text += "::";
            i += best_length - 1;
            continue;
        }
        if (!text.empty() && text.back() != ':') {
            text += ':';
        }
        char group[8];
        std::snprintf(group, sizeof group, "%x", static_cast<unsigned>(groups[i]));
        text += group;
    }
    if (mapped) {
        if (!text.empty() && text.back() != ':') {
            text += ':';
        }
        text += ipv4_text(bytes.data() + 12);
    }
    return text;
}

} // namespace

bool ListenAddress::is_loopback() const noexcept {
    switch (family) {
    case Family::unix_socket:
        return false;
    case Family::ipv4:
        return bytes[0] == 127;
    case Family::ipv6: {
        bool zero_prefix = true;
        for (std::size_t i = 0; i < 10; ++i) {
            zero_prefix = zero_prefix && bytes[i] == 0;
        }
        return zero_prefix && bytes[10] == 0 && bytes[11] == 0 && bytes[12] == 0 &&
               bytes[13] == 0 && bytes[14] == 0 && bytes[15] == 1; // ::1
    }
    }
    return false;
}

std::string ListenAddress::host() const {
    switch (family) {
    case Family::unix_socket:
        return {};
    case Family::ipv4:
        return ipv4_text(bytes.data());
    case Family::ipv6:
        return ipv6_text(bytes);
    }
    return {};
}

std::string ListenAddress::to_string() const {
    switch (family) {
    case Family::unix_socket:
        return std::string(kUnixPrefix) + path;
    case Family::ipv4:
        return host() + ":" + std::to_string(port);
    case Family::ipv6:
        return "[" + host() + "]:" + std::to_string(port);
    }
    return {};
}

bool operator==(const ListenAddress& a, const ListenAddress& b) noexcept {
    if (a.family != b.family) {
        return false;
    }
    if (a.family == ListenAddress::Family::unix_socket) {
        return a.path == b.path;
    }
    return a.bytes == b.bytes && a.port == b.port;
}

ListenAddress parse_listen_address(std::string_view text) {
    ListenAddress address;
    if (text.substr(0, kUnixPrefix.size()) == kUnixPrefix) {
#ifdef _WIN32
        throw Error(ErrorCode::UNSUPPORTED, "listen address '" + std::string(text) +
                                                "': Unix sockets are not supported on Windows; "
                                                "give <IPv4>:<port> or [<IPv6>]:<port>");
#else
        address.family = ListenAddress::Family::unix_socket;
        address.path = std::string(text.substr(kUnixPrefix.size()));
        if (address.path.empty()) {
            throw malformed(text, "the socket's path is empty");
        }
        if (address.path.find('\0') != std::string::npos) {
            throw malformed(text, "the socket's path holds a NUL byte");
        }
        if (address.path.size() > unix_socket_path_limit()) {
            throw malformed(text, "the socket's path is " + std::to_string(address.path.size()) +
                                      " bytes long, and a Unix socket's path holds at most " +
                                      std::to_string(unix_socket_path_limit()) +
                                      " bytes on this platform");
        }
        return address;
#endif
    }
    std::string_view host;
    std::string_view port;
    if (!text.empty() && text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) {
            throw malformed(text, "the IPv6 address has no closing ']'");
        }
        host = text.substr(1, close - 1);
        const std::string_view rest = text.substr(close + 1);
        if (rest.empty() || rest.front() != ':') {
            throw malformed(text, "a port must follow the IPv6 address, as [<IPv6>]:<port>");
        }
        port = rest.substr(1);
        const auto bytes = parse_ipv6(host);
        if (!bytes) {
            throw malformed(text, "'" + std::string(host) + "' is not an IPv6 address");
        }
        address.family = ListenAddress::Family::ipv6;
        address.bytes = *bytes;
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            throw malformed(text, "a port must follow the address, as <IPv4>:<port>");
        }
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
        if (host.find(':') != std::string_view::npos) {
            throw malformed(text, "an IPv6 address goes in brackets, as [<IPv6>]:<port>");
        }
        const auto bytes = parse_ipv4(host);
        if (!bytes) {
            throw malformed(text, "'" + std::string(host) +
                                      "' is not an IPv4 address in dotted decimal (host names are "
                                      "refused: an address is never resolved)");
        }
        address.family = ListenAddress::Family::ipv4;
        for (std::size_t i = 0; i < 4; ++i) {
            address.bytes[i] = (*bytes)[i];
        }
    }
    const auto number = parse_port(port);
    if (!number) {
        throw malformed(text, "the port must be a decimal number from 0 to 65535");
    }
    address.port = *number;
    if (address.family == ListenAddress::Family::ipv6 && is_ipv4_mapped(address.bytes)) {
        // An IPv6 listener takes IPv6 connections only (IPV6_V6ONLY), and the system binds no
        // IPv4-mapped address to it: the IPv4 address is the one to give.
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "listen address '" + std::string(text) + "': '" + std::string(host) +
                        "' is an IPv4-mapped IPv6 address, which a listener cannot bind; give "
                        "the IPv4 address instead, as " +
                        ipv4_text(address.bytes.data() + 12) + ":" + std::to_string(address.port));
    }
    return address;
}

std::optional<std::array<std::uint8_t, 4>> parse_ipv4(std::string_view text) {
    std::array<std::uint8_t, 4> bytes{};
    std::size_t begin = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t end = i < 3 ? text.find('.', begin) : text.size();
        if (end == std::string_view::npos || end == begin || end - begin > 3) {
            return std::nullopt;
        }
        const std::string_view part = text.substr(begin, end - begin);
        if (part.size() > 1 && part.front() == '0') {
            return std::nullopt; // no leading zeros, which some read as octal
        }
        unsigned value = 0;
        for (const char c : part) {
            if (!is_digit(c)) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value > 255) {
            return std::nullopt;
        }
        bytes[i] = static_cast<std::uint8_t>(value);
        begin = end + 1;
    }
    return bytes;
}

std::optional<std::array<std::uint8_t, 16>> parse_ipv6(std::string_view text) {
    const std::size_t gap = text.find("::");
    std::vector<std::uint16_t> head, tail;
    if (gap == std::string_view::npos) {
        const auto groups = parse_groups(text, true);
        if (!groups || groups->size() != 8) {
            return std::nullopt;
        }
        head = *groups;
    } else {
        if (text.find("::", gap + 1) != std::string_view::npos) {
            return std::nullopt; // one "::" at most (":::" included)
        }
        const auto before = parse_groups(text.substr(0, gap), false);
        const auto after = parse_groups(text.substr(gap + 2), true);
        // "::" stands for at least one group of zeros.
        if (!before || !after || before->size() + after->size() > 7) {
            return std::nullopt;
        }
        head = *before;
        tail = *after;
    }
    std::array<std::uint16_t, 8> groups{};
    for (std::size_t i = 0; i < head.size(); ++i) {
        groups[i] = head[i];
    }
    for (std::size_t i = 0; i < tail.size(); ++i) {
        groups[8 - tail.size() + i] = tail[i];
    }
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[2 * i] = static_cast<std::uint8_t>(groups[i] >> 8);
        bytes[2 * i + 1] = static_cast<std::uint8_t>(groups[i] & 0xff);
    }
    return bytes;
}

std::size_t unix_socket_path_limit() noexcept {
#ifdef _WIN32
    return 0;
#else
    return sizeof(sockaddr_un{}.sun_path) - 1;
#endif
}

} // namespace praenora::server
