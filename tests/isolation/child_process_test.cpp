// Starting, watching and ending a child process (isolation/child_process.hpp), with /bin/sh as the
// child: exit codes and signals, the channel as descriptor 3 and /dev/null as standard input, a
// process group of its own that terminate() kills whole, waits with a deadline, a stopped child
// that has not ended, and no zombie left behind.
#include "isolation/child_process.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::ChildExit;
using praenora::detail::ChildProcess;
using praenora::detail::describe;
using praenora::detail::signal_name;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

// /bin/sh -c `script`, started as a child.
ChildProcess::Started shell(const std::string& script) {
    return ChildProcess::start("/bin/sh", {"-c", script});
}

// What the child writes to its descriptor 3 until it closes it (or 10 s pass).
std::string read_channel(int channel) {
    std::string text;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    char buffer[256];
    while (Clock::now() < deadline) {
        const ssize_t n = ::recv(channel, buffer, sizeof buffer, 0);
        if (n <= 0) {
            break;
        }
        text.append(buffer, static_cast<std::size_t>(n));
    }
    return text;
}

// Reads one line from the channel (without its newline).
std::string read_line(int channel) {
    std::string line;
    char c = 0;
    while (::recv(channel, &c, 1, 0) == 1 && c != '\n') {
        line.push_back(c);
    }
    return line;
}

// Whether process `pid` is gone (no process, or only a zombie that is not ours), within 5 s.
bool gone_within_5s(pid_t pid) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        if (::kill(pid, 0) == -1 && errno == ESRCH) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return false;
}

class Descriptor {
public:
    explicit Descriptor(int descriptor) noexcept : descriptor_(descriptor) {}
    ~Descriptor() { ::close(descriptor_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

} // namespace

TEST(ChildProcess, ReportsTheExitCode) {
    auto started = shell("exit 3");
    const Descriptor channel(started.channel);
    const std::optional<ChildExit> exit = started.process->wait();
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exit_code, std::optional<int>(3));
    EXPECT_FALSE(exit->signal.has_value());
    EXPECT_EQ(describe(*exit), "exited with code 3");
    EXPECT_EQ(started.process->exit_status()->exit_code, std::optional<int>(3));
}

TEST(ChildProcess, ReportsTheSignal) {
    auto started = shell("kill -SEGV $$");
    const Descriptor channel(started.channel);
    const std::optional<ChildExit> exit = started.process->wait();
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->signal, std::optional<int>(SIGSEGV));
    EXPECT_FALSE(exit->exit_code.has_value());
    EXPECT_EQ(describe(*exit), "was ended by signal " + std::to_string(SIGSEGV) + " (SIGSEGV)");
    EXPECT_EQ(signal_name(SIGABRT), "SIGABRT");
    EXPECT_EQ(signal_name(SIGKILL), "SIGKILL");
    EXPECT_EQ(signal_name(1000), "signal 1000");
}

TEST(ChildProcess, PassesTheChannelAsDescriptor3AndDevNullAsInput) {
    auto started = shell("read line; echo \"[$line] $?\" >&3; echo hello >&3");
    const Descriptor channel(started.channel);
    EXPECT_EQ(read_channel(channel.get()), "[] 1\nhello\n");
    EXPECT_EQ(started.process->wait()->exit_code, std::optional<int>(0));
}

TEST(ChildProcess, RunsInAProcessGroupOfItsOwn) {
    auto started = shell("echo $$ >&3; read line <&3");
    const Descriptor channel(started.channel);
    const pid_t pid = static_cast<pid_t>(std::stoll(read_line(channel.get())));
    EXPECT_EQ(pid, static_cast<pid_t>(started.process->pid()));
    EXPECT_EQ(::getpgid(pid), pid);
    EXPECT_NE(::getpgid(pid), ::getpgid(0));
    ASSERT_EQ(::send(channel.get(), "\n", 1, 0), 1);
    EXPECT_EQ(started.process->wait()->exit_code, std::optional<int>(0));
}

#if defined(__APPLE__)
TEST(ChildProcess, PassesNoOtherDescriptor) {
    // A descriptor of this process that is not close-on-exec stays out of the child.
    int ends[2];
    ASSERT_EQ(::pipe(ends), 0);
    const Descriptor read_end(ends[0]), write_end(ends[1]);
    const std::string probe = "if (eval ': >&" + std::to_string(write_end.get()) +
                              "') 2>/dev/null; then echo open >&3; else echo closed >&3; fi";
    auto started = shell(probe);
    const Descriptor channel(started.channel);
    EXPECT_EQ(read_channel(channel.get()), "closed\n");
    started.process->wait();
}
#endif

TEST(ChildProcess, WaitsWithADeadline) {
    auto started = shell("sleep 30");
    const Descriptor channel(started.channel);
    const auto start = Clock::now();
    EXPECT_FALSE(started.process->wait(milliseconds(100)).has_value());
    EXPECT_GE(Clock::now() - start, milliseconds(100));
    EXPECT_FALSE(started.process->exit_status().has_value());

    started.process->terminate();
    const std::optional<ChildExit> exit = started.process->wait(std::chrono::seconds(10));
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->signal, std::optional<int>(SIGKILL));
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(10));
}

