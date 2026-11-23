// The listener addresses of the control API (src/server/address.hpp): unix:<path> (POSIX only),
// <IPv4>:<port> and [<IPv6>]:<port>, the text forms of IP addresses, IPv4-mapped addresses
// refused, loopback addresses, and the clock that writes the server's instants
// (src/server/clock.hpp).
#include "address.hpp"
#include "clock.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

using praenora::Error;
using praenora::ErrorCode;
using praenora::server::ListenAddress;
using praenora::server::ListenerKind;
using praenora::server::parse_ipv4;
using praenora::server::parse_ipv6;
using praenora::server::parse_listen_address;

namespace {

// The error that parsing `text` throws.
Error refusal(const std::string& text) {
    try {
        parse_listen_address(text);
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "'" << text << "' was accepted";
    return Error(ErrorCode::OK, "accepted");
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST(ListenAddress, ParsesIpv4WithPort) {
    const ListenAddress address = parse_listen_address("127.0.0.1:8080");
    EXPECT_EQ(address.family, ListenAddress::Family::ipv4);
    EXPECT_EQ(address.kind(), ListenerKind::tcp);
    EXPECT_EQ(address.port, 8080);
    EXPECT_EQ(address.host(), "127.0.0.1");
    EXPECT_EQ(address.to_string(), "127.0.0.1:8080");
    EXPECT_TRUE(address.is_loopback());
    EXPECT_EQ(parse_listen_address("192.0.2.10:65535").to_string(), "192.0.2.10:65535");
    EXPECT_FALSE(parse_listen_address("192.0.2.10:1").is_loopback());
}

TEST(ListenAddress, ParsesIpv6InBracketsWithPortZero) {
    const ListenAddress address = parse_listen_address("[::1]:0");
    EXPECT_EQ(address.family, ListenAddress::Family::ipv6);
    EXPECT_EQ(address.kind(), ListenerKind::tcp);
    EXPECT_EQ(address.port, 0);
    EXPECT_EQ(address.host(), "::1");
    EXPECT_EQ(address.to_string(), "[::1]:0");
    EXPECT_TRUE(address.is_loopback());
    // Written in the form of RFC 5952: lower case, no leading zeros, the longest run of zero
    // groups (the first of equal ones) as "::", a single zero group kept, IPv4-mapped addresses
    // with their IPv4 part (an address that parse_listen_address() refuses, made here directly).
    EXPECT_EQ(parse_listen_address("[2001:DB8:0:0:0:0:0:1]:443").to_string(), "[2001:db8::1]:443");
    EXPECT_EQ(parse_listen_address("[1:0:0:2:0:0:0:3]:1").to_string(), "[1:0:0:2::3]:1");
    EXPECT_EQ(parse_listen_address("[1:0:0:2:0:0:3:4]:1").to_string(), "[1::2:0:0:3:4]:1");
    EXPECT_EQ(parse_listen_address("[2001:db8:0:1:1:1:1:1]:1").to_string(),
              "[2001:db8:0:1:1:1:1:1]:1");
    ListenAddress mapped;
    mapped.family = ListenAddress::Family::ipv6;
    mapped.bytes = parse_ipv6("0:0:0:0:0:ffff:102:304").value();
    mapped.port = 1;
    EXPECT_EQ(mapped.to_string(), "[::ffff:1.2.3.4]:1");
    EXPECT_EQ(parse_listen_address("[::]:1").to_string(), "[::]:1");
    EXPECT_EQ(parse_listen_address("[0000:0000::0001]:9"), parse_listen_address("[::1]:9"));
}

TEST(ListenAddress, ParsesUnixSocketOnPosixOnly) {
#ifdef _WIN32
    const Error error = refusal("unix:C:/praenora.sock");
    EXPECT_EQ(error.code(), ErrorCode::UNSUPPORTED) << error.to_json();
    EXPECT_TRUE(contains(error.what(), "Windows")) << error.what();
#else
    const ListenAddress address = parse_listen_address("unix:/tmp/praenora t\xC3\xABst.sock");
    EXPECT_EQ(address.family, ListenAddress::Family::unix_socket);
    EXPECT_EQ(address.kind(), ListenerKind::unix_socket);
    EXPECT_EQ(address.path, "/tmp/praenora t\xC3\xABst.sock");
    EXPECT_EQ(address.to_string(), "unix:/tmp/praenora t\xC3\xABst.sock");
    EXPECT_EQ(address.host(), "");
    EXPECT_FALSE(address.is_loopback());
    EXPECT_EQ(parse_listen_address("unix:relative.sock").path, "relative.sock");
#endif
}

TEST(ListenAddress, RefusesUnixSocketPathsTooLongOrEmpty) {
#ifdef _WIN32
    GTEST_SKIP() << "Unix sockets are POSIX only";
#else
    const std::size_t limit = praenora::server::unix_socket_path_limit();
    ASSERT_GE(limit, 90u);
    EXPECT_EQ(parse_listen_address("unix:" + std::string(limit, 'x')).path.size(), limit);
    const Error long_path = refusal("unix:" + std::string(limit + 1, 'x'));
    EXPECT_EQ(long_path.code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_TRUE(contains(long_path.what(), std::to_string(limit))) << long_path.what();
    EXPECT_EQ(refusal("unix:").code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(refusal(std::string("unix:a\0b", 8)).code(), ErrorCode::INVALID_ARGUMENT);
#endif
}

TEST(ListenAddress, RefusesHostNames) {
    for (const std::string text : {"localhost:8080", "example.org:80", "LOCALHOST:0", "host:1"}) {
        SCOPED_TRACE(text);
        const Error error = refusal(text);
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
        EXPECT_TRUE(contains(error.what(), "host names are refused")) << error.what();
        EXPECT_TRUE(contains(error.what(), text)) << error.what();
    }
}

TEST(ListenAddress, RefusesPortsOutOfRangeOrMissing) {
    for (const std::string text :
         {"127.0.0.1:65536", "127.0.0.1:", "127.0.0.1", "127.0.0.1:-1", "127.0.0.1:+80",
          "127.0.0.1:8a", "[::1]:65536", "[::1]:", "[::1]", "127.0.0.1:999999", "127.0.0.1: 80"}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(refusal(text).code(), ErrorCode::INVALID_ARGUMENT);
    }
    EXPECT_EQ(parse_listen_address("127.0.0.1:65535").port, 65535);
    EXPECT_EQ(parse_listen_address("[::1]:65535").port, 65535);
}

TEST(ListenAddress, RefusesMalformedAddresses) {
    for (const std::string text :
         {"", ":80", "::1:80", "[::1]80", "[::1:80", "[fe80::1%en0]:80", "256.0.0.1:80",
          "01.2.3.4:80", "1.2.3:80", "1.2.3.4.5:80", "[1.2.3.4]:80", "[]:80", "[::1]]:80",
          "tcp:127.0.0.1:80", " 127.0.0.1:80"}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(refusal(text).code(), ErrorCode::INVALID_ARGUMENT);
    }
    const Error unbracketed = refusal("::1:80");
    EXPECT_TRUE(contains(unbracketed.what(), "brackets")) << unbracketed.what();
}

// An IPv4-mapped IPv6 address (::ffff:0:0/96) cannot be bound by a listener, which takes IPv6
// only (IPV6_V6ONLY): refused, naming the IPv4 form to give instead, in every text form.
TEST(ListenAddress, RefusesIpv4MappedAddressesNamingTheIpv4Form) {
    const Error loopback = refusal("[::ffff:127.0.0.1]:8080");
    EXPECT_EQ(loopback.code(), ErrorCode::INVALID_ARGUMENT) << loopback.to_json();
    EXPECT_TRUE(contains(loopback.what(), "[::ffff:127.0.0.1]:8080")) << loopback.what();
    EXPECT_TRUE(contains(loopback.what(), "IPv4-mapped")) << loopback.what();
    EXPECT_TRUE(contains(loopback.what(), "as 127.0.0.1:8080")) << loopback.what();
    for (const std::string text : {"[::FFFF:7f00:1]:0", "[0:0:0:0:0:ffff:127.0.0.1]:0",
                                   "[::ffff:0:0]:9", "[::ffff:192.0.2.10]:443"}) {
        SCOPED_TRACE(text);
        const Error error = refusal(text);
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
        EXPECT_TRUE(contains(error.what(), "IPv4-mapped")) << error.what();
    }
    EXPECT_TRUE(contains(refusal("[::FFFF:7f00:1]:0").what(), "as 127.0.0.1:0"));
    EXPECT_TRUE(contains(refusal("[::ffff:192.0.2.10]:443").what(), "as 192.0.2.10:443"));
    // A bad port is reported as one first; neighbours of the mapped range are IPv6 addresses.
    EXPECT_TRUE(contains(refusal("[::ffff:127.0.0.1]:65536").what(), "the port must be"));
    EXPECT_EQ(parse_listen_address("[::fffe:7f00:1]:1").family, ListenAddress::Family::ipv6);
    EXPECT_EQ(parse_listen_address("[::1:ffff:7f00:1]:1").family, ListenAddress::Family::ipv6);
    EXPECT_EQ(parse_listen_address("[::7f00:1]:1").family, ListenAddress::Family::ipv6);
}

TEST(ListenAddress, KnowsLoopbackAddresses) {
    EXPECT_TRUE(parse_listen_address("127.255.0.1:1").is_loopback());
    EXPECT_FALSE(parse_listen_address("0.0.0.0:1").is_loopback());
    EXPECT_FALSE(parse_listen_address("[::]:1").is_loopback());
    EXPECT_FALSE(parse_listen_address("10.0.0.1:1").is_loopback());
    EXPECT_FALSE(parse_listen_address("[::2]:1").is_loopback());
    EXPECT_FALSE(parse_listen_address("128.0.0.1:1").is_loopback());
}

TEST(ListenAddress, ParsesIpTextForms) {
    EXPECT_EQ(parse_ipv4("1.2.3.4"), (std::array<std::uint8_t, 4>{1, 2, 3, 4}));
    EXPECT_EQ(parse_ipv4("0.0.0.0"), (std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    EXPECT_EQ(parse_ipv4("255.255.255.255"), (std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    for (const char* text : {"", "1.2.3", "1.2.3.4.", ".1.2.3", "1..2.3", "1.2.3.256", "1.2.3.04",
                             "1.2.3.a", "1.2.3.1234", "+1.2.3.4"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(parse_ipv4(text).has_value());
    }

    std::array<std::uint8_t, 16> loopback{};
    loopback[15] = 1;
    EXPECT_EQ(parse_ipv6("::1"), loopback);
    EXPECT_EQ(parse_ipv6("0:0:0:0:0:0:0:1"), loopback);
    EXPECT_EQ(parse_ipv6("::"), (std::array<std::uint8_t, 16>{}));
    std::array<std::uint8_t, 16> mapped{};
    mapped[10] = 0xff;
    mapped[11] = 0xff;
    mapped[12] = 1;
    mapped[13] = 2;
    mapped[14] = 3;
    mapped[15] = 4;
    EXPECT_EQ(parse_ipv6("::ffff:1.2.3.4"), mapped);
    EXPECT_EQ(parse_ipv6("::FFFF:0102:0304"), mapped);
    for (const char* text : {"1::", "1:2:3:4:5:6:7::", "::1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8",
                             "1:2:3:4:5:6:1.2.3.4", "abcd::ef01"}) {
        SCOPED_TRACE(text);
        EXPECT_TRUE(parse_ipv6(text).has_value());
    }
    for (const char* text : {"", ":", ":::", "1:2:3:4:5:6:7:8:9", "1::2::3", "12345::", "g::",
                             "1:2:3:4:5:6:7:8::", "::1.2.3.4:1", "1.2.3.4::", ":1::", "1:", ":1",
                             "fe80::1%en0", "1:2:3:4:5:6:7", "::ffff:1.2.3"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(parse_ipv6(text).has_value());
    }
}

TEST(ServerClock, FormatsRfc3339WithNineFractionalDigits) {
    using praenora::server::format_rfc3339_utc;
    EXPECT_EQ(format_rfc3339_utc(1700000000123456789LL), "2023-11-14T22:13:20.123456789Z");
    EXPECT_EQ(format_rfc3339_utc(0), "1970-01-01T00:00:00.000000000Z");
    EXPECT_EQ(format_rfc3339_utc(-1), "1969-12-31T23:59:59.999999999Z");
    EXPECT_EQ(format_rfc3339_utc(1709164800000000000LL), "2024-02-29T00:00:00.000000000Z");
    EXPECT_EQ(format_rfc3339_utc(951782400000000000LL), "2000-02-29T00:00:00.000000000Z");
    EXPECT_EQ(format_rfc3339_utc(4102444799999999999LL), "2099-12-31T23:59:59.999999999Z");
    // The clock reads the system's wall clock: some time after this code was written.
    EXPECT_GT(praenora::server::now_unix_ns(), 1790000000000000000LL);
}
