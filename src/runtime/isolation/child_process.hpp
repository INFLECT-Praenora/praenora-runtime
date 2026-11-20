// A child process with a channel: how the runtime starts a module host, watches it and ends it
// (docs/specs/process-isolation.md). Implemented in platform/posix/child_process_posix.cpp and
// platform/windows/child_process_windows.cpp; on Windows nothing can be started in this version.
//
// POSIX. The child is started with posix_spawn(), never by fork() of the runtime's process:
//   - descriptor 3 is its end of a new channel (isolation/channel.hpp), standard input is
//     /dev/null, and standard output and error are this process's;
//   - on macOS every other descriptor is closed (POSIX_SPAWN_CLOEXEC_DEFAULT); elsewhere the
//     child must close those above 3 itself (the module host does it first thing);
//   - it gets a process group of its own (so a terminal's Ctrl+C reaches only the runtime) and an
//     empty signal mask; it inherits the environment and the current directory.
// No process-wide signal disposition of this process is changed. When the child ends, whatever is
// left in its process group (processes it started and that outlive it) is killed before the child
// is reaped, while the group's number cannot yet go to another process.
#ifndef PRAENORA_RUNTIME_ISOLATION_CHILD_PROCESS_HPP
#define PRAENORA_RUNTIME_ISOLATION_CHILD_PROCESS_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// How a child process ended: an exit (exit(), _exit() or returning from main) with its code, or a
// signal. Neither is set when the system could not tell (the process was reaped by someone else).
struct ChildExit {
    std::optional<int> exit_code;
    std::optional<int> signal;
};

// "SIGSEGV", "SIGABRT", "SIGKILL", ... for the usual signals; "signal <n>" for any other.
std::string signal_name(int signal);

// "exited with code 3", "was ended by signal 11 (SIGSEGV)", or "ended" when neither is known.
std::string describe(const ChildExit& exit);

// Whether this platform can start module hosts: POSIX systems; not Windows in this version.
bool process_isolation_available() noexcept;

class ChildProcess {
public:
    struct Started {
        std::unique_ptr<ChildProcess> process;
        int channel = -1; // this process's end of the channel; the caller owns it (Channel)
    };

    // Starts `executable` with `arguments` (argv[1] on; argv[0] is the executable's path) and a
    // new channel, as the header says. Throws praenora::Error(MODULE_HOST_UNAVAILABLE) with
    // details {"reason": "spawn", "system": the system's text} when it cannot be started, and on
    // Windows always ("not available on Windows in this version").
    static Started start(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments);

    // Terminates a process that is still running, and waits for it.
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    std::int64_t pid() const noexcept;

    // Kills the process's group (SIGKILL), unless it was reaped already. Thread-safe.
    void terminate() noexcept;

    // Waits until the process ends, at most `timeout` (no limit without one), kills what is left
    // in its group and reaps it. The exit, or nullopt while it still runs. Thread-safe: concurrent
    // waits agree, and a process is reaped exactly once.
    std::optional<ChildExit> wait(std::optional<std::chrono::nanoseconds> timeout = std::nullopt);

    // The exit once the process is reaped; nullopt before.
    std::optional<ChildExit> exit_status() const;

private:
    explicit ChildProcess(std::int64_t pid) noexcept;
    // Reaps the process if it has ended; under mutex_.
    void poll_locked();

    std::int64_t pid_;
    mutable std::mutex mutex_;
    std::optional<ChildExit> exit_; // guarded; set once reaped
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_CHILD_PROCESS_HPP
