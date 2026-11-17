#include "instance/module_instance.hpp"

#include "instance/worker.hpp"
#include "loader/output_sink.hpp"
#include "observability/logging.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/status.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

using Clock = std::chrono::steady_clock;

// HostContext::tick when no invocation is running.
constexpr std::uint64_t kNoTick = std::numeric_limits<std::uint64_t>::max();

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "request_cancel() must be lock-free to be async-signal-safe");

} // namespace

// Everything a module reaches through its host API pointer. Owned jointly by the instance and the
// tasks running on its worker thread; once the instance is abandoned (unresponsive) it is kept for
// the rest of the process, because the module may still use it.
//
// Cancellation is per call. Calls are numbered from 1; `in_flight` holds the number of the call
// that is running, set just before the call is posted and cleared once its result is collected,
// 0 between calls. A passed deadline or a stop request marks one call by its number, so the next
// call always starts with cancel_requested() clear.
struct HostContext {
    praenora_host_api api{};
    std::string step, module, run_fingerprint; // module: name@version
    std::shared_ptr<spdlog::logger> logger;
    std::atomic<std::uint64_t> in_flight{0};
    std::atomic<std::uint64_t> deadline_passed{0}; // the call whose deadline has passed
    std::atomic<std::uint64_t> stop_requested{0};  // the call a stop was requested for
    std::atomic<std::uint64_t> tick{kNoTick};      // the tick of the invocation running, if any
    std::mutex services_mutex;
    const ServiceRegistry* services = nullptr;       // guarded; NULL once abandoned
    ServiceRegistry provider_services;               // fixed; served only while `serving`
    bool serving = true;                             // guarded; false once withdrawn
    praenora_instance* abandoned_instance = nullptr; // guarded; kept reachable once abandoned
};

// One call into the module, shared by the caller and the task on the worker thread. The task sets
// everything below `mutex` and then `done`, under the mutex; the caller reads them only after it
// has seen `done`.
struct ModuleInstance::Call {
    Call(const std::string& document, std::size_t limit)
        : input(document), output(limit), error(limit) {}

    const std::string input; // the call's document, owned here for as long as the call runs
    OutputSink output, error;
    praenora_instance* created = nullptr; // what create() produced
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    praenora_status status = PRAENORA_STATUS_OK;
    Clock::time_point end{};
};

// Holds the instance's state at busy for one call: unless the call reaches the module (reached()),
// the state it left is restored when the claim ends, for example when building the call throws.
class ModuleInstance::Claim {
public:
    Claim(std::atomic<StepState>& state, StepState previous) noexcept
        : state_(state), previous_(previous) {}
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
    ~Claim() {
        if (!reached_) {
            state_.store(previous_);
        }
    }

    void reached() noexcept { reached_ = true; }

private:
    std::atomic<StepState>& state_;
    StepState previous_;
    bool reached_ = false;
};

