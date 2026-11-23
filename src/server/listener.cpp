// The listener of the control API over cpp-httplib's server. httplib.h comes first: on Windows it
// includes winsock2.h, which must come before windows.h. It is used header-only, without TLS or
// compression (the only CPPHTTPLIB_* definition is CPPHTTPLIB_SEND_FLAGS, MSG_NOSIGNAL on POSIX
// systems other than macOS, from praenora_httplib; cpp-httplib's server ignores SIGPIPE anyway).
//
// The listening socket is made here, not by cpp-httplib, and handed to its server, which then only
// accepts on it: so a Unix socket gets its mode 0600 between bind() and listen() (no connection is
// accepted before), the system's error of a refused bind() is reported, and a TCP port is never
// shared with another process (SO_REUSEADDR on POSIX, never SO_REUSEPORT; SO_EXCLUSIVEADDRUSE on
// Windows).
#include <httplib.h>

#include "listener.hpp"

#include <praenora/runtime/error.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace praenora::server {

namespace {

constexpr int kBacklog = 128;
constexpr const char* kJsonType = "application/json; charset=utf-8";

int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void close_raw(socket_t descriptor) {
#ifdef _WIN32
    ::closesocket(descriptor);
#else
    ::close(descriptor);
#endif
}

// "<call>: <the system's message> (<number>)".
std::string system_error_text(const char* call, int error) {
    return std::string(call) + ": " + std::system_category().message(error) + " (" +
           std::to_string(error) + ")";
}

Error cannot_listen(const ListenAddress& address, const std::string& why) {
    return Error(ErrorCode::IO_ERROR, "cannot listen on " + address.to_string() + ": " + why);
}

// A socket that is closed when the object goes, unless it was released.
class OwnedSocket {
public:
    explicit OwnedSocket(socket_t owned) : socket_(owned) {}
    ~OwnedSocket() {
        if (socket_ != INVALID_SOCKET) {
            close_raw(socket_);
        }
    }
    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;

    socket_t get() const noexcept { return socket_; }
    socket_t release() noexcept { return std::exchange(socket_, INVALID_SOCKET); }

private:
    socket_t socket_;
};

#ifndef _WIN32
// How often, and how far apart, a socket that exists is probed before it counts as stale.
constexpr int kProbes = 3;
constexpr std::chrono::milliseconds kProbePause(50);

// A stream socket of `family` that a program which a child process executes does not inherit:
// module hosts are started with posix_spawn(), and none of them may hold a listener.
int new_socket(int family) {
#ifdef SOCK_CLOEXEC
    return ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int created = ::socket(family, SOCK_STREAM, 0);
    if (created < 0) {
        return created;
    }
    const int flags = ::fcntl(created, F_GETFD);
    if (flags < 0 || ::fcntl(created, F_SETFD, flags | FD_CLOEXEC) != 0) {
        const int error = errno;
        ::close(created);
        errno = error;
        return -1;
    }
    return created;
#endif
}

// The socket address of the Unix socket at `address.path`, its length in `length`. A path that
// sun_path cannot hold with its final NUL, or that holds a NUL itself, is refused here too, not
// only by the parser: a ListenAddress may be made without it.
sockaddr_un unix_address(const ListenAddress& address, socklen_t& length) {
    const std::string& path = address.path;
    sockaddr_un result{};
    if (path.empty() || path.find('\0') != std::string::npos) {
        // Not named: a NUL would end the message's text.
        throw Error(ErrorCode::IO_ERROR, "cannot listen on a Unix socket whose path is empty or "
                                         "holds a NUL byte");
    }
    if (path.size() > sizeof result.sun_path - 1) {
        throw cannot_listen(address, "the socket's path is " + std::to_string(path.size()) +
                                         " bytes long, and a Unix socket's path holds at most " +
                                         std::to_string(sizeof result.sun_path - 1) +
                                         " bytes on this platform");
    }
    result.sun_family = AF_UNIX;
    std::memcpy(result.sun_path, path.data(), path.size());
    result.sun_path[path.size()] = '\0';
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return result;
}

// The stale-socket rule: nothing at `path`, or a socket that nobody listens on, which is removed.
// Anything else is refused: a file of another kind (a symbolic link too, which is not followed),
// a socket that somebody listens on, or one about which that cannot be told.
void clear_unix_path(const ListenAddress& address) {
    const std::string& path = address.path;
    socklen_t length = 0;
    const sockaddr_un target = unix_address(address, length); // checks the path first
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        const int error = errno;
        if (error == ENOENT) {
            return;
        }
        throw cannot_listen(address,
                            "its path cannot be examined: " + system_error_text("lstat()", error));
    }
    if (!S_ISSOCK(status.st_mode)) {
        throw cannot_listen(address, "'" + path +
                                         "' exists and is not a socket; remove it or choose "
                                         "another path");
    }
    // A socket: a connection to it tells whether somebody listens. Without blocking, so that one
    // whose backlog is full is not waited for. On macOS and the BSDs a full backlog refuses a
    // connection as a socket without a listener does (ECONNREFUSED, where Linux says EAGAIN): the
    // socket counts as stale only when kProbes connections, kProbePause apart, are all refused.
    for (int probe_number = 1;; ++probe_number) {
        const OwnedSocket probe(new_socket(AF_UNIX));
        if (probe.get() < 0) {
            throw cannot_listen(address, system_error_text("socket()", errno));
        }
        const int flags = ::fcntl(probe.get(), F_GETFL);
        if (flags >= 0) {
            ::fcntl(probe.get(), F_SETFL, flags | O_NONBLOCK);
        }
        const int connected =
            ::connect(probe.get(), reinterpret_cast<const sockaddr*>(&target), length);
        const int error = connected == 0 ? 0 : errno;
        if (connected == 0 || error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS) {
            throw cannot_listen(address, "another process listens on it");
        }
        if (error != ECONNREFUSED) {
            throw cannot_listen(address, "whether another process listens on it cannot be told: " +
                                             system_error_text("connect()", error));
        }
        if (probe_number == kProbes) {
            break;
        }
        std::this_thread::sleep_for(kProbePause);
    }
    // A stale socket, as a server that crashed leaves it.
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw cannot_listen(address, "the stale socket cannot be removed: " +
                                         system_error_text("unlink()", errno));
    }
}

