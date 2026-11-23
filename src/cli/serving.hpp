// What `praenora run --listen` and `praenora serve` share: the flags of a listener, checked, and a
// server of the control API (praenora_server, src/server): a listener on the address with the
// control API over a run source, the address file, and the note "listening on <address>".
#ifndef PRAENORA_CLI_SERVING_HPP
#define PRAENORA_CLI_SERVING_HPP

#include "args.hpp"
#include "output.hpp"

#include <address.hpp>
#include <control_api.hpp>
#include <listener.hpp>
#include <run_source.hpp>
#include <tokens.hpp>

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/runtime.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::cli {

// The flags of a listener: --listen ADDR, --token-file F, --read-token-file F, --allow-host H
// (repeatable) and --address-file F.
const std::vector<Flag>& listen_flags();

// Their lines in a usage text.
std::string listen_flags_usage();

// A listener's settings, checked.
struct ListenSettings {
    server::ListenAddress address;
    server::Tokens tokens;
    std::vector<std::string> allow_hosts;
    std::optional<std::string> address_file;
};

// The settings that --listen and the flags beside it give; nullopt without --listen, when any of
// the others is a UsageError. Every problem of the address (a host name, a path over the
// platform's limit, a Unix socket on Windows), of the token files, of the credentials (tokens with
// a Unix socket, none with an address that is not a loopback address) and of --allow-host is a
// UsageError that carries its message.
std::optional<ListenSettings> listen_settings(const Arguments& arguments);

// Runs `call`; a praenora::Error that it throws (an address, a token file, credentials that the
// server code refuses) becomes a UsageError with the error's message.
template <class Call> auto as_usage(Call&& call) {
    try {
        return call();
    } catch (const Error& error) {
        throw UsageError(error.what());
    }
}

// Writes `text` and a line feed to `file`, replacing it atomically: a file beside it, renamed over
// it. Throws praenora::Error(IO_ERROR) when it cannot.
void write_address_file(const std::string& file, const std::string& text);

// A server of the control API: its listener, then the control API over a run source.
class Server {
public:
    // Opens the listener (praenora::Error IO_ERROR when it cannot), prints "praenora: listening on
    // <address>" on standard error and writes the address file (IO_ERROR when it cannot), in that
    // order: once the address file is there, the note has been printed and a client may connect;
    // its requests wait until start() answers them.
    Server(const ListenSettings& settings, const Output& output);
    // stop().
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Makes the control API over `source`, which must outlive the Server, and starts answering.
    // Once only.
    void start(const Runtime& runtime, server::RunSource& source);

    // The address listened on, with the real port.
    std::string address() const;

    // Stops listening: no new connection is accepted, and every request being answered has been
    // once it returns. Idempotent.
    void stop();

private:
    ListenSettings settings_;
    server::Listener listener_;
    std::unique_ptr<server::ControlApi> api_;
};

} // namespace praenora::cli

#endif // PRAENORA_CLI_SERVING_HPP
