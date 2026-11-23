// Scopes and tokens of the control API (src/server/tokens.hpp): the rules of token files, the
// credentials a listener accepts, the scopes a request gets, the constant-time comparison and the
// Host check.
#include "address.hpp"
#include "tokens.hpp"

#include "server_fixtures.hpp"

#include "module_fixtures.hpp"
#include "temp_dir.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using praenora::Error;
using praenora::ErrorCode;
using praenora::server::check_allow_host;
using praenora::server::check_credentials;
using praenora::server::constant_time_equal;
using praenora::server::decide_scopes;
using praenora::server::host_allowed;
using praenora::server::ListenerKind;
using praenora::server::load_tokens;
using praenora::server::parse_listen_address;
using praenora::server::read_token_file;
using praenora::server::ScopeDecision;
using praenora::server::Tokens;
using praenora::test::error_of;
using praenora::test::ScratchDir;
using praenora::test::utf8;
using praenora::test::write_text_file;

namespace {

const std::string kControl = "control-token-0123456789abcdefghijklmnop";
const std::string kRead = "read-token-ABCDEFGHIJKLMNOPQRSTUVWXYZ+/=~._";

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Writes a token file that only its owner may read and write.
fs::path token_file(const fs::path& file, const std::string& text) {
    write_text_file(file, text);
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    return file;
}

// The refusal of the token file `file`: INVALID_ARGUMENT naming the file and never its token.
void expect_refused(const fs::path& file, const std::string& token, const std::string& why) {
    const Error error = error_of([&] { read_token_file(utf8(file)); });
    EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT) << error.to_json();
    EXPECT_TRUE(contains(error.what(), utf8(file))) << error.what();
    EXPECT_TRUE(contains(error.what(), why)) << error.what();
    if (!token.empty()) {
        EXPECT_FALSE(contains(error.what(), token)) << error.what();
    }
}

} // namespace

TEST(TokenFile, ReadsFirstLineWithoutTrailingWhiteSpace) {
    ScratchDir dir;
    const fs::path file = token_file(dir.path() / "token", kControl + " \t\r\nsecond line\n");
    EXPECT_EQ(read_token_file(utf8(file)), kControl);
    const fs::path bare = token_file(dir.path() / "bare", kRead);
    EXPECT_EQ(read_token_file(utf8(bare)), kRead);
}

TEST(TokenFile, RefusesGroupOrOtherAccess) {
#ifdef _WIN32
    GTEST_SKIP() << "file modes are POSIX only";
#else
    ScratchDir dir;
    const fs::path file = token_file(dir.path() / "token", kControl + "\n");
    for (const fs::perms extra : {fs::perms::group_read, fs::perms::others_read,
                                  fs::perms::group_write, fs::perms::others_write}) {
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | extra,
                        fs::perm_options::replace);
        expect_refused(file, kControl, "group or others");
    }
    // Mode 0644, the usual one.
    fs::permissions(file,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read,
                    fs::perm_options::replace);
    expect_refused(file, kControl, "group or others");
    fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace);
    EXPECT_EQ(read_token_file(utf8(file)), kControl);
#endif
}

TEST(TokenFile, RefusesTokensShorterThan32Characters) {
    ScratchDir dir;
    const std::string short_token(31, 'a');
    expect_refused(token_file(dir.path() / "short", short_token + "\n"), short_token,
                   "at least 32");
    EXPECT_EQ(read_token_file(utf8(token_file(dir.path() / "ok", std::string(32, 'a')))),
              std::string(32, 'a'));
    expect_refused(token_file(dir.path() / "empty", ""), "", "at least 32");
    expect_refused(token_file(dir.path() / "blank", "\n" + kControl), "", "at least 32");
}

TEST(TokenFile, RefusesCharactersOutsideTheTokenSet) {
    ScratchDir dir;
    for (const std::string& token :
         {std::string("abcdefghijklmnop qrstuvwxyz0123456789"), " " + kControl, "\t" + kControl,
          kControl + "\xC3\xA9", kControl + "!", kControl + "\"", kControl + ","}) {
        SCOPED_TRACE(token);
        expect_refused(token_file(dir.path() / "token", token + "\n"), "", "A-Z a-z 0-9");
    }
}