// A Unix socket at `address.path`, mode 0600, listening; its file's device and inode in `device`
// and `inode`.
socket_t open_unix(const ListenAddress& address, dev_t& device, ino_t& inode) {
    clear_unix_path(address);
    const std::string& path = address.path;
    OwnedSocket listening(new_socket(AF_UNIX));
    if (listening.get() < 0) {
        throw cannot_listen(address, system_error_text("socket()", errno));
    }
#ifdef __linux__
    // Linux gives the socket's file the mode of the socket itself, less the umask: set first, the
    // file never has a wider mode. Elsewhere (macOS) the file is made with 0777 less the umask,
    // and the chmod() below narrows it before anybody can connect.
    static_cast<void>(::fchmod(listening.get(), S_IRUSR | S_IWUSR));
#endif
    socklen_t length = 0;
    const sockaddr_un bound = unix_address(address, length);
    if (::bind(listening.get(), reinterpret_cast<const sockaddr*>(&bound), length) != 0) {
        throw cannot_listen(address, system_error_text("bind()", errno));
    }
    // Bound but not listening: a connection is refused until listen() below.
    const auto remove_file = [&] { static_cast<void>(::unlink(path.c_str())); };
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        const int error = errno;
        remove_file();
        throw cannot_listen(address, system_error_text("chmod()", error));
    }
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode) ||
        (status.st_mode & 07777) != (S_IRUSR | S_IWUSR)) {
        remove_file();
        throw cannot_listen(address, "its socket's file is not a socket of mode 0600 after it "
                                     "was made");
    }
    device = status.st_dev;
    inode = status.st_ino;
    if (::listen(listening.get(), kBacklog) != 0) {
        const int error = errno;
        remove_file();
        throw cannot_listen(address, system_error_text("listen()", error));
    }
    return listening.release();
}
#endif

