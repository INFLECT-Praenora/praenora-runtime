// One live instance of a module, bound to one workflow step: the lifecycle state machine, the
// host API the module sees, and the deadlines of its calls.
//
// Threading. Every call into the module (create through destroy, diagnostics and self_test
// included) runs on the instance's own worker thread (instance/worker.hpp), named
// praenora:<step>, never two at a time. One controlling thread makes the public calls, which block
// until the module returns or its time is up; request_cancel() and state() may be called from any
// thread. A control call made while another one is in progress is refused: create, initialize,
// invoke, finalize and self_test throw praenora::Error(INVALID_STATE), destroy() does nothing but
// log a warning, and diagnostics() returns nothing.
//
// Cancellation is per call. The host API's cancel_requested() is nonzero only while the call in
// flight has passed its deadline or had a stop requested (request_cancel()) while it ran; every
// call starts with it clear, so the calls that follow a stop (finalize, destroy, diagnostics) run
// normally. Stopping before the next call is the caller's job; a caller's stop flag
// (InstanceOptions::stop_flag) also stops a create, initialize or invoke that starts while it is
// set, which closes the gap between the caller's last look at the flag and the call's start.
//
// Deadlines. A call gets `invoke_timeout` (invoke) or `lifecycle_timeout` (all others); a timeout
// of 0 or less means no deadline. When the deadline passes, cancel_requested() starts returning
// nonzero for that call and the caller waits `cancel_grace` more (no limit when it is 0 or less):
//   - the call returns within that time: outcome timed_out (whatever status it returned);
//   - it does not: outcome unresponsive. The instance is abandoned for good: its thread is
//     detached and keeps running the call, its library is pinned, and its host API stays valid
//     for the rest of the process, without runtime services. It is never finalized or destroyed.
//
// States (praenora::StepState): loaded -> created -> ready -> finalized -> destroyed, `busy` while
// a call runs. A failed, timed-out or cancelled create, initialize or finalize leaves `failed`;
// invoke returns to `ready` whatever its outcome, so an initialized instance is always finalized
// at the end. Calls made in a state that does not allow them throw praenora::Error(INVALID_STATE).
#ifndef PRAENORA_RUNTIME_INSTANCE_MODULE_INSTANCE_HPP
#define PRAENORA_RUNTIME_INSTANCE_MODULE_INSTANCE_HPP

#include "instance/services.hpp"
#include "loader/loaded_module.hpp"

#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/sdk/abi.h>

#include <spdlog/logger.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::detail {

class InstanceWorker;
struct HostContext;

enum class CallOutcome { succeeded, failed, timed_out, cancelled, unresponsive };

// "succeeded", "failed", "timed_out", "cancelled" or "unresponsive".
std::string to_string(CallOutcome outcome);

// What one call into the module came to.
// - outcome: succeeded (status OK); cancelled (status CANCELLED, with a stop requested during that
//   call); failed (any other status, or OK from a create that produced no instance, reported as
//   INTERNAL); timed_out; unresponsive (status is then PRAENORA_STATUS_TIMEOUT: the call never
//   returned).
// - output: what the module wrote to the call's output sink (result, diagnostics);
//   error_json: what it wrote to its error sink (normally {"code","message","details"}).
// - duration: from the call being posted until the module returned (or, when unresponsive,
//   until the instance was abandoned).
struct CallResult {
    CallOutcome outcome = CallOutcome::succeeded;
    praenora_status status = PRAENORA_STATUS_OK;
    std::string output, error_json;
    std::chrono::nanoseconds duration{0};
};

struct InstanceOptions {
    std::string step; // the step id, used in logs and the worker's name
    std::chrono::nanoseconds lifecycle_timeout{std::chrono::seconds(60)},
        invoke_timeout{std::chrono::seconds(300)}, cancel_grace{std::chrono::seconds(5)};
    // Receives the module's log messages, with the attributes run.fingerprint (when set), tick
    // (during an invocation), step and module (name@version). Usually the logger module.<step>.
    // Without one, messages are dropped and log_enabled() reports 0.
    std::shared_ptr<spdlog::logger> logger;
    // The runtime's built-in services, served through get_service() to every module whatever its
    // manifest declares; NULL offers none. Must outlive the instance, or at least its abandonment
    // or withdraw_services().
    const ServiceRegistry* services = nullptr;
    // The provider services this instance's module may use: those its manifest declares that
    // resolved for the run (docs/specs/runtime-services.md). get_service() looks here first, then
    // in `services`. Provider tables live until the process ends; the instance keeps a copy.
    ServiceRegistry provider_services;
    // Each of a call's sinks refuses writes beyond this many bytes with
    // PRAENORA_STATUS_RESOURCE_EXHAUSTED.
    std::size_t max_output_bytes = 256u << 20;
    // The run's fingerprint; when set, the module's log messages also carry the attribute
    // run.fingerprint.
    std::string run_fingerprint;
    // The caller's stop flag, or NULL; the engine passes the workflow's. A create, initialize or
    // invoke that starts while it is set is stopped at once, as if request_cancel() had come
    // during the call. finalize, destroy and diagnostics always start clear. Must outlive the
    // instance.
    const std::atomic<bool>* stop_flag = nullptr;
};