TEST(TokenFile, RefusesMissingFileOrDirectory) {
    ScratchDir dir;
    expect_refused(dir.path() / "missing", "", "cannot be read");
    expect_refused(dir.path(), "", "not a regular file");
}

TEST(TokenFile, LoadsBothTokensAndRefusesEqualOnes) {
    ScratchDir dir;
    const fs::path control = token_file(dir.path() / "control", kControl + "\n");
    const fs::path read = token_file(dir.path() / "read", kRead + "\n");
    const Tokens tokens = load_tokens(utf8(control), utf8(read));
    EXPECT_EQ(tokens.control, kControl);
    EXPECT_EQ(tokens.read, kRead);
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(load_tokens(utf8(control), std::nullopt).read, "");
    EXPECT_EQ(load_tokens(std::nullopt, utf8(read)).control, "");
    EXPECT_TRUE(load_tokens(std::nullopt, std::nullopt).empty());

    const fs::path same = token_file(dir.path() / "same", kControl + "  \n");
    const Error error = error_of([&] { load_tokens(utf8(control), utf8(same)); });
    EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT) << error.to_json();
    EXPECT_TRUE(contains(error.what(), "must differ")) << error.what();
    EXPECT_TRUE(contains(error.what(), utf8(same))) << error.what();
    EXPECT_FALSE(contains(error.what(), kControl)) << error.what();
}

TEST(Credentials, UnixSocketRefusesTokens) {
#ifdef _WIN32
    GTEST_SKIP() << "Unix sockets are POSIX only";
#else
    const auto address = parse_listen_address("unix:/tmp/praenora.sock");
    EXPECT_NO_THROW(check_credentials(address, {}));
    const Error error = error_of([&] { check_credentials(address, {kControl, ""}); });
    EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_TRUE(contains(error.what(), "Unix socket")) << error.what();
    EXPECT_EQ(error_of([&] { check_credentials(address, {"", kRead}); }).code(),
              ErrorCode::INVALID_ARGUMENT);
#endif
}

TEST(Credentials, TcpWithoutTokensListensOnLoopbackOnly) {
    EXPECT_NO_THROW(check_credentials(parse_listen_address("127.0.0.1:0"), {}));
    EXPECT_NO_THROW(check_credentials(parse_listen_address("[::1]:0"), {}));
    for (const char* text : {"0.0.0.0:0", "[::]:0", "192.0.2.10:8080"}) {
        SCOPED_TRACE(text);
        const auto address = parse_listen_address(text);
        const Error error = error_of([&] { check_credentials(address, {}); });
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
        EXPECT_TRUE(contains(error.what(), "--token-file")) << error.what();
        EXPECT_NO_THROW(check_credentials(address, {kControl, ""}));
        EXPECT_NO_THROW(check_credentials(address, {"", kRead}));
    }
}

TEST(Scopes, ConstantTimeEqualComparesWholeStrings) {
    EXPECT_TRUE(constant_time_equal(kControl, kControl));
    EXPECT_TRUE(constant_time_equal("", ""));
    EXPECT_FALSE(constant_time_equal(kControl, kRead));
    EXPECT_FALSE(constant_time_equal(kControl, kControl + "x"));
    EXPECT_FALSE(constant_time_equal(kControl + "x", kControl));
    EXPECT_FALSE(constant_time_equal(kControl, ""));
    std::string last = kControl;
    last.back() = 'q';
    EXPECT_FALSE(constant_time_equal(kControl, last));
}

TEST(Scopes, UnixSocketHasBothScopes) {
    const ScopeDecision decision = decide_scopes(ListenerKind::unix_socket, {}, {});
    EXPECT_TRUE(decision.authenticated);
    EXPECT_TRUE(decision.scopes.read);
    EXPECT_TRUE(decision.scopes.control);
}

TEST(Scopes, TcpWithoutTokensHasTheReadScopeOnly) {
    for (const std::vector<std::string>& authorization :
         {std::vector<std::string>{}, std::vector<std::string>{"Bearer " + kControl}}) {
        const ScopeDecision decision = decide_scopes(ListenerKind::tcp, {}, authorization);
        EXPECT_TRUE(decision.authenticated);
        EXPECT_TRUE(decision.scopes.read);
        EXPECT_FALSE(decision.scopes.control);
    }
}

