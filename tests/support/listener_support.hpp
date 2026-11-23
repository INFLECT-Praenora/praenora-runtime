// For tests that open local listeners (label server): the rules of docs/testing.md#listeners
// (127.0.0.1 or ::1 only, never a wildcard, never a host name resolved, always port 0 with the
// chosen port read back, Unix sockets under the system's temporary directory with short paths, and
// connections only to listeners the test opened itself); what such a test does where a listener is
// refused (a sandbox's EPERM): it skips, saying kListenerRefused, or, with
// PRAENORA_TESTS_REQUIRE_LISTENERS=1, which the ci-* test presets set, it fails; a fake OTLP
// collector over cpp-httplib's server, which records every request and answers as the test
// scripts; three raw TCP listeners: one that never accepts (a peer that never answers), a port
// whose listener the test opened and closed again (a connection refused) and, on POSIX, one that
// resets every connection while the client still writes; and, on POSIX, a death test that a
// client's request fails without SIGPIPE ending the process.
//
// Header-only. It includes httplib.h, so an executable that uses it links praenora_httplib (its
// include directory, ws2_32 on Windows and CPPHTTPLIB_SEND_FLAGS on other POSIX systems than
// macOS, which every source that includes httplib.h must agree on); it uses only the standard
// library, POSIX or Winsock, cpp-httplib and GoogleTest, so white-box and black-box tests alike
// can use it.
#ifndef PRAENORA_TESTS_SUPPORT_LISTENER_SUPPORT_HPP
#define PRAENORA_TESTS_SUPPORT_LISTENER_SUPPORT_HPP

// httplib.h comes first: on Windows it includes winsock2.h before anything includes windows.h.
#include <httplib.h>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace praenora::test {

// What a listener test skips with where a local listener is refused, followed by the reason.
// CTest shows such a test as skipped: the SKIP_REGULAR_EXPRESSION of the executables that have
// listener tests (praenora_add_gtest(... LISTENER_SKIP), tests/CMakeLists.txt) names it.
inline constexpr const char* kListenerRefused = "local listener refused: ";

// Whether a refused listener fails the test instead (PRAENORA_TESTS_REQUIRE_LISTENERS=1).
inline bool listeners_required() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* value = std::getenv("PRAENORA_TESTS_REQUIRE_LISTENERS");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && std::string(value) == "1";
}

// The failure where listeners are required. Its words differ from kListenerRefused, which CTest
// would otherwise read as a skip, whatever the test's result.
inline std::string listener_required_failure(const std::string& reason) {
    return "PRAENORA_TESTS_REQUIRE_LISTENERS=1 requires local listeners, and one was not "
           "allowed: " +
           reason;
}

// Skips the test, or fails it where listeners are required, because a local listener was refused
// for `reason`. Use it in the test's body (it returns from the function).
#define PRAENORA_LISTENER_REFUSED(reason)                                                          \
    do {                                                                                           \
        const std::string praenora_listener_reason_ = (reason);                                    \
        if (::praenora::test::listeners_required()) {                                              \
            FAIL() << ::praenora::test::listener_required_failure(praenora_listener_reason_);      \
        } else {                                                                                   \
            GTEST_SKIP() << ::praenora::test::kListenerRefused << praenora_listener_reason_;       \
        }                                                                                          \
    } while (false)

// Starts `listener` (anything with std::optional<std::string> start(), the reason it was
// refused), or skips or fails the test as PRAENORA_LISTENER_REFUSED says.
#define PRAENORA_START_LISTENER(listener)                                                          \
    do {                                                                                           \
        if (const std::optional<std::string> praenora_refusal_ = (listener).start()) {             \
            PRAENORA_LISTENER_REFUSED(*praenora_refusal_);                                         \
        }                                                                                          \
    } while (false)

#ifdef _WIN32
using RawSocket = SOCKET;
inline constexpr RawSocket kNoSocket = INVALID_SOCKET;
inline int last_socket_error() { return WSAGetLastError(); }
inline void close_raw_socket(RawSocket socket) { static_cast<void>(::closesocket(socket)); }
using SocketLength = int;
#else
using RawSocket = int;
inline constexpr RawSocket kNoSocket = -1;
inline int last_socket_error() { return errno; }
inline void close_raw_socket(RawSocket socket) { static_cast<void>(::close(socket)); }
using SocketLength = socklen_t;
#endif

// The text of socket error `error`.
inline std::string socket_error_text(int error) {
    return std::system_category().message(error) + " (" + std::to_string(error) + ")";
}

