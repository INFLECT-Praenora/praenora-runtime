// Helpers for the listener tests of the control API (label server; the rules of
// docs/testing.md#listeners): a short directory for Unix sockets below the system temporary
// directory, the probe that skips (or, where listeners are required, fails) a test where local
// listeners are refused, a cpp-httplib client of a listener's address, and, on POSIX, the praenora
// tool as a child process: started with posix_spawn(), its standard input /dev/null and its
// outputs written to files, waited for with a deadline, signalled, and killed when the test ends
// with it still running. Header-only; httplib.h comes first (listener_support.hpp).
#ifndef PRAENORA_TESTS_SERVER_LISTENER_FIXTURES_HPP
#define PRAENORA_TESTS_SERVER_LISTENER_FIXTURES_HPP

#include "listener_support.hpp"

#include "address.hpp"

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

namespace praenora::test {

inline std::string read_file_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A new directory with a short name ("pn" and eight hexadecimal digits) right below the system
// temporary directory, mode 0700, for Unix sockets, whose paths must stay short; removed with its
// contents when the object is destroyed.
class SocketDir {
public:
    SocketDir() {
        std::random_device device;
        char name[16];
        std::snprintf(name, sizeof name, "pn%08x", static_cast<unsigned>(device()));
        path_ = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(path_);
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all);
    }
    ~SocketDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    SocketDir(const SocketDir&) = delete;
    SocketDir& operator=(const SocketDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    // "unix:<dir>/<name>"
    std::string address(const std::string& name) const { return "unix:" + socket(name); }
    std::string socket(const std::string& name) const {
        const auto text = (path_ / name).u8string();
        return std::string(text.begin(), text.end());
    }

private:
    std::filesystem::path path_;
};

#ifndef _WIN32
// A Unix socket at `path` as a server that crashed leaves it: bound, listening, then closed without
// its file removed, so that a connection to it is refused. The reason it was refused, if it was.
inline std::optional<std::string> leave_stale_socket(const std::string& path) {
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return "socket(AF_UNIX): " + socket_error_text(errno);
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path) {
        ::close(descriptor);
        return std::string("the socket's path is too long: ") + path;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        const int error = errno;
        ::close(descriptor);
        return "bind() to a Unix socket below the temporary directory: " + socket_error_text(error);
    }
    if (::listen(descriptor, 4) != 0) {
        const int error = errno;
        ::close(descriptor);
        return "listen() on a Unix socket below the temporary directory: " +
               socket_error_text(error);
    }
    ::close(descriptor);
    return std::nullopt;
}
#endif

// Why local listeners are refused here (a sandbox's EPERM), if they are: a listener of the
// system's own sockets on 127.0.0.1 and, on POSIX, a Unix socket below the system temporary
// directory, each opened and closed again.
inline std::optional<std::string> listener_refusal() {
    SilentListener tcp;
    if (std::optional<std::string> refusal = tcp.start()) {
        return refusal;
    }
    tcp.close();
#ifndef _WIN32
    const SocketDir dir;
    if (std::optional<std::string> refusal = leave_stale_socket(dir.socket("probe"))) {
        return refusal;
    }
#endif
    return std::nullopt;
}

// A test fixture whose tests skip, or fail where listeners are required, where local listeners
// are refused.
class ListenerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (const std::optional<std::string> refusal = listener_refusal()) {
            PRAENORA_LISTENER_REFUSED(*refusal);
        }
    }
};

// Sends `request`, as it is, to 127.0.0.1:`port` on a connection of its own, and returns all that
// comes back until the server closes the connection (waiting at most 15 s for each read): the raw
// answer. For requests that cpp-httplib's client does not send: HTTP/1.0 without Host, two Host
// headers, a Content-Length larger than the body that follows.
inline std::string raw_exchange(int port, const std::string& request) {
    const RawSocket connection = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == kNoSocket) {
        ADD_FAILURE() << "socket(): " << socket_error_text(last_socket_error());
        return {};
    }
#ifdef _WIN32
    const DWORD timeout_ms = 15000;
    ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                 static_cast<int>(sizeof timeout_ms));
#else
    timeval timeout{};
    timeout.tv_sec = 15;
    ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
#if defined(SO_NOSIGPIPE)
    // A server that closed early is no reason to end the test (macOS has no MSG_NOSIGNAL).
    const int no_sigpipe = 1;
    ::setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
#if defined(MSG_NOSIGNAL)
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::connect(connection, reinterpret_cast<const sockaddr*>(&address),
                  static_cast<SocketLength>(sizeof address)) != 0) {
        ADD_FAILURE() << "connect() to 127.0.0.1:" << port << ": "
                      << socket_error_text(last_socket_error());
        close_raw_socket(connection);
        return {};
    }
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(request.size() - sent, 16384));
        const auto n = ::send(connection, request.data() + sent, chunk, send_flags);
        if (n <= 0) {
            break; // the server answered and closed early: what it answered is read below
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string answer;
    char buffer[4096];
    while (true) {
        const auto n = ::recv(connection, buffer, static_cast<int>(sizeof buffer), 0);
        if (n <= 0) {
            break;
        }
        answer.append(buffer, static_cast<std::size_t>(n));
    }
    close_raw_socket(connection);
    return answer;
}

// The status of a raw answer ("HTTP/1.1 403 Forbidden..." is 403), or 0.
inline int raw_status(const std::string& answer) {
    const std::size_t space = answer.find(' ');
    return space == std::string::npos ? 0 : std::atoi(answer.c_str() + space + 1);
}