// A TCP socket bound to `address` (its port 0 letting the system choose), listening; its port in
// `port`.
socket_t open_tcp(const ListenAddress& address, std::uint16_t& port) {
    const bool ipv6 = address.family == ListenAddress::Family::ipv6;
    const int family = ipv6 ? AF_INET6 : AF_INET;
#ifdef _WIN32
    OwnedSocket listening(::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                       WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
    if (listening.get() == INVALID_SOCKET) {
        throw cannot_listen(address, system_error_text("WSASocketW()", last_socket_error()));
    }
    // No other process may bind the same port.
    const BOOL exclusive = TRUE;
    ::setsockopt(listening.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), static_cast<int>(sizeof exclusive));
    const DWORD v6_only = 1;
    const int v6_only_size = static_cast<int>(sizeof v6_only);
#else
    OwnedSocket listening(new_socket(family));
    if (listening.get() < 0) {
        throw cannot_listen(address, system_error_text("socket()", errno));
    }
    // A port that a server ended a moment ago may be bound again (connections of the old one may
    // linger in TIME_WAIT); SO_REUSEPORT, which lets two processes share a port, is never set.
    const int reuse = 1;
    ::setsockopt(listening.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                 static_cast<socklen_t>(sizeof reuse));
    const int v6_only = 1;
    const auto v6_only_size = static_cast<socklen_t>(sizeof v6_only);
#endif
    if (ipv6) {
        // [::] takes IPv6 only: its Host check names IPv6 addresses.
        ::setsockopt(listening.get(), IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&v6_only), v6_only_size);
    }
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (ipv6) {
        auto& bound = reinterpret_cast<sockaddr_in6&>(storage);
        bound.sin6_family = AF_INET6;
        bound.sin6_port = htons(address.port);
        std::memcpy(&bound.sin6_addr, address.bytes.data(), 16);
        length = static_cast<socklen_t>(sizeof(sockaddr_in6));
    } else {
        auto& bound = reinterpret_cast<sockaddr_in&>(storage);
        bound.sin_family = AF_INET;
        bound.sin_port = htons(address.port);
        std::memcpy(&bound.sin_addr, address.bytes.data(), 4);
        length = static_cast<socklen_t>(sizeof(sockaddr_in));
    }
    if (::bind(listening.get(), reinterpret_cast<const sockaddr*>(&storage), length) != 0) {
        throw cannot_listen(address, system_error_text("bind()", last_socket_error()));
    }
    if (::listen(listening.get(), kBacklog) != 0) {
        throw cannot_listen(address, system_error_text("listen()", last_socket_error()));
    }
    sockaddr_storage chosen{};
    auto chosen_length = static_cast<socklen_t>(sizeof chosen);
    if (::getsockname(listening.get(), reinterpret_cast<sockaddr*>(&chosen), &chosen_length) != 0) {
        throw cannot_listen(address, system_error_text("getsockname()", last_socket_error()));
    }
    port = ntohs(ipv6 ? reinterpret_cast<const sockaddr_in6&>(chosen).sin6_port
                      : reinterpret_cast<const sockaddr_in&>(chosen).sin_port);
    return listening.release();
}

// cpp-httplib's server, accepting on a socket that listens already.
class AcceptingServer final : public httplib::Server {
public:
    // From now on, listen_after_bind() accepts on `listening`, and stop() closes it.
    void adopt(socket_t listening) { svr_sock_ = listening; }
};

// `text` with the ASCII letters A-Z in lower case.
std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// The error document of an answer that cpp-httplib gave before the handler, or of a handler that
// threw: such an answer has no body yet.
Error transport_error(int status) {
    switch (status) {
    case 413:
        return Error(ErrorCode::INVALID_ARGUMENT,
                     "the request's body is larger than 64 KiB, the limit of the control API");
    case 414:
        return Error(ErrorCode::INVALID_ARGUMENT, "the request's target is too long");
    case 404:
        return Error(ErrorCode::NOT_FOUND,
                     "the path is not one of the control API (GET /v0/openapi.json lists them)");
    case 416:
        return Error(ErrorCode::INVALID_ARGUMENT, "the request's Range header is malformed");
    case 501:
        return Error(ErrorCode::INVALID_ARGUMENT,
                     "the request's transfer coding is not supported: send chunked or none");
    default:
        break;
    }
    if (status >= 500) {
        return Error(ErrorCode::INTERNAL, "the server failed to answer the request");
    }
    return Error(ErrorCode::INVALID_ARGUMENT,
                 "the request is not one that the control API reads (HTTP status " +
                     std::to_string(status) +
                     "): a request line of a known method, headers as HTTP/1.1 defines them, "
                     "and exactly one Host header");
}

// The limits of a listener, and error documents for the answers that cpp-httplib gives itself.
void configure(httplib::Server& server) {
    server.set_payload_max_length(kMaxBodyBytes);
    server.set_read_timeout(kIoTimeoutSeconds, 0);
    server.set_write_timeout(kIoTimeoutSeconds, 0);
    server.set_keep_alive_max_count(kKeepAliveMaxRequests);
    server.set_keep_alive_timeout(kKeepAliveSeconds);
    server.new_task_queue = [] { return new httplib::ThreadPool(kWorkerThreads); };
    // An answer of cpp-httplib's own (a malformed request, a body over the limit) carries an error
    // document, as every error of the API does; the handler's answers keep their bodies.
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (!response.body.empty()) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        response.set_content(transport_error(response.status).to_json(), kJsonType);
        return httplib::Server::HandlerResponse::Handled;
    });
    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& response, std::exception_ptr) {
            response.status = 500;
            response.set_content(transport_error(500).to_json(), kJsonType);
        });
}