namespace {

praenora_bytes bytes(const std::string& text) noexcept {
    return praenora_bytes{text.data(), text.size()};
}

// The attributes of an event about this instance: run.fingerprint (when known), tick (during an
// invocation), step and module.
std::vector<LogAttribute> attributes_of(const HostContext& host) {
    std::vector<LogAttribute> attributes;
    if (!host.run_fingerprint.empty()) {
        attributes.push_back({"run.fingerprint", host.run_fingerprint});
    }
    const std::uint64_t tick = host.tick.load();
    if (tick != kNoTick) {
        attributes.push_back({"tick", tick});
    }
    attributes.push_back({"step", host.step});
    attributes.push_back({"module", host.module});
    return attributes;
}

HostContext* host_of(void* context) noexcept { return static_cast<HostContext*>(context); }

// Marks call `call` as stopped. The mark only ever moves forward, so a canceller that read an
// earlier call's number and stalled cannot take the stop back from a later call. A lock-free
// compare-exchange loop: async-signal-safe.
void mark_stop(HostContext& host, std::uint64_t call) noexcept {
    std::uint64_t seen = host.stop_requested.load();
    while (seen < call && !host.stop_requested.compare_exchange_weak(seen, call)) {
    }
}

// The host API callbacks. Thread-safe; they never throw into module code.

void PRAENORA_CALL host_log(void* context, std::int32_t level, const char* message,
                            std::size_t size) noexcept {
    const HostContext* host = host_of(context);
    if (host == nullptr || host->logger == nullptr || (message == nullptr && size != 0)) {
        return;
    }
    const LogLevel log_level = log_level_from_abi(level);
    try {
        if (!log_enabled(*host->logger, log_level)) {
            return;
        }
        const std::string_view text =
            message == nullptr ? std::string_view() : std::string_view(message, size);
        log_event(*host->logger, log_level, text, attributes_of(*host));
    } catch (...) {
        // The message is lost.
    }
}

std::int32_t PRAENORA_CALL host_log_enabled(void* context, std::int32_t level) noexcept {
    const HostContext* host = host_of(context);
    return host != nullptr && host->logger != nullptr &&
                   log_enabled(*host->logger, log_level_from_abi(level))
               ? 1
               : 0;
}

std::int32_t PRAENORA_CALL host_cancel_requested(void* context) noexcept {
    const HostContext* host = host_of(context);
    if (host == nullptr) {
        return 0;
    }
    const std::uint64_t call = host->in_flight.load();
    return call != 0 &&
                   (host->deadline_passed.load() == call || host->stop_requested.load() == call)
               ? 1
               : 0;
}

const void* PRAENORA_CALL host_get_service(void* context, const char* name, std::size_t name_size,
                                           std::uint32_t major) noexcept {
    HostContext* host = host_of(context);
    if (host == nullptr || name == nullptr) {
        return nullptr;
    }
    const std::string_view service(name, name_size);
    try {
        const void* table = nullptr;
        {
            // The provider services the module declared first, then the built-in ones.
            const std::lock_guard<std::mutex> lock(host->services_mutex);
            if (host->serving) {
                table = host->provider_services.find(service, major);
                if (table == nullptr && host->services != nullptr) {
                    table = host->services->find(service, major);
                }
            }
        }
        if (host->logger != nullptr && host->logger->should_log(spdlog::level::debug)) {
            auto attributes = attributes_of(*host);
            attributes.push_back({"event", "service.lookup"});
            log_event(*host->logger, LogLevel::debug,
                      "runtime service " + std::string(service) + "@" + std::to_string(major) +
                          (table != nullptr ? " provided" : " is not available"),
                      attributes);
        }
        return table;
    } catch (...) {
        return nullptr;
    }
}

// The deadline `wait` after `start`, or nullopt for none: `wait` is 0 or less, or beyond any
// clock reading.
std::optional<Clock::time_point> time_after(Clock::time_point start,
                                            std::chrono::nanoseconds wait) {
    if (wait <= std::chrono::nanoseconds::zero() || wait >= Clock::time_point::max() - start) {
        return std::nullopt;
    }
    return start + std::chrono::duration_cast<Clock::duration>(wait);
}

// Keeps an abandoned instance's host API valid for the rest of the process: a detached thread or
// the module's own threads may still call it. Deliberately never freed (the list is reachable from
// a static pointer, so leak checkers do not report it).
void keep_forever(std::shared_ptr<HostContext> host) noexcept {
    static auto* const mutex = new std::mutex;
    static auto* const kept = new std::vector<std::shared_ptr<HostContext>>;
    try {
        const std::lock_guard<std::mutex> lock(*mutex);
        kept->push_back(host);
    } catch (...) {
        // Out of memory: hold on to it anyway.
        static_cast<void>(new (std::nothrow) std::shared_ptr<HostContext>(std::move(host)));
    }
}

std::string shown(std::chrono::nanoseconds duration) {
    const auto ns = duration.count();
    if (ns % 1'000'000'000 == 0) {
        return std::to_string(ns / 1'000'000'000) + " s";
    }
    if (ns % 1'000'000 == 0) {
        return std::to_string(ns / 1'000'000) + " ms";
    }
    return std::to_string(ns) + " ns";
}

// An error document written by the host about the module.
std::string host_error(praenora_status code, const std::string& message) {
    return nlohmann::json{
        {"code", code}, {"message", message}, {"details", nlohmann::json::object()}}
        .dump();
}

} // namespace

std::string to_string(CallOutcome outcome) {
    switch (outcome) {
    case CallOutcome::succeeded:
        return "succeeded";
    case CallOutcome::failed:
        return "failed";
    case CallOutcome::timed_out:
        return "timed_out";
    case CallOutcome::cancelled:
        return "cancelled";
    case CallOutcome::unresponsive:
        return "unresponsive";
    }
    return "unknown";
}

ModuleInstance::ModuleInstance(std::shared_ptr<LoadedModule> module, InstanceOptions options)
    : module_(std::move(module)), options_(std::move(options)) {
    if (module_ == nullptr) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "a module instance needs a loaded module");
    }
    identity_ = module_->manifest().name + "@" + to_string(module_->manifest().version);

    host_ = std::make_shared<HostContext>();
    host_->step = options_.step;
    host_->module = identity_;
    host_->run_fingerprint = options_.run_fingerprint;
    host_->logger = options_.logger;
    host_->services = options_.services;
    host_->provider_services = options_.provider_services;
    praenora_host_api& api = host_->api;
    api.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
    api.abi_major = PRAENORA_ABI_MAJOR;
    api.abi_minor = PRAENORA_ABI_MINOR;
    api.context = host_.get();
    api.log = &host_log;
    api.log_enabled = &host_log_enabled;
    api.cancel_requested = &host_cancel_requested;
    api.get_service = &host_get_service;

    worker_ = std::make_unique<InstanceWorker>("praenora:" + options_.step);
}

