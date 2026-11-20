#include "instance/module_instance.hpp"

#include "core/deadline.hpp"
#include "instance/worker.hpp"
#include "loader/module_code.hpp"
#include "loader/output_sink.hpp"
#include "observability/logging.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/status.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string_view>
#include <tuple>
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
// tasks running on its worker thread (and, for a module in a module host, by the module's code,
// which reports what the module does there through the InstanceHost interface); once an instance
// in this process is abandoned (unresponsive) it is kept for the rest of the process, because the
// module may still use it.
//
// Cancellation is per call. Calls are numbered from 1; `in_flight` holds the number of the call
// that is running, set just before the call is posted and cleared once its result is collected,
// 0 between calls. A passed deadline or a stop request marks one call by its number, so the next
// call always starts with cancel_requested() clear.
struct HostContext final : InstanceHost {
    const praenora_host_api* host_api() const noexcept override { return &api; }
    void log(std::int32_t level, std::string_view message) noexcept override;
    std::int32_t log_threshold() const noexcept override;
    bool cancel_requested() const noexcept override;
    bool call_stopped() const noexcept override;
    void lookup(std::string_view service, std::uint32_t major, bool provided) noexcept override;

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
    // The lookups of the call `recording` (guarded; 0 when none is recorded), named
    // `recording_call`, of the invocation of `recording_tick` if any: one per service name,
    // major and whether provided, in order; `recorded` indexes them.
    std::uint64_t recording = 0;
    const char* recording_call = nullptr;
    std::optional<std::uint64_t> recording_tick;
    std::vector<ServiceLookup> lookups;
    std::map<std::tuple<std::string, std::uint32_t, bool>, std::size_t> recorded;
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
    std::optional<std::string> crash; // the crash of the module's module host, if it crashed
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
    // Whether the call reached the module: false for one refused or stopped before it started.
    bool called() const noexcept { return reached_; }

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

// Counts one lookup in the call being recorded; the caller holds services_mutex. Out of memory,
// the lookup goes unrecorded: the module gets its table all the same.
void record_lookup(HostContext& host, std::string_view service, std::uint32_t major,
                   bool provided) noexcept {
    try {
        auto key = std::make_tuple(std::string(service), major, provided);
        const auto found = host.recorded.find(key);
        if (found != host.recorded.end()) {
            ++host.lookups[found->second].count;
            return;
        }
        ServiceLookup lookup;
        lookup.phase = host.recording_call;
        lookup.tick = host.recording_tick;
        lookup.service = std::get<0>(key);
        lookup.major = major;
        lookup.provided = provided;
        lookup.count = 1;
        host.lookups.push_back(std::move(lookup));
        try {
            host.recorded.emplace(std::move(key), host.lookups.size() - 1);
        } catch (...) {
            host.lookups.pop_back();
            throw;
        }
    } catch (...) {
    }
}

// Logs one lookup (event service.lookup, debug). May throw (out of memory).
void log_lookup(const HostContext& host, std::string_view service, std::uint32_t major,
                bool provided) {
    if (host.logger != nullptr && host.logger->should_log(spdlog::level::debug)) {
        auto attributes = attributes_of(host);
        attributes.push_back({"event", "service.lookup"});
        log_event(*host.logger, LogLevel::debug,
                  "runtime service " + std::string(service) + "@" + std::to_string(major) +
                      (provided ? " provided" : " is not available"),
                  attributes);
    }
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
            if (host->recording != 0) {
                record_lookup(*host, service, major, table != nullptr);
            }
        }
        log_lookup(*host, service, major, table != nullptr);
        return table;
    } catch (...) {
        return nullptr;
    }
}