// A TCP listener on 127.0.0.1 at a port the system chooses, which never accepts a connection: the
// system completes a few connections into its backlog, so a client connects and writes, and then
// waits for an answer that never comes. start() gives the reason it was refused, if it was.
class SilentListener {
public:
    SilentListener() = default;
    ~SilentListener() { close(); }
    SilentListener(const SilentListener&) = delete;
    SilentListener& operator=(const SilentListener&) = delete;

    std::optional<std::string> start() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == kNoSocket) {
            return "socket(): " + socket_error_text(last_socket_error());
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                   static_cast<SocketLength>(sizeof address)) != 0) {
            return "bind() to 127.0.0.1: " + socket_error_text(last_socket_error());
        }
        if (::listen(socket_, 8) != 0) {
            return "listen() on 127.0.0.1: " + socket_error_text(last_socket_error());
        }
        SocketLength size = static_cast<SocketLength>(sizeof address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return "getsockname(): " + socket_error_text(last_socket_error());
        }
        port_ = ntohs(address.sin_port);
        return std::nullopt;
    }

    // Closes the listener: its port now refuses connections (until the system gives it to
    // another listener).
    void close() {
        if (socket_ != kNoSocket) {
            close_raw_socket(socket_);
            socket_ = kNoSocket;
        }
    }

    int port() const noexcept { return port_; }
    // The listening socket, for a listener that accepts on it after all (ResettingListener);
    // kNoSocket before start() and after close().
    RawSocket listening_socket() const noexcept { return socket_; }

private:
    RawSocket socket_ = kNoSocket;
    int port_ = 0;
};

// A port of 127.0.0.1 that refuses connections: the port of a listener that the test opened and
// closed again.
class ClosedPort {
public:
    std::optional<std::string> start() {
        SilentListener listener;
        if (std::optional<std::string> refusal = listener.start()) {
            return refusal;
        }
        port_ = listener.port();
        return std::nullopt; // the listener closes here
    }

    int port() const noexcept { return port_; }

private:
    int port_ = 0;
};

#ifndef _WIN32
// A TCP listener on 127.0.0.1 at a port the system chooses that resets every connection it
// accepts, on a thread of its own: it reads what the client sent first (the start of a request, at
// most 64 KiB), shuts its side down (a FIN) and closes with SO_LINGER {1, 0} (a reset), without
// reading the rest. A client that writes on, as one with a large request does, finds a write
// failing with EPIPE, which raises SIGPIPE unless the client asked for none: the FIN first makes
// Linux report EPIPE, not ECONNRESET, also on the first write after the reset; macOS reports
// EPIPE either way. POSIX only: Windows has no SIGPIPE. start() gives the reason it was refused,
// if it was.
class ResettingListener {
public:
    ResettingListener() = default;
    ~ResettingListener() { stop(); }
    ResettingListener(const ResettingListener&) = delete;
    ResettingListener& operator=(const ResettingListener&) = delete;

    std::optional<std::string> start() {
        if (std::optional<std::string> refusal = listener_.start()) {
            return refusal;
        }
        thread_ = std::thread([this] { accept_and_reset(); });
        return std::nullopt;
    }

    void stop() {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        listener_.close();
    }

    int port() const noexcept { return listener_.port(); }

private:
    void accept_and_reset() {
        while (!stopping_.load()) {
            pollfd waiting{};
            waiting.fd = listener_.listening_socket();
            waiting.events = POLLIN;
            if (::poll(&waiting, 1, 20) <= 0) {
                continue; // nothing yet, or a signal: look at stopping_ again
            }
            const RawSocket connection = ::accept(listener_.listening_socket(), nullptr, nullptr);
            if (connection != kNoSocket) {
                reset(connection);
            }
        }
    }

    static void reset(RawSocket connection) {
        pollfd readable{};
        readable.fd = connection;
        readable.events = POLLIN;
        if (::poll(&readable, 1, 5000) > 0) {
            std::vector<char> received(64 * 1024);
            static_cast<void>(::recv(connection, received.data(), received.size(), 0));
        }
        static_cast<void>(::shutdown(connection, SHUT_WR));
        linger abortive{};
        abortive.l_onoff = 1;
        abortive.l_linger = 0;
        static_cast<void>(
            ::setsockopt(connection, SOL_SOCKET, SO_LINGER, &abortive, sizeof abortive));
        close_raw_socket(connection);
    }

