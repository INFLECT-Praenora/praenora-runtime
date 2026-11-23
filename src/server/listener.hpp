// A listener of the control API: a Unix domain socket of mode 0600 or a TCP address, served by
// cpp-httplib's server on worker threads of its own, within the limits of the transport. Each
// request is handed to a handler as a Request (http_types.hpp) and its Response written back; the
// handler (ControlApi::handle()) does everything else. listener.cpp is, with client.cpp, the only
// source of praenora_server that includes httplib.h; this header names none of its types.
#ifndef PRAENORA_SERVER_LISTENER_HPP
#define PRAENORA_SERVER_LISTENER_HPP

#include "address.hpp"
#include "http_types.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace praenora::server {

// The limits of a listener: a request's body (a larger one answers 413), the time to read a
// request and to write an answer, the requests of one connection kept alive and the time it is
// kept between two, and the worker threads that answer requests.
inline constexpr std::size_t kMaxBodyBytes = 64 * 1024;
inline constexpr int kIoTimeoutSeconds = 5;
inline constexpr std::size_t kKeepAliveMaxRequests = 100;
inline constexpr int kKeepAliveSeconds = 5;
inline constexpr std::size_t kWorkerThreads = 8;

// What answers the requests of a listener, from its worker threads, at the same time.
using RequestHandler = std::function<Response(const Request&)>;

class Listener {
public:
    // Opens `address` and listens on it, so that a client may connect from now on; requests are
    // answered once start() is called. A Unix socket: a path that exists is refused unless it is
    // a socket that nobody listens on (a connection to it is refused), which is removed first;
    // the socket is created with mode 0600 before it listens, so no connection is accepted before
    // the mode is set. A TCP address: bound as given (never a host name), its port 0 letting the
    // system choose (address() then has the port chosen), IPv6 for IPv6 only. Throws
    // praenora::Error(IO_ERROR), naming the address and the system's error, when it cannot.
    explicit Listener(const ListenAddress& address);
    // stop(), then removes the Unix socket it created, unless the path now names another file.
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // The address listened on, with the real port.
    const ListenAddress& address() const noexcept;

    // Starts answering requests with `handler`, on kWorkerThreads threads, within the limits
    // above. A request is handed over with its method, its decoded path, its query, its headers
    // (names in lower case), its body and the listener's kind; requests that cpp-httplib refuses
    // before (a malformed request line or header, a Host header missing or given twice in
    // HTTP/1.1, a body over kMaxBodyBytes, an unknown method) and a handler that throws answer
    // the error document of an Error (Error::to_json()) with their status. At most once; throws
    // praenora::Error(INVALID_STATE) for a second call or one after stop().
    void start(RequestHandler handler);

    // Stops accepting connections and waits until every request being answered has been: once it
    // returns, no handler runs, and a connection to the address is refused. Idempotent; from any
    // thread but a handler's.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace praenora::server

#endif // PRAENORA_SERVER_LISTENER_HPP
