// The addresses a control API listener takes (--listen): unix:<path> (POSIX only),
// <IPv4>:<port> and [<IPv6>]:<port>, port 0 letting the system choose. Host names are refused:
// an address is never resolved.
#ifndef PRAENORA_SERVER_ADDRESS_HPP
#define PRAENORA_SERVER_ADDRESS_HPP

#include "http_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::server {

struct ListenAddress {
    enum class Family { unix_socket, ipv4, ipv6 };
    Family family = Family::ipv4;
    std::string path;                     // unix_socket: the socket's path, as given
    std::array<std::uint8_t, 16> bytes{}; // ipv4: the first 4 bytes; ipv6: all 16
    std::uint16_t port = 0;               // ipv4, ipv6: 0 lets the system choose

    ListenerKind kind() const noexcept {
        return family == Family::unix_socket ? ListenerKind::unix_socket : ListenerKind::tcp;
    }
    // 127.0.0.0/8 and ::1; never a Unix socket (nor an IPv4-mapped address, which
    // parse_listen_address() refuses).
    bool is_loopback() const noexcept;
    // The IP address as text (IPv4 dotted decimal, IPv6 in the form of RFC 5952, without
    // brackets); empty for a Unix socket.
    std::string host() const;
    // The address in the syntax parse_listen_address() takes: "unix:<path>", "127.0.0.1:8080",
    // "[::1]:8080".
    std::string to_string() const;
};

bool operator==(const ListenAddress& a, const ListenAddress& b) noexcept;
inline bool operator!=(const ListenAddress& a, const ListenAddress& b) noexcept {
    return !(a == b);
}

// Parses `text`. Throws praenora::Error: INVALID_ARGUMENT for anything but the three forms (a host
// name, an IPv6 address without brackets, a port that is missing or above 65535, an empty path, a
// path with a NUL byte or one longer than unix_socket_path_limit() bytes) and for an IPv4-mapped
// IPv6 address (::ffff:0:0/96, which an IPv6 listener cannot bind; the message names the IPv4
// form to give instead), naming `text`; UNSUPPORTED for unix:<path> on Windows.
ListenAddress parse_listen_address(std::string_view text);

// The four bytes of an IPv4 address in dotted decimal (four decimal parts from 0 to 255, without
// leading zeros), or nullopt.
std::optional<std::array<std::uint8_t, 4>> parse_ipv4(std::string_view text);

// The sixteen bytes of an IPv6 address in the text forms of RFC 4291 (hexadecimal groups, one
// "::", an IPv4 address in the last 32 bits), without brackets or a zone, or nullopt.
std::optional<std::array<std::uint8_t, 16>> parse_ipv6(std::string_view text);

// The longest path, in bytes, that a Unix domain socket's address holds on this platform (its
// sun_path without the final NUL); 0 on Windows.
std::size_t unix_socket_path_limit() noexcept;

} // namespace praenora::server

#endif // PRAENORA_SERVER_ADDRESS_HPP