    SilentListener listener_; // bound and listening; accepted on by the thread
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// In the child process of a death test: runs `request` with SIGPIPE at its default disposition,
// which ends the process, and exits: 0 where it failed (saying "the request failed: " and how),
// 1 where it did not. std::_Exit: the child runs nothing more of the test, nor any exit handler.
template <class Request> [[noreturn]] void run_with_default_sigpipe(Request& request) {
    std::signal(SIGPIPE, SIG_DFL);
    const std::optional<std::string> failure = request();
    if (failure) {
        std::fprintf(stderr, "the request failed: %s\n", failure->c_str());
        std::_Exit(0);
    }
    std::fprintf(stderr, "the request did not fail\n");
    std::_Exit(1);
}

// Expects `request` to fail, and its process to live on, where a SIGPIPE ends a process (as in
// plain `praenora run`, or an application that embeds the runtime, which keep its default
// disposition). `request` runs in the child process of a death test, and gives the text that says
// how it failed, or nullopt where it did not fail; it starts what it needs there, such as a
// ResettingListener. GoogleTest's "threadsafe" style: the child is a fresh process that runs only
// this test (its body again, up to here), not a copy of this one with its threads. A SIGPIPE ends
// the child, and the test fails: the child was terminated by a signal, not exited with code 0.
template <class Request> void expect_failure_without_sigpipe(Request&& request) {
#ifdef GTEST_FLAG_SET
    GTEST_FLAG_SET(death_test_style, "threadsafe");
#else
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe"; // GoogleTest 1.11
#endif
    EXPECT_EXIT(run_with_default_sigpipe(request), ::testing::ExitedWithCode(0),
                "the request failed: ");
}
#endif

// A request the fake collector received.
struct ReceivedRequest {
    std::string method, path, body;
    httplib::Headers headers; // names compared in any case

    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string() : found->second;
    }
};

// What the fake collector answers.
struct CollectorAnswer {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body = "{}";
};

// A fake OTLP collector on 127.0.0.1 at a port the system chooses, over httplib::Server: it
// accepts a POST to any path, records it, and answers with the next answer of its script, then
// with its default answer (200 and "{}"). Thread-safe.
class FakeCollector {
public:
    FakeCollector() = default;
    ~FakeCollector() { stop(); }
    FakeCollector(const FakeCollector&) = delete;
    FakeCollector& operator=(const FakeCollector&) = delete;

    // Binds and starts serving; the reason it was refused, if it was.
    std::optional<std::string> start() {
        server_.Post(R"(.*)", [this](const httplib::Request& request, httplib::Response& response) {
            CollectorAnswer answer;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back({request.method, request.path, request.body, request.headers});
                if (!script_.empty()) {
                    answer = std::move(script_.front());
                    script_.pop_front();
                } else {
                    answer = default_answer_;
                }
            }
            changed_.notify_all();
            response.status = answer.status;
            for (const auto& [name, value] : answer.headers) {
                response.set_header(name, value);
            }
            response.set_content(answer.body, "application/json");
        });
        // The reason a listener is refused: a listener of the system's own sockets first, whose
        // error is known (cpp-httplib's says only that it failed).
        SilentListener probe;
        if (std::optional<std::string> refusal = probe.start()) {
            return refusal;
        }
        probe.close();
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            return std::string("httplib::Server::bind_to_any_port(\"127.0.0.1\") failed");
        }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return std::nullopt;
    }

    void stop() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int port() const noexcept { return port_; }

    // http://127.0.0.1:<port><path>
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    // The answers to the next requests, in order; then the default answer.
    void script(std::vector<CollectorAnswer> answers) {
        const std::lock_guard<std::mutex> lock(mutex_);
        script_.assign(answers.begin(), answers.end());
    }
    void set_default_answer(CollectorAnswer answer) {
        const std::lock_guard<std::mutex> lock(mutex_);
        default_answer_ = std::move(answer);
    }

    std::vector<ReceivedRequest> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    // The requests to `path`, in order.
    std::vector<ReceivedRequest> requests_to(const std::string& path) const {
        std::vector<ReceivedRequest> matching;
        for (ReceivedRequest& request : requests()) {
            if (request.path == path) {
                matching.push_back(std::move(request));
            }
        }
        return matching;
    }

    // Waits until `condition` holds for the requests received, at most `timeout`; whether it
    // holds.
    bool wait_for(const std::function<bool(const std::vector<ReceivedRequest>&)>& condition,
                  std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return condition(requests_); });
    }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<ReceivedRequest> requests_;
    std::deque<CollectorAnswer> script_;
    CollectorAnswer default_answer_;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_LISTENER_SUPPORT_HPP