// The body of a raw answer (after the blank line that ends its headers).
inline std::string raw_body(const std::string& answer) {
    const std::size_t end = answer.find("\r\n\r\n");
    return end == std::string::npos ? std::string() : answer.substr(end + 4);
}

// The headers of a raw answer, names in lower case, each line as "name: value".
inline std::string raw_headers_lower(const std::string& answer) {
    std::string headers = answer.substr(0, answer.find("\r\n\r\n"));
    for (char& c : headers) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return headers;
}

// A client of the control API at `address` (parse_listen_address()'s syntax), with timeouts of a
// few seconds: through its Unix socket, or to its IP address, never resolved.
inline std::unique_ptr<httplib::Client> client_of(const std::string& address) {
    const server::ListenAddress parsed = server::parse_listen_address(address);
    std::unique_ptr<httplib::Client> client;
    if (parsed.family == server::ListenAddress::Family::unix_socket) {
        client = std::make_unique<httplib::Client>(parsed.path, 80);
        client->set_address_family(AF_UNIX);
    } else {
        client = std::make_unique<httplib::Client>(parsed.host(), parsed.port);
        client->set_direct_hostname_addr_map({{parsed.host(), parsed.host()}});
    }
    client->set_connection_timeout(5, 0);
    client->set_read_timeout(15, 0);
    client->set_write_timeout(15, 0);
    client->set_keep_alive(false);
    // Paths and queries go as the test wrote them.
    client->set_path_encode(false);
    return client;
}

// "Authorization: Bearer <token>", and more headers.
inline httplib::Headers bearer(const std::string& token, httplib::Headers more = {}) {
    more.emplace("Authorization", "Bearer " + token);
    return more;
}

// Waits, at most `timeout`, until `done()` holds; returns whether it does.
template <class Done>
bool eventually(Done&& done, std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return done();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// A token file with `token` and mode 0600 at `file`.
inline void write_token_file(const std::filesystem::path& file, const std::string& token) {
    write_text_file(file, token + "\n");
    std::filesystem::permissions(file, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
}

#ifndef _WIN32
// The praenora tool, started by the test with `args`; its outputs go to stdout.txt and
// stderr.txt in `dir`, which must exist.
class ToolProcess {
public:
    ToolProcess(const std::vector<std::string>& args, const std::filesystem::path& dir)
        : out_(dir / "stdout.txt"), err_(dir / "stderr.txt") {
        std::vector<std::string> command = {PRAENORA_TEST_TOOL};
        command.insert(command.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (std::string& arg : command) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        const std::string out = read_path(out_), err = read_path(err_);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                         0600);
        posix_spawn_file_actions_addopen(&actions, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                         0600);
        // SIGINT and SIGTERM at their default dispositions and no signal blocked, whatever this
        // process inherited (a runner that ignores SIGINT, as a shell does for a job in the
        // background): the tool then installs its handlers, as it does from a terminal.
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGINT);
        sigaddset(&defaults, SIGTERM);
        posix_spawnattr_setsigdefault(&attributes, &defaults);
        sigset_t none;
        sigemptyset(&none);
        posix_spawnattr_setsigmask(&attributes, &none);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
        const int started = posix_spawn(&pid_, command.front().c_str(), &actions, &attributes,
                                        argv.data(), environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        if (started != 0) {
            pid_ = -1;
            ADD_FAILURE() << "posix_spawn() of " << command.front() << ": "
                          << socket_error_text(started);
        }
    }
    ~ToolProcess() {
        if (pid_ > 0 && !exit_code_) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    ToolProcess(const ToolProcess&) = delete;
    ToolProcess& operator=(const ToolProcess&) = delete;

    // Its exit code (128 + the signal's number when a signal ended it), once it ended; waits at
    // most `timeout`.
    std::optional<int> wait(std::chrono::milliseconds timeout = std::chrono::seconds(120)) {
        if (exit_code_ || pid_ <= 0) {
            return exit_code_;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            int status = 0;
            const pid_t reaped = ::waitpid(pid_, &status, WNOHANG);
            if (reaped == pid_) {
                exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                return exit_code_;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool running() { return pid_ > 0 && !exit_code_ && !wait(std::chrono::milliseconds(0)); }

    void signal(int number) {
        if (pid_ > 0 && !exit_code_) {
            ::kill(pid_, number);
        }
    }

    std::string out() const { return read_file_text(out_); }
    std::string err() const { return read_file_text(err_); }
    // Both outputs, for a failure's message.
    std::string outputs() const { return "--- stdout ---\n" + out() + "--- stderr ---\n" + err(); }

    // The address that the tool wrote to `file` (--address-file), without its line feed, once it
    // has; nullopt when the tool ended first or `timeout` passed.
    std::optional<std::string>
    address_in(const std::filesystem::path& file,
               std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) {
                const std::string text = read_file_text(file);
                if (!text.empty() && text.back() == '\n') {
                    return text.substr(0, text.size() - 1);
                }
            }
            if (!running()) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }

private:
    static std::string read_path(const std::filesystem::path& path) {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::filesystem::path out_, err_;
    pid_t pid_ = -1;
    std::optional<int> exit_code_;
};
#endif

} // namespace praenora::test

#endif // PRAENORA_TESTS_SERVER_LISTENER_FIXTURES_HPP