// The deadline `wait` after `start`, or nullopt for none: `wait` is 0 or less, or reaches beyond
// any clock reading (deadline_after() saturates there).
std::optional<Clock::time_point> time_after(Clock::time_point start,
                                            std::chrono::nanoseconds wait) {
    const Clock::time_point deadline = deadline_after(start, wait);
    if (wait <= std::chrono::nanoseconds::zero() || deadline == Clock::time_point::max()) {
        return std::nullopt;
    }
    return deadline;
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

// What a module in a module host does with its host API, handled as the same call on this
// process's host API would be.

void HostContext::log(std::int32_t level, std::string_view message) noexcept {
    host_log(this, level, message.data(), message.size());
}

std::int32_t HostContext::log_threshold() const noexcept {
    for (std::int32_t level = PRAENORA_LOG_TRACE; level <= PRAENORA_LOG_CRITICAL; ++level) {
        if (host_log_enabled(const_cast<HostContext*>(this), level) != 0) {
            return level;
        }
    }
    return PRAENORA_LOG_CRITICAL + 1;
}

bool HostContext::cancel_requested() const noexcept {
    return host_cancel_requested(const_cast<HostContext*>(this)) != 0;
}

bool HostContext::call_stopped() const noexcept {
    const std::uint64_t call = in_flight.load();
    return call != 0 && stop_requested.load() == call;
}

void HostContext::lookup(std::string_view service, std::uint32_t major, bool provided) noexcept {
    try {
        {
            const std::lock_guard<std::mutex> lock(services_mutex);
            if (recording != 0) {
                record_lookup(*this, service, major, provided);
            }
        }
        log_lookup(*this, service, major, provided);
    } catch (...) {
        // The lookup goes unlogged.
    }
}

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
    case CallOutcome::refused:
        return "refused";
    case CallOutcome::crashed:
        return "crashed";
    }
    return "unknown";
}