TEST(Scopes, TcpWithTokensNeedsOneBearerToken) {
    const Tokens tokens{kControl, kRead};
    const auto decide = [&](std::vector<std::string> authorization) {
        return decide_scopes(ListenerKind::tcp, tokens, authorization);
    };
    const ScopeDecision control = decide({"Bearer " + kControl});
    EXPECT_TRUE(control.authenticated);
    EXPECT_TRUE(control.scopes.read);
    EXPECT_TRUE(control.scopes.control);
    const ScopeDecision read = decide({"bearer   " + kRead + " "});
    EXPECT_TRUE(read.authenticated);
    EXPECT_TRUE(read.scopes.read);
    EXPECT_FALSE(read.scopes.control);
    EXPECT_TRUE(decide({"BEARER " + kControl}).scopes.control);
    for (const std::vector<std::string>& refused : {std::vector<std::string>{},
                                                    {""},
                                                    {"Bearer"},
                                                    {"Bearer "},
                                                    {"Bearer unknown"},
                                                    {"Basic " + kControl},
                                                    {kControl},
                                                    {"Bearer " + kControl + "x"},
                                                    {"Bearer " + kControl, "Bearer " + kControl},
                                                    {"Bearer " + kControl + " extra"},
                                                    {"Bearer\t" + kControl}}) {
        SCOPED_TRACE(refused.empty() ? std::string("(none)") : refused.front());
        const ScopeDecision decision = decide(refused);
        EXPECT_FALSE(decision.authenticated);
        EXPECT_FALSE(decision.scopes.read);
        EXPECT_FALSE(decision.scopes.control);
    }
    // With the read token only, the control scope cannot be had over TCP.
    const Tokens read_only{"", kRead};
    EXPECT_TRUE(decide_scopes(ListenerKind::tcp, read_only, {"Bearer " + kRead}).authenticated);
    EXPECT_FALSE(decide_scopes(ListenerKind::tcp, read_only, {"Bearer "}).authenticated);
}

