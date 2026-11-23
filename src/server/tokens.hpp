// Who may do what through the control API: the scopes of a request, the bearer tokens read from
// token files, the rules for listeners and credentials, and the Host check against DNS rebinding.
#ifndef PRAENORA_SERVER_TOKENS_HPP
#define PRAENORA_SERVER_TOKENS_HPP

#include "address.hpp"
#include "http_types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::server {

// The scopes of a request: read (the queries) and control (opening, stopping and deleting runs).
struct Scopes {
    bool read = false;
    bool control = false;
};

// The tokens of a TCP listener; an empty one is not set. A token is never empty.
struct Tokens {
    std::string control; // --token-file: the read and control scopes
    std::string read;    // --read-token-file: the read scope only

    bool empty() const noexcept { return control.empty() && read.empty(); }
};

// The shortest token a token file may hold.
inline constexpr std::size_t kMinTokenLength = 32;

// The token in the token file `path`: its first line without trailing white space, at least
// kMinTokenLength characters, each one of A-Z a-z 0-9 - . _ ~ + / =. On POSIX, neither the group
// nor others may read or write the file (its mode & 077 is 0). Throws
// praenora::Error(INVALID_ARGUMENT) naming the file (never the token) when it cannot be read or
// breaks a rule.
std::string read_token_file(const std::string& path);

// The tokens of --token-file and --read-token-file, each read with read_token_file(). Throws
// praenora::Error(INVALID_ARGUMENT) as that does, and when the two tokens are equal.
Tokens load_tokens(const std::optional<std::string>& token_file,
                   const std::optional<std::string>& read_token_file);

// Throws praenora::Error(INVALID_ARGUMENT) for credentials that a listener on `address` refuses:
// tokens with a Unix socket (its mode is the credential), and no tokens with a TCP address that
// is not a loopback address.
void check_credentials(const ListenAddress& address, const Tokens& tokens);

// Whether `a` and `b` are equal, in a time that depends on their lengths only.
bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

// What the credentials of a request give it. A request that is not authenticated answers 401
// (but for GET /v0/health, which needs none).
struct ScopeDecision {
    bool authenticated = false;
    Scopes scopes;
};

// The scopes of a request through a listener of `kind` with `tokens`, whose Authorization header
// has `authorization` (its values). A Unix socket gives both scopes; TCP without tokens gives the
// read scope; TCP with tokens needs exactly one "Authorization: Bearer <token>" (the scheme in any
// case) naming one of them: the control token gives both scopes, the read token the read scope,
// and anything else nothing. The tokens are compared in constant time, each of them every time.
ScopeDecision decide_scopes(ListenerKind kind, const Tokens& tokens,
                            const std::vector<std::string>& authorization);

// Throws praenora::Error(INVALID_ARGUMENT) unless `name` can be an --allow-host name: a host name
// or an IPv4 address (letters, digits, '-' and '.', neither first nor last a '.' or '-'), or an
// IPv6 address in brackets ("[2001:db8::1]"); never a port.
void check_allow_host(std::string_view name);

// Whether a request through a TCP listener on `listener` (its real port) whose Host header has
// `host` (its values) may be answered: exactly one value, whose host is the listener's IP address
// (compared as an address), "localhost" (in any case, for a loopback listener only) or one of
// `allow_hosts` (names in any case, bracketed IPv6 addresses compared as addresses), and whose
// port, when it gives one, is the listener's. A wildcard listener (0.0.0.0, [::]) is no
// exception: only its literal address and `allow_hosts` pass.
bool host_allowed(const std::vector<std::string>& host, const ListenAddress& listener,
                  const std::vector<std::string>& allow_hosts);

} // namespace praenora::server

#endif // PRAENORA_SERVER_TOKENS_HPP