ModuleInstance::ModuleInstance(std::shared_ptr<LoadedModule> module, InstanceOptions options)
    : module_(std::move(module)), options_(std::move(options)) {
    if (module_ == nullptr) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "a module instance needs a loaded module");
    }
    identity_ = module_->manifest().name + "@" + to_string(module_->manifest().version);
    caller_ = options_.caller.empty() ? "step '" + options_.step + "'" : options_.caller;

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
    bool refused = false;
    if (release_terminated()) {
        // Its module host was killed with the module's instance in it: nothing to call.
    } else if (state_.load() == StepState::ready) {
        try {
            refused = finalize().outcome == CallOutcome::refused;
        } catch (...) {
            // Destructors never throw; destroy() still runs.
        }
    }
    // A ready instance is never destroyed before it is finalized: one whose finalize was refused
    // is kept as it is, like one whose destroy is refused. Its module instance still exists, and
    // may not be called.
    if (refused || destroy() == CallOutcome::refused) {
        keep();
    }
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
                               LibraryGate::Access access, const std::string& input, Body body,
                               praenora_instance** created, std::optional<std::uint64_t> tick) {
    // The library first (loader/library_gate.hpp): the call waits for its turn, which is not
    // part of its deadline, and is refused when it would have to wait for an abandoned call. The
    // caller's stop flag ends the wait of a call it applies to.
    LibraryGate::Grant grant =
        module_->gate().acquire(access, LibraryCall{identity_, call_name, caller_},
                                stop_flag == StopFlag::applies ? options_.stop_flag : nullptr);
    if (grant.lease == nullptr) {
        return not_called(call_name, std::move(grant.refused_by));
    }
    const std::shared_ptr<LibraryGate::Lease> lease = std::move(grant.lease);

    auto call = std::make_shared<Call>(input, options_.max_output_bytes);
    const std::uint64_t number = ++calls_;
    host_->in_flight.store(number);
    start_recording(number, call_name, tick);
    // In flight first, the flag second: a stop set after this read finds the call in flight
    // through request_cancel(), and one set before it is seen here.
    if (stop_flag == StopFlag::applies && options_.stop_flag != nullptr &&
        options_.stop_flag->load()) {
        mark_stop(*host_, number);
    }
    const Clock::time_point start = Clock::now();
    try {
        // The task shares the call, the host API, the library and the lease, so they stay valid
        // for as long as the module runs, even if this instance is abandoned and destroyed
        // meanwhile. The lease goes back as soon as the module returns, abandoned or not.
        worker_->post([call, host = host_, module = module_, lease, body = std::move(body)] {
            static_cast<void>(host);
            static_cast<void>(module);
            CodeStatus status{PRAENORA_STATUS_INTERNAL, {}};
            try {
                status = body(*call);
            } catch (...) {
                status = CodeStatus{PRAENORA_STATUS_INTERNAL, {}};
            }
            lease->release();
            {
                const std::lock_guard<std::mutex> lock(call->mutex);
                call->status = status.status;
                call->crash = std::move(status.crash);
                call->end = Clock::now();
                call->done = true;
            }
            call->finished.notify_all();
        });
    } catch (...) {
        host_->in_flight.store(0);
        finish_recording(false); // never posted: what its module's threads looked up is not its
        throw;                   // the claim restores the state
    }
    claim.reached();
    CallResult result;
    result.called = true;

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

    if (!returned && module_->runs_in_child()) {
        // The module host is killed, so the call returns: nothing is abandoned, pinned or kept.
        module_->code().terminate();
        {
            std::unique_lock<std::mutex> lock(call->mutex);
            call->finished.wait(lock, [&call] { return call->done; });
        }
        host_->in_flight.store(0);
        finish_recording(true);
        if (call->crash) {
            // It crashed before it could be killed: a crash, not an unresponsive call.
            result.outcome = CallOutcome::crashed;
            result.status = PRAENORA_STATUS_INTERNAL;
            result.details_json = *call->crash;
            result.duration = call->end - start;
            state_.store(StepState::crashed);
            return result;
        }
        result.outcome = CallOutcome::unresponsive;
        result.status = PRAENORA_STATUS_TIMEOUT;
        result.duration = Clock::now() - start;
        result.details_json = R"({"terminated":true})";
        state_.store(StepState::unresponsive);
        try {
            log(LogLevel::error,
                std::string(call_name) +
                    " did not return within the cancellation grace period of " +
                    shown(options_.cancel_grace) +
                    " after its deadline; the instance is unresponsive: its module host was "
                    "terminated",
                "instance.unresponsive");
        } catch (...) {
            // Out of memory while building the message; the state says it all.
        }
        return result;
    }
    if (!returned) {
        result.outcome = CallOutcome::unresponsive;
        result.status = PRAENORA_STATUS_TIMEOUT;
        result.duration = Clock::now() - start;
        lease->abandon(); // the library refuses what would wait for this call, until it returns
        // The lookups the call made so far; the rest are not recorded.
        finish_recording(true);
        abandon(call_name); // the call stays in flight for good
        return result;
    }
    // The call is over: a stop requested from now on reaches no call. What the task wrote is ours.
    host_->in_flight.store(0);
    finish_recording(true);
    if (call->crash) {
        // The module host ended or broke its channel: the call never returned.
        result.outcome = CallOutcome::crashed;
        result.status = PRAENORA_STATUS_INTERNAL;
        result.details_json = *call->crash;
        result.duration = call->end - start;
        state_.store(StepState::crashed);
        return result;
    }
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

CallResult ModuleInstance::not_called(const char* call_name,
                                      std::optional<LibraryCall> refused_by) {
    CallResult result;
    if (!refused_by) {
        result.outcome = CallOutcome::cancelled; // a stop ended the wait for the library
        result.status = PRAENORA_STATUS_CANCELLED;
        return result;
    }
    const std::string reason = refusal_reason(*refused_by);
    result.outcome = CallOutcome::refused;
    result.status = PRAENORA_STATUS_INVALID_STATE;
    result.error_json =
        host_error(result.status, std::string(call_name) + " was refused: " + reason);
    result.refused_by = refused_by;
    refused_by_ = std::move(refused_by);
    log(LogLevel::warn, std::string(call_name) + " was not called: " + reason, "library.refused");
    return result;
}

void ModuleInstance::keep() noexcept {
    module_->pin();
    stop_serving(); // the registry may go away before the module's own threads stop using it
    {
        const std::lock_guard<std::mutex> lock(host_->services_mutex);
        host_->abandoned_instance = instance_;
    }
    keep_forever(host_);
    try {
        log(LogLevel::warn,
            "the instance cannot be destroyed: " +
                (refused_by_ ? refusal_reason(*refused_by_) : std::string("its call was refused")) +
                "; it is kept for the rest of the process, never destroyed or freed",
            "instance.kept");
    } catch (...) {
        // Out of memory while building the message; the instance is kept all the same.
    }
}

CallResult ModuleInstance::create(const std::string& create_json) {
    Claim claim(state_, begin("create", {StepState::loaded}));
    ModuleCode* const code = &module_->code();
    const std::shared_ptr<InstanceHost> host = host_;
    praenora_instance* created = nullptr;
    CallResult result = run(
        claim, "create", options_.lifecycle_timeout, StopFlag::applies, module_->call_access(),
        create_json,
        [code, host](Call& call) {
            praenora_instance* instance = nullptr;
            CodeStatus status = code->create(host, bytes(call.input), &instance, call.error);
            call.created = instance;
            return status;
        },
        &created);
    if (!claim.called() || result.outcome == CallOutcome::unresponsive ||
        result.outcome == CallOutcome::crashed) {
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
    ModuleCode* const code = &module_->code();
    praenora_instance* const instance = instance_;
    const CallResult result =
        run(claim, "initialize", options_.lifecycle_timeout, StopFlag::applies,
            module_->call_access(), initialize_json, [code, instance](Call& call) {
                return code->initialize(instance, bytes(call.input), call.error);
            });
    if (claim.called() && result.outcome != CallOutcome::unresponsive &&
        result.outcome != CallOutcome::crashed) {
        state_.store(result.outcome == CallOutcome::succeeded ? StepState::ready
                                                              : StepState::failed);
    }
    return result;
}

CallResult ModuleInstance::invoke(const std::string& invocation_json, std::uint64_t tick) {
    Claim claim(state_, begin("invoke", {StepState::ready}));
    ModuleCode* const code = &module_->code();
    praenora_instance* const instance = instance_;
    HostContext* const host = host_.get();
    // The invocation's tick is set on the worker as the call starts, once it has its turn at the
    // library, and cleared as it returns.
    const CallResult result = run(
        claim, "invoke", options_.invoke_timeout, StopFlag::applies, module_->call_access(),
        invocation_json,
        [code, instance, host, tick](Call& call) {
            host->tick.store(tick);
            CodeStatus status = code->invoke(instance, bytes(call.input), call.output, call.error);
            host->tick.store(kNoTick);
            return status;
        },
        nullptr, tick);
    if (!claim.called()) {
        return result; // refused or stopped before it started; the claim restores ready
    }
    // Whatever the outcome, the module's instance is still initialized: it stays ready, so it
    // is finalized at the end (unless its module host is gone).
    if (result.outcome != CallOutcome::unresponsive && result.outcome != CallOutcome::crashed) {
        state_.store(StepState::ready);
    }
    return result;
}

CallResult ModuleInstance::finalize() {
    Claim claim(state_, begin("finalize", {StepState::ready}));
    ModuleCode* const code = &module_->code();
    praenora_instance* const instance = instance_;
    const CallResult result =
        run(claim, "finalize", options_.lifecycle_timeout, StopFlag::ignored,
            module_->call_access(), std::string(),
            [code, instance](Call& call) { return code->finalize(instance, call.error); });
    if (claim.called() && result.outcome != CallOutcome::unresponsive &&
        result.outcome != CallOutcome::crashed) {
        state_.store(result.outcome == CallOutcome::succeeded ? StepState::finalized
                                                              : StepState::failed);
    }
    return result;
}

CallOutcome ModuleInstance::destroy(std::string* details_json) noexcept {
    StepState current = state_.load();
    for (;;) {
        if (current == StepState::unresponsive) {
            return CallOutcome::unresponsive;
        }
        if (current == StepState::crashed) {
            return CallOutcome::crashed; // its module host is gone, and the instance with it
        }
        if (current == StepState::destroyed) {
            return CallOutcome::succeeded;
        }
        if (current == StepState::busy) {
            log(LogLevel::warn, "destroy ignored: another call into the instance is in progress",
                "call.refused");
            return CallOutcome::failed;
        }
        if (state_.compare_exchange_weak(current, StepState::busy)) {
            break;
        }
    }
    // The instance is ours now (busy), so instance_ can be read without racing another caller.
    Claim claim(state_, current);
    if (instance_ == nullptr) {
        return CallOutcome::succeeded; // nothing was created, so there is nothing to destroy
    }
    ModuleCode* const code = &module_->code();
    praenora_instance* const instance = instance_;
    try {
        const CallResult result = run(claim, "destroy", options_.lifecycle_timeout,
                                      StopFlag::ignored, module_->call_access(), std::string(),
                                      [code, instance](Call&) { return code->destroy(instance); });
        if (details_json != nullptr && result.outcome == CallOutcome::crashed) {
            *details_json = result.details_json;
        }
        if (claim.called() && result.outcome != CallOutcome::unresponsive &&
            result.outcome != CallOutcome::crashed) {
            instance_ = nullptr;
            state_.store(StepState::destroyed);
            // A module host serves this one instance: it is asked to exit now, and killed after
            // the lifecycle timeout (no limit without one: the longest wait, which every deadline
            // made of it saturates, never overflows).
            code->end(options_.lifecycle_timeout > std::chrono::nanoseconds::zero()
                          ? options_.lifecycle_timeout
                          : std::chrono::nanoseconds::max());
        }
        return result.outcome;
    } catch (...) {
        // destroy() could not even be posted; the instance stays, and a later destroy() may
        // succeed. The claim restores the state.
        log(LogLevel::error, "destroy could not be called", "call.failed");
        return CallOutcome::failed;
    }
}

std::optional<std::string> ModuleInstance::diagnostics() {
    StepState current = state_.load();
    for (;;) {
        if (current == StepState::loaded || current == StepState::busy ||
            current == StepState::unresponsive || current == StepState::crashed ||
            current == StepState::destroyed) {
            return std::nullopt;
        }
        if (state_.compare_exchange_weak(current, StepState::busy)) {
            break;
        }
    }
    // The instance is ours now (busy); the claim puts the state back when nothing is called.
    Claim claim(state_, current);
    ModuleCode* const code = &module_->code();
    if (instance_ == nullptr || !code->has_diagnostics()) {
        return std::nullopt;
    }
    praenora_instance* const instance = instance_;
    CallResult result = run(claim, "diagnostics", options_.lifecycle_timeout, StopFlag::ignored,
                            module_->call_access(), std::string(), [code, instance](Call& call) {
                                return code->diagnostics(instance, call.output);
                            });
    if (!claim.called() || result.outcome == CallOutcome::unresponsive ||
        result.outcome == CallOutcome::crashed) {
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
    if (module_->runs_in_child()) {
        throw Error(ErrorCode::UNSUPPORTED,
                    "module " + identity_ +
                        " runs in a module host; self_test() is called only in the runtime's "
                        "process (by the contract checks)");
    }
    ModuleCode* const code = &module_->code();
    if (!code->has_self_test()) {
        throw Error(ErrorCode::UNSUPPORTED, "module " + identity_ + " has no self_test()");
    }
    const praenora_host_api* host_api = &host_->api;
    const CallResult result =
        run(claim, "self_test", options_.invoke_timeout, StopFlag::ignored,
            LibraryGate::Access::exclusive, std::string(),
            [code, host_api](Call& call) { return code->self_test(host_api, call.output); });
    if (claim.called() && result.outcome != CallOutcome::unresponsive) {
        state_.store(StepState::loaded);
    }
    return result;
}

std::optional<StepState> ModuleInstance::release_terminated() noexcept {
    if (!module_->runs_in_child() || instance_ == nullptr || !module_->code().terminated()) {
        return std::nullopt;
    }
    StepState current = state_.load();
    for (;;) {
        if (current != StepState::created && current != StepState::ready &&
            current != StepState::finalized && current != StepState::failed) {
            return std::nullopt; // busy, or already over (destroyed, unresponsive, crashed)
        }
        if (state_.compare_exchange_weak(current, StepState::destroyed)) {
            break;
        }
    }
    instance_ = nullptr;
    return current;
}

void ModuleInstance::request_cancel() noexcept {
    // Marks the call in flight, if any; a call that starts later has another number.
    const std::uint64_t call = host_->in_flight.load();
    if (call != 0) {
        mark_stop(*host_, call);
    }
}

void ModuleInstance::withdraw_services() noexcept { stop_serving(); }

void ModuleInstance::start_recording(std::uint64_t number, const char* call,
                                     std::optional<std::uint64_t> tick) noexcept {
    if (!options_.record_lookups) {
        return;
    }
    const std::lock_guard<std::mutex> lock(host_->services_mutex);
    host_->recording = number;
    host_->recording_call = call;
    host_->recording_tick = tick;
    host_->lookups.clear();
    host_->recorded.clear();
}

void ModuleInstance::finish_recording(bool keep) noexcept {
    if (!options_.record_lookups) {
        return;
    }
    const std::lock_guard<std::mutex> lock(host_->services_mutex);
    if (host_->recording == 0) {
        return;
    }
    if (keep) {
        try {
            ended_lookups_.insert(ended_lookups_.end(),
                                  std::make_move_iterator(host_->lookups.begin()),
                                  std::make_move_iterator(host_->lookups.end()));
        } catch (...) {
            // Out of memory: the call's lookups go unrecorded.
        }
    }
    host_->recording = 0;
    host_->recording_call = nullptr;
    host_->recording_tick.reset();
    host_->lookups.clear();
    host_->recorded.clear();
}

std::vector<ServiceLookup> ModuleInstance::take_lookups() noexcept {
    std::vector<ServiceLookup> taken;
    taken.swap(ended_lookups_);
    return taken;
}

void ModuleInstance::stop_serving() noexcept {
    // The lock that get_service() takes: a lookup running now finishes with the registry first.
    const std::lock_guard<std::mutex> lock(host_->services_mutex);
    host_->services = nullptr;
    host_->serving = false;
}

StepState ModuleInstance::state() const noexcept { return state_.load(); }

const LoadedModule& ModuleInstance::module() const noexcept { return *module_; }

const std::optional<LibraryCall>& ModuleInstance::last_refusal() const noexcept {
    return refused_by_;
}

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
