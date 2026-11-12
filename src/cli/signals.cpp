#include "signals.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

namespace praenora::cli {

namespace {

// Set by the handler when a signal arrives; read by the watcher thread. A lock-free atomic is
// safe in a signal handler and between threads alike; a volatile std::sig_atomic_t is only the
// former, and reading it from another thread is a data race.
std::atomic<std::sig_atomic_t> signalled{0};
static_assert(std::atomic<std::sig_atomic_t>::is_always_lock_free,
              "the signal handler may use only lock-free atomics");

// Async-signal-safe: it only sets the flag, or ends the process for a second signal.
void stop_requested() {
    if (signalled.exchange(1) != 0) {
        std::_Exit(130);
    }
}

#ifdef _WIN32
// A manual-reset event, set once the run that a signal stops is over (StopOnSignal::watch()).
// Created once and never closed: a handler thread may still wait on it while the process ends.
HANDLE run_over = nullptr;

BOOL WINAPI on_console_event(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        stop_requested();
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // The system ends the process once this returns, or about 5 s after the event: give the
        // run up to 4 s to wind down (its modules finalized and destroyed) before returning.
        stop_requested();
        if (run_over != nullptr) {
            WaitForSingleObject(run_over, 4000);
        }
        return TRUE;
    default:
        return FALSE;
    }
}
#else
void on_signal(int) { stop_requested(); }

struct sigaction previous_interrupt;
struct sigaction previous_terminate;
bool installed_interrupt = false;
bool installed_terminate = false;

// Installs `action` for `signal`, keeping the earlier action in `previous`, unless the process
// started with the signal ignored (as a shell starts a command in the background), which stays
// ignored, as in other tools. True when installed.
bool install(int signal, const struct sigaction& action, struct sigaction& previous) {
    if (sigaction(signal, nullptr, &previous) != 0) {
        return false;
    }
    if ((previous.sa_flags & SA_SIGINFO) == 0 && previous.sa_handler == SIG_IGN) {
        return false;
    }
    return sigaction(signal, &action, &previous) == 0;
}
#endif

} // namespace

StopOnSignal::StopOnSignal() {
#ifdef _WIN32
    if (run_over == nullptr) {
        run_over = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    } else {
        ResetEvent(run_over); // an earlier StopOnSignal set it
    }
    SetConsoleCtrlHandler(&on_console_event, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = &on_signal;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    action.sa_flags = SA_RESTART;
    installed_interrupt = install(SIGINT, action, previous_interrupt);
    installed_terminate = install(SIGTERM, action, previous_terminate);
#endif
    watcher_ = std::thread([this] { loop(); });
}

StopOnSignal::~StopOnSignal() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    watcher_.join();
#ifdef _WIN32
    if (run_over != nullptr) {
        SetEvent(run_over); // no run to wait for any more
    }
    SetConsoleCtrlHandler(&on_console_event, FALSE);
#else
    if (installed_interrupt) {
        sigaction(SIGINT, &previous_interrupt, nullptr);
    }
    if (installed_terminate) {
        sigaction(SIGTERM, &previous_terminate, nullptr);
    }
#endif
}

void StopOnSignal::watch(Workflow* workflow) {
    const std::lock_guard<std::mutex> lock(mutex_);
#ifdef _WIN32
    if (run_over != nullptr) {
        if (workflow != nullptr) {
            ResetEvent(run_over); // a new run: a console close waits for it
        } else if (workflow_ != nullptr) {
            SetEvent(run_over); // the watched run is over: a waiting console handler may return
        }
    }
#endif
    workflow_ = workflow;
    delivered_ = false;
    if (workflow_ != nullptr && signalled.load() != 0) {
        workflow_->request_stop();
        delivered_ = true;
    }
}

void StopOnSignal::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
        wake_.wait_for(lock, std::chrono::milliseconds(50), [this] { return quit_; });
        if (signalled.load() != 0 && workflow_ != nullptr && !delivered_) {
            workflow_->request_stop(); // lock-free; the workflow outlives this, see watch()
            delivered_ = true;
        }
    }
}

} // namespace praenora::cli