ModuleInstance::~ModuleInstance() {
    if (state_.load() == StepState::ready) {
        try {
            static_cast<void>(finalize());
        } catch (...) {
            // Destructors never throw; destroy() still runs.
        }
    }
    destroy();
    if (worker_ != nullptr) {
        worker_->join(); // nothing to join once abandoned: the worker is detached
    }
}

StepState ModuleInstance::begin(const char* call, std::initializer_list<StepState> allowed) {
    StepState current = state_.load();
    for (;;) {
        if (std::find(allowed.begin(), allowed.end(), current) == allowed.end()) {
            std::string needed;
            for (const StepState state : allowed) {
                needed += (needed.empty() ? "" : " or ") + to_string(state);
            }
            throw Error(ErrorCode::INVALID_STATE, "step '" + options_.step + "' (module " +
                                                      identity_ + "): " + call +
                                                      " is not allowed while the instance is " +
                                                      to_string(current) + "; it needs " + needed);
        }
        if (state_.compare_exchange_weak(current, StepState::busy)) {
            return current;
        }
    }
}

CallResult ModuleInstance::run(Claim& claim, const char* call_name,
                               std::chrono::nanoseconds timeout, StopFlag stop_flag,
                               const std::string& input, Body body, praenora_instance** created) {
    auto call = std::make_shared<Call>(input, options_.max_output_bytes);
    const std::uint64_t number = ++calls_;
    host_->in_flight.store(number);
    // In flight first, the flag second: a stop set after this read finds the call in flight
    // through request_cancel(), and one set before it is seen here.
    if (stop_flag == StopFlag::applies && options_.stop_flag != nullptr &&
        options_.stop_flag->load()) {
        mark_stop(*host_, number);
    }
    const Clock::time_point start = Clock::now();
    try {
        // The task shares the call, the host API and the library, so they stay valid for as
        // long as the module runs, even if this instance is abandoned and destroyed meanwhile.
        worker_->post([call, host = host_, module = module_, body = std::move(body)] {
            static_cast<void>(host);
            static_cast<void>(module);
            praenora_status status = PRAENORA_STATUS_INTERNAL;
            try {
                status = body(*call);
            } catch (...) {
                status = PRAENORA_STATUS_INTERNAL;
            }
            {
                const std::lock_guard<std::mutex> lock(call->mutex);
                call->status = status;
                call->end = Clock::now();
                call->done = true;
            }
            call->finished.notify_all();
        });
    } catch (...) {
        host_->in_flight.store(0);
        throw; // the claim restores the state
    }
    claim.reached();

    // Wait until the deadline; then raise the cancel flag and wait the grace period too.
    const auto deadline = time_after(start, timeout);
    bool returned = true;
    {
        std::unique_lock<std::mutex> lock(call->mutex);
        const auto done = [&call] { return call->done; };
        if (!deadline) {
            call->finished.wait(lock, done);
        } else if (!call->finished.wait_until(lock, *deadline, done)) {
            host_->deadline_passed.store(number);
            lock.unlock();
            log(LogLevel::warn,
                std::string(call_name) + " did not return within " + shown(timeout) +
                    "; cancellation requested",
                "call.deadline");
            lock.lock();
            const auto grace_end = time_after(*deadline, options_.cancel_grace);
            if (!grace_end) {
                call->finished.wait(lock, done);
            } else {
                returned = call->finished.wait_until(lock, *grace_end, done);
            }
        }
    }

    CallResult result;
    if (!returned) {
        result.outcome = CallOutcome::unresponsive;
        result.status = PRAENORA_STATUS_TIMEOUT;
        result.duration = Clock::now() - start;
        abandon(call_name); // the call stays in flight for good
        return result;
    }
    // The call is over: a stop requested from now on reaches no call. What the task wrote is ours.
    host_->in_flight.store(0);
    result.status = call->status;
    result.output = call->output.take();
    result.error_json = call->error.take();
    result.duration = call->end - start;
    if (created != nullptr) {
        *created = call->created;
    }
    if (deadline && call->end > *deadline) {
        result.outcome = CallOutcome::timed_out;
    } else if (result.status == PRAENORA_STATUS_OK) {
        result.outcome = CallOutcome::succeeded;
    } else if (result.status == PRAENORA_STATUS_CANCELLED &&
               host_->stop_requested.load() == number) {
        result.outcome = CallOutcome::cancelled;
    } else {
        result.outcome = CallOutcome::failed;
    }
    return result;
}