TEST(HostCheck, AcceptsTheListenersAddressAndPort) {
    const auto listener = parse_listen_address("127.0.0.1:8080");
    EXPECT_TRUE(host_allowed({"127.0.0.1:8080"}, listener, {}));
    EXPECT_TRUE(host_allowed({"127.0.0.1"}, listener, {}));
    EXPECT_FALSE(host_allowed({}, listener, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.1:8080", "127.0.0.1:8080"}, listener, {}));
    EXPECT_FALSE(host_allowed({""}, listener, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.2:8080"}, listener, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.1:8081"}, listener, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.1:"}, listener, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.1:80800"}, listener, {}));
    EXPECT_FALSE(host_allowed({"evil.example:8080"}, listener, {}));

    const auto six = parse_listen_address("[::1]:9000");
    EXPECT_TRUE(host_allowed({"[::1]:9000"}, six, {}));
    EXPECT_TRUE(host_allowed({"[0:0:0:0:0:0:0:1]"}, six, {}));
    EXPECT_FALSE(host_allowed({"::1"}, six, {}));
    EXPECT_FALSE(host_allowed({"[::1]:9001"}, six, {}));
    EXPECT_FALSE(host_allowed({"[::2]:9000"}, six, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.1:9000"}, six, {}));
}

TEST(HostCheck, AcceptsLocalhostOnLoopbackListenersOnly) {
    const auto loopback = parse_listen_address("127.0.0.1:8080");
    EXPECT_TRUE(host_allowed({"localhost:8080"}, loopback, {}));
    EXPECT_TRUE(host_allowed({"LocalHost"}, loopback, {}));
    EXPECT_FALSE(host_allowed({"localhost:8081"}, loopback, {}));
    EXPECT_FALSE(host_allowed({"localhost.:8080"}, loopback, {}));
    EXPECT_TRUE(host_allowed({"localhost:9000"}, parse_listen_address("[::1]:9000"), {}));
    const auto wide = parse_listen_address("192.0.2.10:8080");
    EXPECT_FALSE(host_allowed({"localhost:8080"}, wide, {}));
    EXPECT_TRUE(host_allowed({"192.0.2.10:8080"}, wide, {}));
}

TEST(HostCheck, AcceptsAllowedHostNames) {
    const auto listener = parse_listen_address("192.0.2.10:8080");
    const std::vector<std::string> allowed = {"praenora.example.org", "10.1.2.3"};
    EXPECT_TRUE(host_allowed({"praenora.example.org"}, listener, allowed));
    EXPECT_TRUE(host_allowed({"Praenora.Example.ORG:8080"}, listener, allowed));
    EXPECT_TRUE(host_allowed({"10.1.2.3:8080"}, listener, allowed));
    EXPECT_FALSE(host_allowed({"praenora.example.org:443"}, listener, allowed));
    EXPECT_FALSE(host_allowed({"example.org"}, listener, allowed));
    EXPECT_FALSE(host_allowed({"praenora.example.org.evil"}, listener, allowed));
}

TEST(HostCheck, AcceptsAllowedIpv6AddressesAsAddresses) {
    const auto listener = parse_listen_address("[::]:8080");
    const std::vector<std::string> allowed = {"[2001:db8::1]", "praenora.example.org"};
    EXPECT_TRUE(host_allowed({"[2001:db8::1]:8080"}, listener, allowed));
    EXPECT_TRUE(host_allowed({"[2001:DB8:0:0::1]"}, listener, allowed));
    EXPECT_TRUE(host_allowed({"praenora.example.org:8080"}, listener, allowed));
    EXPECT_FALSE(host_allowed({"[2001:db8::2]:8080"}, listener, allowed));
    EXPECT_FALSE(host_allowed({"[2001:db8::1]:9090"}, listener, allowed));
    EXPECT_FALSE(host_allowed({"2001:db8::1"}, listener, allowed));
    // An IPv6 literal of --allow-host is no IPv4 name, nor the other way round.
    EXPECT_FALSE(
        host_allowed({"[::ffff:10.1.2.3]"}, parse_listen_address("192.0.2.10:8080"), {"10.1.2.3"}));
}

// A wildcard or other non-loopback listener answers only its own literal address and the
// --allow-host names: not 127.0.0.1, not localhost, not the machine's other addresses.
TEST(HostCheck, WildcardListenersAcceptTheirLiteralAddressAndAllowedNamesOnly) {
    const auto any4 = parse_listen_address("0.0.0.0:8080");
    EXPECT_TRUE(host_allowed({"0.0.0.0:8080"}, any4, {}));
    EXPECT_FALSE(host_allowed({"127.0.0.1:8080"}, any4, {}));
    EXPECT_FALSE(host_allowed({"localhost:8080"}, any4, {}));
    EXPECT_FALSE(host_allowed({"192.0.2.10:8080"}, any4, {}));
    EXPECT_TRUE(host_allowed({"192.0.2.10:8080"}, any4, {"192.0.2.10"}));
    EXPECT_TRUE(host_allowed({"127.0.0.1:8080"}, any4, {"127.0.0.1"}));
    // localhost is a loopback listener's name only, also when --allow-host names it.
    EXPECT_FALSE(host_allowed({"localhost:8080"}, any4, {"localhost"}));
    const auto any6 = parse_listen_address("[::]:8080");
    EXPECT_TRUE(host_allowed({"[::]:8080"}, any6, {}));
    EXPECT_FALSE(host_allowed({"[::1]:8080"}, any6, {}));
    EXPECT_FALSE(host_allowed({"localhost:8080"}, any6, {}));
    EXPECT_TRUE(host_allowed({"[::1]:8080"}, any6, {"[::1]"}));
}

TEST(HostCheck, ChecksAllowHostNames) {
    for (const char* name : {"example.org", "praenora-1.internal", "10.0.0.5", "LOCALHOST", "a",
                             "[::1]", "[2001:db8::1]", "[::ffff:1.2.3.4]"}) {
        SCOPED_TRACE(name);
        EXPECT_NO_THROW(check_allow_host(name));
    }
    for (const char* name :
         {"", "-x", "x-", ".x", "x.", "a b", "x.example.org:80", "a/b", "\xC3\xA9.example", "::1",
          "[::1", "::1]", "[]", "[example.org]", "[::1]:80", "[fe80::1%en0]", "[1.2.3.4]"}) {
        SCOPED_TRACE(name);
        const Error error = error_of([&] { check_allow_host(name); });
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
        EXPECT_TRUE(contains(error.what(), "--allow-host")) << error.what();
    }
}