class ModuleInstance {
public:
    // Starts the worker thread; nothing is called in the module yet (state loaded). Throws
    // praenora::Error(INVALID_ARGUMENT) without a module.
    ModuleInstance(std::shared_ptr<LoadedModule> module, InstanceOptions options);
    // Finalizes the instance if it is ready, destroys it if it exists (each call bounded by the
    // lifecycle timeout and the cancellation grace), then joins the worker thread. An unresponsive
    // instance is left alone. Never throws.
    ~ModuleInstance();
    ModuleInstance(const ModuleInstance&) = delete;
    ModuleInstance& operator=(const ModuleInstance&) = delete;

    // In state loaded. The module's instance exists afterwards if create returned OK with one,
    // even after its deadline; then destroy() frees it later.
    CallResult create(const std::string& create_json);
    // In state created.
    CallResult initialize(const std::string& initialize_json);
    // In state ready. The module's log messages during the call carry `tick`.
    CallResult invoke(const std::string& invocation_json, std::uint64_t tick);
    // In state ready.
    CallResult finalize();
    // Calls the module's destroy() once, if its instance exists, and the state becomes destroyed.
    // Does nothing for an unresponsive or destroyed instance, or when no module instance exists;
    // while another call is in progress it does nothing but log a warning.
    void destroy() noexcept;
    // The module's diagnostics document, called between calls with the lifecycle timeout. nullopt
    // when the module has no diagnostics(), when no module instance exists or the instance is
    // busy or unresponsive, or when the call does not succeed; it never throws for a state.
    std::optional<std::string> diagnostics();
    // Calls the module's self_test() with this instance's host API, for the contract checks: in
    // state loaded (before create), with the invoke timeout, and the state is loaded again
    // afterwards (unless the call became unresponsive). The output is the self-test report.
    // Throws praenora::Error: INVALID_STATE in another state, UNSUPPORTED when the module has no
    // self_test().
    CallResult self_test();

    // Asks the call in flight to stop: cancel_requested() reports nonzero to the module until that
    // call returns, and if it returns CANCELLED its outcome is cancelled. Without a call in
    // flight it does nothing; later calls start with the flag clear (but see stop_flag). Lock-free
    // and async-signal-safe; callable from any thread, also by several threads at once.
    void request_cancel() noexcept;

    // Stops serving runtime services to the module, as an abandoned (unresponsive) instance does:
    // its get_service() returns NULL from now on for every service, provider services included,
    // also on the module's own threads, so the registry (InstanceOptions::services) need not
    // outlive the instance any more once this has returned. For an instance that is kept, never
    // to be called again, while the run it belonged to goes away. Thread-safe; never throws.
    void withdraw_services() noexcept;

    StepState state() const noexcept; // thread-safe
    const LoadedModule& module() const noexcept;

private:
    struct Call;
    class Claim;
    using Body = std::function<praenora_status(Call&)>;

    // Moves the state from one of `allowed` to busy and returns the state it left; throws
    // praenora::Error(INVALID_STATE) for any other state.
    StepState begin(const char* call, std::initializer_list<StepState> allowed);
    // Whether the caller's stop flag stops a call as it starts.
    enum class StopFlag { applies, ignored };
    // Runs `body` on the worker thread with a copy of `input` as the call's document, and waits
    // for it as the deadlines say. Marks `claim` once the call is posted, so an exception before
    // that restores the state. On unresponsive the instance is abandoned before this returns.
    // When `created` is given, it receives the instance a create() call produced.
    CallResult run(Claim& claim, const char* call, std::chrono::nanoseconds timeout,
                   StopFlag stop_flag, const std::string& input, Body body,
                   praenora_instance** created = nullptr);
    void abandon(const char* call) noexcept;
    // Stops serving runtime services, built-in and provider ones alike: what withdraw_services()
    // and an abandonment both do. Takes the lock that get_service() takes.
    void stop_serving() noexcept;
    // Logs an event about the instance; never throws (the message is copied inside).
    void log(LogLevel level, std::string_view message, const char* event) const noexcept;

    std::shared_ptr<LoadedModule> module_;
    InstanceOptions options_;
    std::string identity_; // name@version
    std::shared_ptr<HostContext> host_;
    std::unique_ptr<InstanceWorker> worker_;
    std::atomic<StepState> state_{StepState::loaded};
    praenora_instance* instance_ = nullptr; // the module's instance, while it exists
    std::uint64_t calls_ = 0;               // calls posted so far; numbers them from 1
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_INSTANCE_MODULE_INSTANCE_HPP