void ModuleInstance::abandon(const char* call_name) noexcept {
    state_.store(StepState::unresponsive);
    module_->pin();
    stop_serving(); // the registry may go away before the module stops using it
    {
        const std::lock_guard<std::mutex> lock(host_->services_mutex);
        host_->abandoned_instance = instance_;
    }
    keep_forever(host_);
    worker_->detach();
    try {
        log(LogLevel::error,
            std::string(call_name) + " did not return within the cancellation grace period of " +
                shown(options_.cancel_grace) +
                " after its deadline; the instance is unresponsive: its thread is detached and "
                "its library stays loaded",
            "instance.unresponsive");
    } catch (...) {
        // Out of memory while building the message; the state says it all.
    }
}

CallResult ModuleInstance::create(const std::string& create_json) {
    Claim claim(state_, begin("create", {StepState::loaded}));
    const praenora_module_api_v0* api = &module_->api();
    const praenora_host_api* host_api = &host_->api;
    praenora_instance* created = nullptr;
    CallResult result = run(
        claim, "create", options_.lifecycle_timeout, StopFlag::applies, create_json,
        [api, host_api](Call& call) {
            praenora_instance* instance = nullptr;
            const praenora_status status =
                api->create(host_api, bytes(call.input), &instance, call.error.sink());
            call.created = instance;
            return status;
        },
        &created);
    if (result.outcome == CallOutcome::unresponsive) {
        return result;
    }
    if (result.status == PRAENORA_STATUS_OK && created != nullptr) {
        instance_ = created; // it exists now, so destroy() frees it, even after a timeout
    } else if (result.outcome == CallOutcome::succeeded) {
        result.outcome = CallOutcome::failed;
        result.status = PRAENORA_STATUS_INTERNAL;
        result.error_json =
            host_error(PRAENORA_STATUS_INTERNAL,
                       "create returned OK without an instance (module " + identity_ + ")");
    }
    state_.store(result.outcome == CallOutcome::succeeded ? StepState::created : StepState::failed);
    return result;
}

CallResult ModuleInstance::initialize(const std::string& initialize_json) {
    Claim claim(state_, begin("initialize", {StepState::created}));
    const praenora_module_api_v0* api = &module_->api();
    praenora_instance* const instance = instance_;
    const CallResult result =
        run(claim, "initialize", options_.lifecycle_timeout, StopFlag::applies, initialize_json,
            [api, instance](Call& call) {
                return api->initialize(instance, bytes(call.input), call.error.sink());
            });
    if (result.outcome != CallOutcome::unresponsive) {
        state_.store(result.outcome == CallOutcome::succeeded ? StepState::ready
                                                              : StepState::failed);
    }
    return result;
}

CallResult ModuleInstance::invoke(const std::string& invocation_json, std::uint64_t tick) {
    Claim claim(state_, begin("invoke", {StepState::ready}));
    const praenora_module_api_v0* api = &module_->api();
    praenora_instance* const instance = instance_;
    HostContext* const host = host_.get();
    host->tick.store(tick);
    CallResult result;
    try {
        result = run(claim, "invoke", options_.invoke_timeout, StopFlag::applies, invocation_json,
                     [api, instance, host](Call& call) {
                         const praenora_status status = api->invoke(
                             instance, bytes(call.input), call.output.sink(), call.error.sink());
                         host->tick.store(kNoTick);
                         return status;
                     });
    } catch (...) {
        host->tick.store(kNoTick);
        throw;
    }
    // Whatever the outcome, the module's instance is still initialized: it stays ready, so it
    // is finalized at the end.
    if (result.outcome != CallOutcome::unresponsive) {
        state_.store(StepState::ready);
    }
    return result;
}

CallResult ModuleInstance::finalize() {
    Claim claim(state_, begin("finalize", {StepState::ready}));
    const praenora_module_api_v0* api = &module_->api();
    praenora_instance* const instance = instance_;
    const CallResult result =
        run(claim, "finalize", options_.lifecycle_timeout, StopFlag::ignored, std::string(),
            [api, instance](Call& call) { return api->finalize(instance, call.error.sink()); });
    if (result.outcome != CallOutcome::unresponsive) {
        state_.store(result.outcome == CallOutcome::succeeded ? StepState::finalized
                                                              : StepState::failed);
    }
    return result;
}