// Hands every request that `server` reads from a listener of `kind` to `handler`, as a Request,
// and writes its Response back.
void route_to(httplib::Server& server, RequestHandler handler, ListenerKind kind) {
    const auto answer = [handler = std::move(handler), kind](const httplib::Request& in,
                                                             httplib::Response& out) {
        Request request;
        request.method = in.method;
        request.path = in.path;
        const std::size_t question = in.target.find('?');
        if (question != std::string::npos) {
            request.query = in.target.substr(question + 1);
        }
        for (const auto& [name, value] : in.headers) {
            request.headers.emplace(ascii_lower(name), value);
        }
        request.body = in.body;
        request.listener = kind;
        Response response;
        try {
            response = handler(request);
        } catch (...) {
            // Whatever it said stays in the server.
            response = Response();
            response.status = 500;
            response.content_type = kJsonType;
            response.body = transport_error(500).to_json();
        }
        out.status = response.status;
        for (const auto& [name, value] : response.headers) {
            out.set_header(name, value);
        }
        if (!response.content_type.empty()) {
            out.set_content(response.body, response.content_type);
        }
    };
    // Every path and method reaches the handler, which answers 404 and 405 itself; HEAD comes
    // with the GET handlers, its method kept. Methods that cpp-httplib knows no handler for
    // (TRACE, CONNECT) and others are refused by it (400).
    constexpr const char* kEveryPath = ".*";
    server.Get(kEveryPath, answer);
    server.Post(kEveryPath, answer);
    server.Put(kEveryPath, answer);
    server.Patch(kEveryPath, answer);
    server.Delete(kEveryPath, answer);
    server.Options(kEveryPath, answer);
}

} // namespace

struct Listener::Impl {
    ListenAddress address;
    AcceptingServer server;
    std::thread thread; // runs the accept loop; joined by stop()
    std::mutex mutex;   // guards the members below
    bool started = false;
    bool stopped = false;
#ifndef _WIN32
    bool owns_socket_file = false; // a Unix socket's file, made by this listener
    dev_t device = 0;
    ino_t inode = 0;
#endif

    // Removes the Unix socket's file, unless the path names another file now.
    void remove_socket_file() {
#ifndef _WIN32
        if (!owns_socket_file) {
            return;
        }
        owns_socket_file = false;
        struct stat status{};
        if (::lstat(address.path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) &&
            status.st_dev == device && status.st_ino == inode) {
            static_cast<void>(::unlink(address.path.c_str()));
        }
#endif
    }
};

Listener::Listener(const ListenAddress& address) : impl_(std::make_unique<Impl>()) {
    impl_->address = address;
    socket_t listening = INVALID_SOCKET;
    if (address.family == ListenAddress::Family::unix_socket) {
#ifdef _WIN32
        throw Error(ErrorCode::UNSUPPORTED, "listen address '" + address.to_string() +
                                                "': Unix sockets are not supported on Windows");
#else
        listening = open_unix(address, impl_->device, impl_->inode);
        impl_->owns_socket_file = true;
#endif
    } else {
        listening = open_tcp(address, impl_->address.port);
    }
    // From here the server owns the socket: its stop() closes it.
    impl_->server.adopt(listening);

    configure(impl_->server);
}

Listener::~Listener() { stop(); }

const ListenAddress& Listener::address() const noexcept { return impl_->address; }

void Listener::start(RequestHandler handler) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started || impl_->stopped) {
        throw Error(ErrorCode::INVALID_STATE,
                    impl_->stopped
                        ? "the listener on " + impl_->address.to_string() + " has stopped"
                        : "the listener on " + impl_->address.to_string() + " has started already");
    }
    route_to(impl_->server, std::move(handler), impl_->address.kind());
    impl_->thread = std::thread([&server = impl_->server] { server.listen_after_bind(); });
    impl_->started = true;
}

void Listener::stop() {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopped = true;
    // Closes the listening socket: the accept loop ends, and listen_after_bind() returns once the
    // worker threads have answered the requests they hold.
    impl_->server.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->remove_socket_file();
}

} // namespace praenora::server
