// isolation/child_process.hpp on POSIX systems (Linux, macOS).
#include "isolation/child_process.hpp"

#include "core/paths.hpp"
#include "isolation/channel.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <system_error>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ; // NOLINT: the process environment, declared by POSIX
#endif

namespace praenora::detail {

namespace {

char** process_environment() noexcept {
#if defined(__APPLE__)
    // Shared libraries on macOS cannot reach `environ` directly.
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

[[noreturn]] void refuse(const std::filesystem::path& executable, const std::string& what,
                         int error) {
    const std::string system = std::strerror(error);
    throw Error(
        ErrorCode::MODULE_HOST_UNAVAILABLE,
        "cannot start the module host '" + path_to_utf8(executable) + "': " + what + ": " + system,
        {},
        nlohmann::json{
            {"reason", "spawn"}, {"executable", path_to_utf8(executable)}, {"system", system}}
            .dump());
}

// Owns a descriptor until released.
class Owned {
public:
    explicit Owned(int descriptor) noexcept : descriptor_(descriptor) {}
    ~Owned() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    int get() const noexcept { return descriptor_; }
    int release() noexcept { return std::exchange(descriptor_, -1); }
    // Moves the descriptor to a number above 3, close-on-exec, so that it can neither be one of
    // the child's standard descriptors nor its descriptor 3 before the file actions run.
    bool move_above_3() noexcept {
        if (descriptor_ > 3) {
            return true;
        }
        const int moved = ::fcntl(descriptor_, F_DUPFD_CLOEXEC, 4);
        if (moved < 0) {
            return false;
        }
        static_cast<void>(::close(descriptor_));
        descriptor_ = moved;
        return true;
    }

private:
    int descriptor_;
};

class FileActions {
public:
    FileActions() { ok_ = ::posix_spawn_file_actions_init(&actions_) == 0; }
    ~FileActions() {
        if (ok_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;
    bool ok() const noexcept { return ok_; }
    posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    bool ok_ = false;
};

class Attributes {
public:
    Attributes() { ok_ = ::posix_spawnattr_init(&attributes_) == 0; }
    ~Attributes() {
        if (ok_) {
            ::posix_spawnattr_destroy(&attributes_);
        }
    }
    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    bool ok() const noexcept { return ok_; }
    posix_spawnattr_t* get() noexcept { return &attributes_; }

private:
    posix_spawnattr_t attributes_{};
    bool ok_ = false;
};

#if defined(__APPLE__)
bool is_open(int descriptor) noexcept { return ::fcntl(descriptor, F_GETFD) != -1; }
#endif

#if defined(__linux__)
// On Linux a child's parent-death signal (PR_SET_PDEATHSIG, which the module host asks for) is
// sent when the thread that started the child ends, not the process. So children are started from
// one thread that lasts as long as the process; never destroyed, as it may be needed until then.
class Spawner {
public:
    static Spawner& instance() {
        static auto* const spawner = new Spawner;
        return *spawner;
    }

    // posix_spawn() on the spawner's thread, or on the calling thread when there is none.
    int spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
              const posix_spawnattr_t* attributes, char* const argv[], char* const envp[]) {
        std::packaged_task<int()> task(
            [=] { return ::posix_spawn(pid, path, actions, attributes, argv, envp); });
        std::future<int> result = task.get_future();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                task();
                return result.get();
            }
            tasks_.push_back(std::move(task));
        }
        changed_.notify_one();
        return result.get();
    }

private:
    Spawner() {
        try {
            std::thread([this] { run(); }).detach();
            running_ = true;
        } catch (const std::system_error&) {
            // No thread: children are started from the calling thread.
        }
    }

    void run() {
        for (;;) {
            std::packaged_task<int()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return !tasks_.empty(); });
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::packaged_task<int()>> tasks_; // guarded by mutex_
    bool running_ = false;                        // guarded by mutex_ once the thread runs
};
#endif

} // namespace

std::string signal_name(int signal) {
    switch (signal) {
    case SIGHUP:
        return "SIGHUP";
    case SIGINT:
        return "SIGINT";
    case SIGQUIT:
        return "SIGQUIT";
    case SIGILL:
        return "SIGILL";
    case SIGTRAP:
        return "SIGTRAP";
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    case SIGFPE:
        return "SIGFPE";
    case SIGKILL:
        return "SIGKILL";
    case SIGUSR1:
        return "SIGUSR1";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGUSR2:
        return "SIGUSR2";
    case SIGPIPE:
        return "SIGPIPE";
    case SIGALRM:
        return "SIGALRM";
    case SIGTERM:
        return "SIGTERM";
    case SIGSYS:
        return "SIGSYS";
    case SIGXCPU:
        return "SIGXCPU";
    case SIGXFSZ:
        return "SIGXFSZ";
    default:
        return "signal " + std::to_string(signal);
    }
}

std::string describe(const ChildExit& exit) {
    if (exit.signal) {
        return "was ended by signal " + std::to_string(*exit.signal) + " (" +
               signal_name(*exit.signal) + ")";
    }
    if (exit.exit_code) {
        return "exited with code " + std::to_string(*exit.exit_code);
    }
    return "ended";
}

bool process_isolation_available() noexcept { return true; }

ChildProcess::Started ChildProcess::start(const std::filesystem::path& executable,
                                          const std::vector<std::string>& arguments) {
    const auto ends = open_channel_pair();
    Owned ours(ends.first), theirs(ends.second);
    if (!ours.move_above_3() || !theirs.move_above_3()) {
        refuse(executable, "cannot place its channel", errno);
    }

    FileActions actions;
    Attributes attributes;
    if (!actions.ok() || !attributes.ok()) {
        refuse(executable, "cannot prepare it", ENOMEM);
    }
    int error = ::posix_spawn_file_actions_adddup2(actions.get(), theirs.get(), 3);
    if (error == 0) {
        error = ::posix_spawn_file_actions_addopen(actions.get(), 0, "/dev/null", O_RDONLY, 0);
    }
#if defined(__APPLE__)
    // POSIX_SPAWN_CLOEXEC_DEFAULT closes everything not named here: standard output and error
    // are passed on as they are, when this process has them.
    for (const int descriptor : {1, 2}) {
        if (error == 0 && is_open(descriptor)) {
            error = ::posix_spawn_file_actions_addinherit_np(actions.get(), descriptor);
        }
    }
#endif
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t no_signals;
    sigemptyset(&no_signals);
    // SIGPIPE at its default disposition, whatever this process has (a process that serves the
    // control API ignores it, as cpp-httplib's server does): a module host's modules meet the same
    // SIGPIPE whether or not the tool that started it listens.
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    if (error == 0) {
        error = ::posix_spawnattr_setflags(attributes.get(), flags);
    }
    if (error == 0) {
        error = ::posix_spawnattr_setpgroup(attributes.get(), 0); // a group of its own
    }
    if (error == 0) {
        error = ::posix_spawnattr_setsigmask(attributes.get(), &no_signals);
    }
    if (error == 0) {
        error = ::posix_spawnattr_setsigdefault(attributes.get(), &defaults);
    }
    if (error != 0) {
        refuse(executable, "cannot prepare it", error);
    }

    const std::string program = path_to_utf8(executable);
    std::vector<std::string> owned_arguments;
    owned_arguments.reserve(arguments.size() + 1);
    owned_arguments.push_back(program);
    owned_arguments.insert(owned_arguments.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : owned_arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
#if defined(__linux__)
    error = Spawner::instance().spawn(&pid, program.c_str(), actions.get(), attributes.get(),
                                      argv.data(), process_environment());
#else
    error = ::posix_spawn(&pid, program.c_str(), actions.get(), attributes.get(), argv.data(),
                          process_environment());
#endif
    if (error != 0) {
        refuse(executable, "posix_spawn() failed", error);
    }
    Started started;
    started.process.reset(new ChildProcess(static_cast<std::int64_t>(pid)));
    started.channel = ours.release();
    return started; // `theirs` is closed here: only the child holds that end now
}

ChildProcess::ChildProcess(std::int64_t pid) noexcept : pid_(pid) {}

ChildProcess::~ChildProcess() {
    if (!exit_status()) {
        terminate();
        static_cast<void>(wait());
    }
}

std::int64_t ChildProcess::pid() const noexcept { return pid_; }

void ChildProcess::terminate() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (exit_) {
        return; // reaped: the number may belong to another process by now
    }
    // The process first: signalling a group is not one atomic step, and a process that sees a
    // child of its group die first could still exit on its own. Then the group, so that whatever
    // the process started goes too.
    static_cast<void>(::kill(static_cast<pid_t>(pid_), SIGKILL));
    static_cast<void>(::kill(-static_cast<pid_t>(pid_), SIGKILL));
}

void ChildProcess::poll_locked() {
    if (exit_) {
        return;
    }
    // Whether it has ended, without reaping it: until it is reaped, its number (and so its
    // group's) cannot go to another process, so what it left in its group can be killed safely.
    siginfo_t info{};
    int found = 0;
    do {
        info.si_pid = 0;
        found = ::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT);
    } while (found < 0 && errno == EINTR);
    if (found == 0 && info.si_pid == 0) {
        return; // still running
    }
    // macOS reports a stopped child too, although only ended ones were asked for: a child that is
    // stopped, traced or continued has not ended (and is not killed for it).
    if (found == 0 && (info.si_code == CLD_STOPPED || info.si_code == CLD_TRAPPED ||
                       info.si_code == CLD_CONTINUED)) {
        return;
    }
    if (found == 0) {
        static_cast<void>(::kill(-static_cast<pid_t>(pid_), SIGKILL));
    }
    int status = 0;
    pid_t reaped = 0;
    do {
        reaped = ::waitpid(static_cast<pid_t>(pid_), &status, found == 0 ? 0 : WNOHANG);
    } while (reaped < 0 && errno == EINTR);
    if (reaped == 0) {
        return; // not reaped after all (waitid() failed): look again later
    }
    ChildExit exit;
    if (reaped > 0) {
        if (WIFEXITED(status)) {
            exit.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit.signal = WTERMSIG(status);
        }
    }
    // reaped < 0 (ECHILD): someone else reaped it, or children are reaped automatically here;
    // it has ended, how is unknown.
    exit_ = exit;
}

std::optional<ChildExit> ChildProcess::wait(std::optional<std::chrono::nanoseconds> timeout) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    // waitpid() cannot wait with a deadline: look every few milliseconds, more often at first.
    auto pause = std::chrono::milliseconds(1);
    for (;;) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            poll_locked();
            if (exit_) {
                return exit_;
            }
        }
        if (timeout) {
            const auto waited = Clock::now() - start;
            if (waited >= *timeout) {
                return std::nullopt;
            }
            const auto remaining =
                std::chrono::duration_cast<Clock::duration>(*timeout) - waited; // positive
            std::this_thread::sleep_for(std::min<Clock::duration>(pause, remaining));
        } else {
            std::this_thread::sleep_for(pause);
        }
        pause = std::min(pause * 2, std::chrono::milliseconds(10));
    }
}

std::optional<ChildExit> ChildProcess::exit_status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return exit_;
}

} // namespace praenora::detail