void ModuleInstance::destroy() noexcept {
    StepState current = state_.load();
    for (;;) {
        if (current == StepState::unresponsive || current == StepState::destroyed) {
            return;
        }
        if (current == StepState::busy) {
            log(LogLevel::warn, "destroy ignored: another call into the instance is in progress",
                "call.refused");
            return;
        }
        if (state_.compare_exchange_weak(current, StepState::busy)) {
            break;
        }
    }
    // The instance is ours now (busy), so instance_ can be read without racing another caller.
    Claim claim(state_, current);
    if (instance_ == nullptr) {
        return; // nothing was created, so there is nothing to destroy
    }
    const praenora_module_api_v0* api = &module_->api();
    praenora_instance* const instance = instance_;
    try {
        const CallResult result = run(claim, "destroy", options_.lifecycle_timeout,
                                      StopFlag::ignored, std::string(), [api, instance](Call&) {
                                          api->destroy(instance);
                                          return PRAENORA_STATUS_OK;
                                      });
        if (result.outcome != CallOutcome::unresponsive) {
            instance_ = nullptr;
            state_.store(StepState::destroyed);
        }
    } catch (...) {
        // destroy() could not even be posted; the instance stays, and a later destroy() may
        // succeed. The claim restores the state.
        log(LogLevel::error, "destroy could not be called", "call.failed");
    }
}

std::optional<std::string> ModuleInstance::diagnostics() {
    StepState current = state_.load();
    for (;;) {
        if (current == StepState::loaded || current == StepState::busy ||
            current == StepState::unresponsive || current == StepState::destroyed) {
            return std::nullopt;
        }
        if (state_.compare_exchange_weak(current, StepState::busy)) {
            break;
        }
    }
    // The instance is ours now (busy); the claim puts the state back when nothing is called.
    Claim claim(state_, current);
    const praenora_module_api_v0* api = &module_->api();
    if (instance_ == nullptr || api->diagnostics == nullptr) {
        return std::nullopt;
    }
    praenora_instance* const instance = instance_;
    CallResult result =
        run(claim, "diagnostics", options_.lifecycle_timeout, StopFlag::ignored, std::string(),
            [api, instance](Call& call) { return api->diagnostics(instance, call.output.sink()); });
    if (result.outcome == CallOutcome::unresponsive) {
        return std::nullopt;
    }
    state_.store(current);
    if (result.outcome != CallOutcome::succeeded) {
        return std::nullopt;
    }
    return std::move(result.output);
}

CallResult ModuleInstance::self_test() {
    Claim claim(state_, begin("self_test", {StepState::loaded}));
    const praenora_module_api_v0* api = &module_->api();
    if (api->self_test == nullptr) {
        throw Error(ErrorCode::UNSUPPORTED, "module " + identity_ + " has no self_test()");
    }
    const praenora_host_api* host_api = &host_->api;
    const CallResult result =
        run(claim, "self_test", options_.invoke_timeout, StopFlag::ignored, std::string(),
            [api, host_api](Call& call) { return api->self_test(host_api, call.output.sink()); });
    if (result.outcome != CallOutcome::unresponsive) {
        state_.store(StepState::loaded);
    }
    return result;
}

void ModuleInstance::request_cancel() noexcept {
    // Marks the call in flight, if any; a call that starts later has another number.
    const std::uint64_t call = host_->in_flight.load();
    if (call != 0) {
        mark_stop(*host_, call);
    }
}

void ModuleInstance::withdraw_services() noexcept { stop_serving(); }

void ModuleInstance::stop_serving() noexcept {
    // The lock that get_service() takes: a lookup running now finishes with the registry first.
    const std::lock_guard<std::mutex> lock(host_->services_mutex);
    host_->services = nullptr;
    host_->serving = false;
}

StepState ModuleInstance::state() const noexcept { return state_.load(); }

const LoadedModule& ModuleInstance::module() const noexcept { return *module_; }

void ModuleInstance::log(LogLevel level, std::string_view message,
                         const char* event) const noexcept {
    if (options_.logger == nullptr) {
        return;
    }
    try {
        auto attributes = attributes_of(*host_);
        attributes.push_back({"event", event});
        log_event(*options_.logger, level, message, attributes);
    } catch (...) {
        // The message is lost.
    }
}

} // namespace praenora::detail