TEST(ChildProcess, AStoppedChildHasNotEnded) {
    // macOS's waitid() reports a stopped child even when asked for ended ones only; a stopped
    // child has not ended, and is not killed for it: continued, it runs on and exits as usual.
    auto started = shell("echo ready >&3; read line <&3; exit 4");
    const Descriptor channel(started.channel);
    ASSERT_EQ(read_line(channel.get()), "ready");
    const auto pid = static_cast<pid_t>(started.process->pid());
    ASSERT_EQ(::kill(pid, SIGSTOP), 0);
    EXPECT_FALSE(started.process->wait(milliseconds(300)).has_value());
    EXPECT_FALSE(started.process->exit_status().has_value());
    ASSERT_EQ(::kill(pid, SIGCONT), 0);
    ASSERT_EQ(::send(channel.get(), "\n", 1, 0), 1);
    const std::optional<ChildExit> exit = started.process->wait(std::chrono::seconds(10));
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exit_code, std::optional<int>(4));

    // A stopped child that is killed has ended, by the kill's signal.
    auto stopped = shell("sleep 30");
    const Descriptor stopped_channel(stopped.channel);
    ASSERT_EQ(::kill(static_cast<pid_t>(stopped.process->pid()), SIGSTOP), 0);
    EXPECT_FALSE(stopped.process->wait(milliseconds(100)).has_value());
    stopped.process->terminate();
    const std::optional<ChildExit> killed = stopped.process->wait(std::chrono::seconds(10));
    ASSERT_TRUE(killed.has_value());
    EXPECT_EQ(killed->signal, std::optional<int>(SIGKILL));
}

TEST(ChildProcess, TerminateKillsTheProcessGroup) {
    // The shell's background job stays in the shell's process group.
    auto started = shell("sleep 100 & echo $! >&3; wait");
    const Descriptor channel(started.channel);
    const pid_t grandchild = static_cast<pid_t>(std::stoll(read_line(channel.get())));
    ASSERT_GT(grandchild, 0);
    EXPECT_EQ(::kill(grandchild, 0), 0);

    started.process->terminate();
    EXPECT_EQ(started.process->wait()->signal, std::optional<int>(SIGKILL));
    EXPECT_TRUE(gone_within_5s(grandchild));
}

TEST(ChildProcess, WhatAProcessLeavesInItsGroupEndsWithIt) {
    // The shell exits at once, its background job would live on: reaping the shell ends it.
    auto started = shell("sleep 100 & echo $! >&3; exit 0");
    const Descriptor channel(started.channel);
    const pid_t grandchild = static_cast<pid_t>(std::stoll(read_line(channel.get())));
    ASSERT_GT(grandchild, 0);
    EXPECT_EQ(started.process->wait()->exit_code, std::optional<int>(0));
    EXPECT_TRUE(gone_within_5s(grandchild));
}

TEST(ChildProcess, LeavesNoZombie) {
    pid_t ended_pid = 0;
    {
        auto started = shell("exit 0");
        const Descriptor channel(started.channel);
        ended_pid = static_cast<pid_t>(started.process->pid());
        started.process->wait();
        int status = 0;
        EXPECT_EQ(::waitpid(ended_pid, &status, WNOHANG), -1);
        EXPECT_EQ(errno, ECHILD);
    }
    EXPECT_TRUE(gone_within_5s(ended_pid));

    // One still running when its object goes away is killed and waited for.
    pid_t running_pid = 0;
    {
        auto started = shell("sleep 100");
        const Descriptor channel(started.channel);
        running_pid = static_cast<pid_t>(started.process->pid());
    }
    int status = 0;
    EXPECT_EQ(::waitpid(running_pid, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
    EXPECT_TRUE(gone_within_5s(running_pid));
}

TEST(ChildProcess, ConcurrentWaitsAgree) {
    auto started = shell("sleep 0.2; exit 4");
    const Descriptor channel(started.channel);
    std::vector<std::optional<ChildExit>> exits(4);
    std::vector<std::thread> waiters;
    for (std::size_t i = 0; i < exits.size(); ++i) {
        waiters.emplace_back([&started, &exits, i] { exits[i] = started.process->wait(); });
    }
    for (auto& waiter : waiters) {
        waiter.join();
    }
    for (const auto& exit : exits) {
        ASSERT_TRUE(exit.has_value());
        EXPECT_EQ(exit->exit_code, std::optional<int>(4));
    }
}

TEST(ChildProcess, AnExecutableThatCannotStartIsModuleHostUnavailable) {
    try {
        static_cast<void>(ChildProcess::start("/nonexistent/praenora-module-host", {}));
        FAIL() << "the start did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::MODULE_HOST_UNAVAILABLE) << error.what();
        const nlohmann::json details = nlohmann::json::parse(error.details_json());
        EXPECT_EQ(details.at("reason"), "spawn");
        EXPECT_FALSE(details.at("system").get<std::string>().empty());
        EXPECT_NE(std::string(error.what()).find("/nonexistent/praenora-module-host"),
                  std::string::npos)
            << error.what();
    }
}

#endif // _WIN32
