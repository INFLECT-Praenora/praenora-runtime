// Stopping a run on a signal: SIGINT and SIGTERM on POSIX, Ctrl+C, Ctrl+Break and closing the
// console on Windows. The first one asks the workflow being watched to stop (it ends as stopped,
// its modules finalized and destroyed); a second one ends the process at once with exit code 130.
// On POSIX, a signal that the process started with ignored (as a shell starts a command in the
// background) stays ignored. On Windows, closing the console, logging off and shutting down end
// the process once the handler returns: the handler then waits, at most 4 s (below the system's
// limit of about 5 s), until the watched run is over.
#ifndef PRAENORA_CLI_SIGNALS_HPP
#define PRAENORA_CLI_SIGNALS_HPP

#include <praenora/runtime/workflow.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace praenora::cli {

// One at a time. The handler only sets a flag (a lock-free std::atomic<std::sig_atomic_t>), or the
// second time calls _Exit(130); a watcher thread looks at the flag every 50 ms and passes a signal
// on with Workflow::request_stop().
class StopOnSignal {
public:
    // Installs the handlers (sigaction(), for each signal not inherited as ignored, or
    // SetConsoleCtrlHandler()) and starts the watcher.
    StopOnSignal();
    // Stops the watcher and restores the handlers that were there before.
    ~StopOnSignal();
    StopOnSignal(const StopOnSignal&) = delete;
    StopOnSignal& operator=(const StopOnSignal&) = delete;

    // The workflow that a signal stops from now on; nullptr for none, once the run of the one
    // watched before is over (on Windows this releases a console handler waiting for it). A
    // signal that arrived earlier reaches the workflow at once. Once this returns, the watcher no
    // longer uses the workflow watched before.
    void watch(Workflow* workflow);

private:
    void loop();

    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;            // guarded by mutex_
    Workflow* workflow_ = nullptr; // guarded by mutex_
    bool delivered_ = false;       // guarded by mutex_: the stop was requested
    std::thread watcher_;
};

} // namespace praenora::cli

#endif // PRAENORA_CLI_SIGNALS_HPP
