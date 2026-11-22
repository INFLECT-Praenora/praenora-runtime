// For the tests of a module host's restricted sandbox profile (macOS): whether this process runs
// in a sandbox already, where macOS applies no second one, so that a module host it starts cannot
// apply the profile; the words such a test skips with; a TCP listener on 127.0.0.1 for a module
// host to connect to; whether this process's standard error is /dev/null; and, on macOS, a guard
// that removes a directory with what was made immutable in it. Header-only; uses only the standard
// library, POSIX and macOS's file flags, so white-box and black-box tests alike can use it.
//
// The answer does not come from the runtime under test: /usr/bin/sandbox-exec, the system's own
// tool, is asked to run /usr/bin/true under a profile that allows everything, which it cannot do
// in a sandbox. CTest shows a test that skips with kNestedSandboxSkip as skipped
// (SKIP_REGULAR_EXPRESSION, tests/CMakeLists.txt).
#ifndef PRAENORA_TESTS_SUPPORT_SANDBOX_SUPPORT_HPP
#define PRAENORA_TESTS_SUPPORT_SANDBOX_SUPPORT_HPP

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#endif

namespace praenora::test {

// What a test of the restricted profile skips with in a process that runs in a sandbox already.
inline constexpr const char* kNestedSandboxSkip =
    "nested sandbox: this process runs in a sandbox already, and macOS applies no second one, so "
    "a module host cannot apply the restricted profile here; run this test outside any sandbox";

// What a test of the restricted profile skips with where the platform has none.
inline constexpr const char* kNoRestrictedProfileSkip =
    "the restricted sandbox profile is available only on macOS in this version";

// Whether this platform has the restricted profile: macOS.
inline constexpr bool restricted_profile_available() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

// The program that tells whether this process runs in a sandbox already.
inline constexpr const char* kSandboxExec = "/usr/bin/sandbox-exec";

// Whether this process runs in a macOS sandbox already: sandbox-exec cannot run /usr/bin/true even
// under a profile that allows everything (it exits nonzero, saying "Operation not permitted").
// Found out once; false off macOS, and where sandbox-exec cannot be started.
inline bool in_a_sandbox() {
#if defined(__APPLE__)
    static const bool sandboxed = [] {
        char program[] = "sandbox-exec";
        char option[] = "-p";
        char profile[] = "(version 1)(allow default)";
        char command[] = "/usr/bin/true";
        char* argv[] = {program, option, profile, command, nullptr};
        char* envp[] = {nullptr};
        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            return false;
        }
        // Its complaint is no news: the test says why it skips.
        static_cast<void>(
            ::posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0));
        static_cast<void>(
            ::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0));
        pid_t child = 0;
        const int started = ::posix_spawn(&child, kSandboxExec, &actions, nullptr, argv, envp);
        static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
        if (started != 0) {
            return false;
        }
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }();
    return sandboxed;
#else
    return false;
#endif
}

// Why a module host of this process cannot apply the restricted profile, for a test that needs it
// applied to skip with: kNoRestrictedProfileSkip off macOS, kNestedSandboxSkip in a sandbox
// already; nullptr when it can.
inline const char* restricted_profile_skip() {
    if (!restricted_profile_available()) {
        return kNoRestrictedProfileSkip;
    }
    return in_a_sandbox() ? kNestedSandboxSkip : nullptr;
}

#ifndef _WIN32
// A TCP listener on 127.0.0.1, where this process may open one (a sandbox around it may refuse
// that): a connection to its port is then accepted by the system, without an accept(). Without
// one, its port is one that nothing listens on.
class LoopbackListener {
public:
    LoopbackListener() {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof address;
        if (descriptor_ >= 0 &&
            ::bind(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0 &&
            ::listen(descriptor_, 8) == 0 &&
            ::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
            port_ = ntohs(address.sin_port);
            listening_ = true;
        } else {
            port_ = 9; // discard: nothing listens there on a test machine
        }
    }
    ~LoopbackListener() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;

    int port() const noexcept { return port_; }
    bool listening() const noexcept { return listening_; }

private:
    int descriptor_ = -1;
    int port_ = 0;
    bool listening_ = false;
};

// The errno this process gets when it connects to 127.0.0.1:`port`; 0 when it connects.
inline int own_connect(int port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return errno;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int result =
        ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    const int error = result == 0 ? 0 : errno;
    static_cast<void>(::close(descriptor));
    return error;
}

// Whether this process's descriptor 2 (standard error) is /dev/null: both have the same device and
// inode numbers. fx-sandbox-probe reports the same of its module host's descriptor 2, which is the
// runtime's, and so this process's when this process runs the runtime.
inline bool stderr_is_null() {
    struct stat descriptor{};
    struct stat null{};
    return ::fstat(2, &descriptor) == 0 && ::stat("/dev/null", &null) == 0 &&
           descriptor.st_dev == null.st_dev && descriptor.st_ino == null.st_ino;
}
#endif

#if defined(__APPLE__)
// Removes a directory with everything in it when it goes out of scope, however the test ends (a
// failed assertion included), also a file that a test or a module made immutable there
// (UF_IMMUTABLE), which neither std::filesystem::remove_all() nor the system's cleaning of its
// temporary directory removes: it first clears the flags of the directory and of every entry below
// it, never following a symbolic link. Errors are ignored. Declare it before anything there can be
// made immutable, and after the ScratchDir of that directory, if any, so that it goes first.
class RemoveAllEvenImmutable {
public:
    explicit RemoveAllEvenImmutable(std::filesystem::path directory)
        : directory_(std::move(directory)) {}
    ~RemoveAllEvenImmutable() {
        std::error_code ec;
        static_cast<void>(::lchflags(directory_.c_str(), 0));
        for (std::filesystem::recursive_directory_iterator entry(directory_, ec), end;
             !ec && entry != end; entry.increment(ec)) {
            static_cast<void>(::lchflags(entry->path().c_str(), 0));
        }
        std::filesystem::remove_all(directory_, ec);
    }
    RemoveAllEvenImmutable(const RemoveAllEvenImmutable&) = delete;
    RemoveAllEvenImmutable& operator=(const RemoveAllEvenImmutable&) = delete;

private:
    std::filesystem::path directory_;
};
#endif

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_SANDBOX_SUPPORT_HPP
