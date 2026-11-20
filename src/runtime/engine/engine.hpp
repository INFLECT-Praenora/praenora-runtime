// The execution engine: runs one validated workflow, tick by tick.
//
// start(). With an output directory, the directory must hold no earlier run (prepare_output_dir()
// without overwrite), run-record.json is written (status running), and the output files and, when
// tracing, trace/messages.jsonl are opened. Without an output directory nothing is written to
// disk: the run record is kept in memory, and a message trace that was asked for is skipped with a
// warning. Then, unless a stop was requested, the providers of the services of the steps in this
// process (isolation mode "in-process") are loaded (load_provider(), each provider once, with
// every such service the run gets from it, in the order of the reproducibility section's
// `services` keys): LIBRARY_DIGEST_MISMATCH when a provider's library changed after validation,
// and every other error of the provider loader, located in the provider's manifest, fails the run
// before any module is loaded. A provider that only steps in module hosts use is never loaded in
// this process. Then for each step, in schedule order: the step's module is loaded (once per
// manifest, shared by the steps that use it; LIBRARY_DIGEST_MISMATCH when the library changed
// after validation), or, for a step whose isolation mode is "process", loaded in a module host of
// the step's own, never shared (LoadedModule::load_in_child(): the providers of the step's
// services loaded and started there first, then the module, with the same checks and the
// providers' errors located in their manifests; the module host's events on the step's logger,
// the providers' records on the run's provider.<name>; MODULE_HOST_UNAVAILABLE when no module host
// can be started, MODULE_CRASHED when it ends while it loads, and the step is then `crashed`); an
// instance is created with the create document {pmp_version, step, capability {id, version},
// module {name, version}, parameters} and initialized with the initialize document {pmp_version,
// run {fingerprint, seed (the step's)}, time {unit, start, period}}. The instance is served the
// provider services of its step (ResolvedStep::services), by the providers in its own process, and
// no other provider service; in this process also the built-in services (EngineSettings::services),
// which a module host does not offer.
//
// run_tick(). Tick t covers the logical-time window [start + t*period, start + (t+1)*period),
// computed by multiplication; its logical time is the window's end.
//   1. Each workflow input's source gives the data of the window, wrapped in an envelope with
//      source inputs.<name>.
//   2. Each step runs once, in schedule order, with the invocation document {pmp_version, step,
//      tick, logical_time, inputs {port: envelope}}: a delay-0 input receives the message produced
//      in tick t, a delay-1 input the one of tick t-1 (none at tick 0); inputs without a message
//      are left out.
//   3. The step's result must be JSON (else OUTPUT_INVALID with details.raw_prefix, its first 200
//      bytes), satisfy pmp-result, name only output ports of the step's capability and every
//      required one (OUTPUT_INVALID at /outputs/<port> or /outputs, with details.port) and, when
//      payloads are validated, each output's data must satisfy its port's message type
//      (OUTPUT_INVALID at /outputs/<port>/data<pointer>). Each output is then wrapped in an
//      envelope with source <step>.<port>.
//   4. The messages that reach workflow outputs are the tick's outputs. They become the latest
//      outputs, and the recorded ones (outputs.record) are appended to outputs/<output>.jsonl.
//   5. With tracing, every envelope that went into or came out of a step is in the trace, the
//      result's diagnostics object on the lines of its outputs (on a line of its own when the
//      result has no outputs), and between them the runtime-service lookups the invocation made.
//      The metrics count the tick.
// With tracing, the lookups of every other call into a step's instance (create, initialize,
// diagnostics, finalize, destroy) are in the trace too, without a tick, right after the call. The
// lookups of a call that did not succeed (an invoke whose result is invalid included) and of a
// failure's diagnostics call are written when the teardown starts, which is the same place:
// nothing is traced in between. So a trace that cannot be written never hides how a call ended.
// Validating payloads (EngineSettings::validate_payloads; "strict" in the run record) also checks
// each input's data against its message type (TARGET_DATA_INVALID) and every envelope against
// pmp-envelope. Only a defect of the runtime can break an envelope the engine built, so that is
// INTERNAL, with the step, module and port (or the input), the tick and the schema issues in its
// details; it is not the module's failure, whose invocation counts as succeeded. Without
// validation ("off") no data and no envelope is checked against its schema.
//
// The end. When `ticks` ticks are complete, or no tick count is set and every replay input is
// exhausted after the last window, the engine tears everything down at once (after that tick, or
// in start() for a run of no ticks), enters `completed` and writes the final run record.
// finished() is true from then on, and once a run is stopped or failed; a stop that was only
// requested does not finish the run by itself: the next run_tick() (or shutdown()) winds it down.
//
// Failures. The first failure ends the run. A module call that does not return in time but within
// the cancellation grace is STEP_TIMEOUT, one that does not return at all MODULE_UNRESPONSIVE (in a
// module host, which is then killed, with details.terminated), one that fails STEP_FAILED (its
// message and cause are the module's error document), one whose module host ends or breaks its
// channel MODULE_CRASHED (with how it ended); a library that does not load keeps the loader's code.
// The error's details hold step, module, phase (load, create, initialize or invoke), tick (during a
// tick), cause (the module's error document) and, when the failing instance declares diagnostics
// and is neither unresponsive nor crashed, its diagnostics (asked for before the teardown; over 64
// KiB they become {"truncated": true, "bytes", "text": the first 64 KiB}); a provider in this
// process that does not load names no step, and its error keeps the loader's details (one in a
// module host fails its step's load, phase load). The engine enters `stopping`, tears
// everything down, enters `failed`, writes the final run record and throws the error. Other errors
// (writing files, input data, unexpected exceptions, as INTERNAL) end the run the same way. Even an
// exception that breaks off the ending itself (out of memory, most likely) leaves a failed run with
// an error, the first failure: the one it was ending with, else an error met earlier in the ending
// (an output file), else an INTERNAL one; the run records the very error that the control call that
// ended the run throws (shutdown() and the destructor only record it), so a failed run always
// carries its error in the summary, the run record and the status. Every error record the engine
// makes of an error keeps its location and details (for a run failure, the details the error is
// thrown with).
//
// Stop. request_stop() raises the stop flag, which every instance sees as its calls start, and asks
// the call in flight to cancel; a module host still loading its step's module is killed instead (no
// load has a timeout of its own). The engine starts no module call while the flag is up: the next
// step of start() or of a tick, or the next tick, ends the run as `stopped` instead, as does a call
// that returns cancelled, or a load that the stop ended; a run stopped before start() loads no
// provider either. A tick cut short does not count and records no outputs (the trace keeps what it
// has written). A stopped run is not an error: start() and run_tick() return normally (run_tick()
// with finished set, no outputs and the tick that did not complete), and the summary's error is
// RUN_STOPPED.
//
// Teardown. Every ready instance is finalized, the last created first; then every created instance
// is destroyed, again the last created first; then the libraries are released (unloaded unless
// pinned). finalize and destroy run normally after a stop. Unresponsive instances are left alone:
// never finalized or destroyed, their libraries pinned (in a module host, which was killed, nothing
// is pinned); so are crashed ones. Every call goes through its library's gate
// (loader/library_gate.hpp), which refuses a call that would have to wait for an abandoned call, of
// this run or another one in the process: with features.concurrency "module", any call into a
// library while an abandoned call may still be running in it. An instance whose finalize or destroy
// is refused is left alone too: from then on it is neither finalized nor destroyed, never freed,
// and served no more runtime services (a warning, event teardown.skipped, names it, what was left
// undone and the abandoned call). Instances already destroyed or never created (still loaded) make
// no module call when released. This also applies when an instance becomes unresponsive during the
// teardown itself; and once the abandoned call returns, the calls that follow are made as usual.
//
// Calls into a library shared with other runs. A call waits for its turn at the library's gate
// before its deadline starts (with concurrency "module", while another run's call into the
// library runs). A stop ends the wait of a create, initialize or invoke, which is then not an
// invocation; the teardown's finalize and destroy, and the diagnostics of a failure, still wait.
// A create, initialize or invoke that is refused fails the run with MODULE_UNRESPONSIVE, naming
// the abandoned call (details: abandoned_call), and its step as failed; the module is not
// called, and a refused invoke is the step's last error, not an invocation. A load of a library
// refused that way (LoadedModule::load()) fails the run the same way, at its step's load.
//
// Metrics. The constructor registers the run and its steps with EngineSettings::metrics (so a
// status-file writer built afterwards has them). The engine keeps the workflow state there
// (created, starting, ready, running, completed; stopping only while a stop or failure tears down,
// then stopped or failed), each step's state (busy during each call the engine makes into it;
// failed, sticky, for a step whose call failed or whose finalize failed; unresponsive; crashed),
// every invocation, every tick, and the last errors: a failure is the workflow's last error and the
// failing step's (counted with its invocation, or set on its own for load, create, initialize,
// finalize and an invoke that was refused); a finalize at the end that fails, times out, never
// returns or whose module host crashed (not one that is refused), and a destroy that never returns
// or whose module host crashed, are only the step's, with the call in details.phase (warnings
// teardown.finalize_failed and teardown.destroy_failed); a destroy that returns after its
// deadline, within the grace, sets none. An invoke that a stop ended while it waited for its
// library is not counted. A stop sets no last error.
//
// Threading. start(), run_tick() and shutdown() are control calls, made by one thread at a time;
// the destructor too. All other methods are thread-safe and may be called at any time from any
// thread; request_stop() is also lock-free and async-signal-safe.
#ifndef PRAENORA_RUNTIME_ENGINE_ENGINE_HPP
#define PRAENORA_RUNTIME_ENGINE_ENGINE_HPP

#include "api/validation.hpp"
#include "instance/module_instance.hpp"
#include "instance/services.hpp"
#include "io/output_recorder.hpp"
#include "io/run_record.hpp"
#include "io/trace_writer.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"
#include "observability/metrics.hpp"
#include "observability/path_anchors.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

struct EngineSettings {
    // The loggers praenora.engine and module.<step> (a step's log_level filters its logger), and
    // provider.<name> of the providers the run loads. With none, nothing is logged. Must outlive
    // the engine.
    LogHub* logs = nullptr;
    // Required. Must outlive the engine.
    MetricsRegistry* metrics = nullptr;
    // The built-in services, served to every module in this process through the host API's
    // get_service() after its step's provider services (not in a module host). Must outlive the
    // engine.
    const ServiceRegistry* services = nullptr;
    // "strict" validation: input data and module outputs against their message types, envelopes
    // against pmp-envelope. false ("off") is for performance measurements; the run record says so.
    bool validate_payloads = true;
    // The directories of the run that the error records of its run record name by placeholders
    // (RunRecordWriter). With none, they keep their absolute paths.
    std::shared_ptr<const PathAnchors> path_anchors;
    // Called on the control thread after each change of the workflow state, for example to have a
    // status file rewritten. Exceptions it throws are ignored.
    std::function<void()> on_state_change;
};

class Engine {
public:
    // Registers the run (workflow name and version, fingerprint) and its steps (in schedule order,
    // state loaded) with settings.metrics and enters `created`; nothing is loaded yet. The context
    // that `workflow` was validated against must outlive the engine. Throws
    // praenora::Error(INVALID_ARGUMENT) without metrics, or for a workflow that is not one
    // validate_workflow() returned.
    Engine(ValidatedWorkflow workflow, EngineSettings settings);
    // shutdown(). Never throws.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the run (state created; else INVALID_STATE): ends in `ready`, or `completed` for a run
    // of no ticks, or `stopped`. Throws the failure, after cleaning up, as described above.
    void start();

    // Runs the next tick (state ready or running; else INVALID_STATE) and returns its number, its
    // logical time, the workflow outputs it produced (sorted) and whether the run is now finished.
    // Throws the failure, after cleaning up, as described above.
    TickResult run_tick();

    // True once the run is over (completed, stopped or failed), and when its ticks are complete
    // or its inputs exhausted (the run then completes at once). A stop request alone does not make
    // it true: the next run_tick() winds the run down without calling any module (or shutdown()
    // does), so a loop of run_tick() until finished() ends as stopped. Thread-safe.
    bool finished() const;

    // Raises the stop flag and asks the module call in flight, if any, to cancel. Lock-free and
    // async-signal-safe.
    void request_stop() noexcept;

    // Ends a run that is ready or running as `stopped` (summary error RUN_STOPPED): tears down and
    // writes the final run record. Does nothing before start() or once the run is over. Never
    // throws; problems are logged.
    void shutdown() noexcept;

    // The newest envelope of each workflow output produced so far, as a JSON object keyed by
    // output name ({} before the first tick).
    std::string latest_outputs_json() const;

    // The run record as last written; null before start().
    nlohmann::json run_record() const;

    WorkflowState state() const noexcept;
    // The state, ticks completed, fingerprint and the error that ended the run (failed: the
    // failure; stopped: RUN_STOPPED; otherwise none).
    RunSummary summary() const;
    const std::string& fingerprint() const noexcept;

private:
    struct Step;
    struct Failure;

    // Runs `body` (the work of a control call): an exception that escapes it fails the run if it
    // is not over yet, and is rethrown as a praenora::Error.
    template <class Body> auto control(Body&& body) -> decltype(body());
    void start_run();
    // Loads the providers of the services of the steps in this process and keeps their tables.
    void load_providers();
    TickResult tick_once();
    void open_output(std::int64_t started_at);
    // Loads, creates and initializes `step`; false when a stop ended the run instead.
    bool start_step(Step& step);
    // Calls create or initialize (`phase`, through `method`) with `document`; false when a stop
    // ended the run instead.
    bool lifecycle_call(Step& step, const char* phase,
                        CallResult (ModuleInstance::*method)(const std::string&),
                        const nlohmann::json& document);
    // Runs `step` in `tick`, adding its outputs to `messages`; false when a stop ended the run.
    bool run_step(Step& step, std::uint64_t tick, double logical_time,
                  std::map<std::string, nlohmann::json>& messages);
    // The envelopes of a successful invocation's outputs, by port (sorted); throws the failure
    // when the result is not acceptable (OUTPUT_INVALID) or an envelope built around it is broken
    // (INTERNAL). `diagnostics` receives the result's.
    std::vector<std::pair<std::string, nlohmann::json>>
    check_result(Step& step, std::uint64_t tick, double logical_time, const CallResult& result,
                 std::optional<nlohmann::json>& diagnostics);
    // Makes a call into the step's instance (`body`) reachable by request_stop() while it runs.
    template <class Call> CallResult call(Step& step, Call&& body);
    // Takes the runtime-service lookups of the calls into the step's instance that ended since
    // the last time (ModuleInstance::take_lookups()) and, with tracing, writes them to the trace.
    // Throws like TraceWriter::write_lookup().
    void trace_lookups(Step& step);

    // Ends the run as failed because of `error`, in the context `failure`; returns the error to
    // throw.
    Error fail(const Error& error, const Failure& failure);
    // The error of a call into the step's instance that did not succeed (`phase`: create,
    // initialize, invoke or finalize; `tick` during a tick): STEP_TIMEOUT, MODULE_UNRESPONSIVE
    // (also for a refused call, with details {"abandoned_call": ...}) or STEP_FAILED.
    Error call_error(const Step& step, const char* phase, const CallResult& result,
                     std::optional<std::uint64_t> tick) const;
    // TARGET_DATA_INVALID for workflow input `input` in `tick`: `problem`, then the first of
    // `issues` (not empty), located at the input's file and `pointer` (into the data, if any).
    Error input_error(const std::string& input, std::uint64_t tick, const std::string& problem,
                      const std::vector<SchemaIssue>& issues, std::string pointer) const;
    // Ends the run as stopped (RUN_STOPPED), during `tick` if any.
    void stop_run(std::optional<std::uint64_t> tick);
    // Ends the run as completed.
    void complete_run();
    // Winds the run down: `stopping` first for a stop or failure, then the teardown, the output
    // files closed, the final state and the final run record. Returns the I/O error that turned a
    // completed or stopped run into a failed one, if any; otherwise problems are only logged.
    std::optional<Error> finish(WorkflowState final_state, std::optional<ErrorRecord> error);
    void teardown() noexcept;

    void set_state(WorkflowState next);
    void sync_step_state(const Step& step) noexcept;
    void show_step_state(const Step& step, StepState shown) noexcept;
    void update_end_reached();
    double bound(std::uint64_t tick) const;
    const MessageTypeDef& message_type(const std::string& id, const SemVer& version) const;
    void log(LogLevel level, const std::string& message,
             std::vector<LogAttribute> attributes = {}) const noexcept;

    ValidatedWorkflow workflow_;
    EngineSettings settings_;
    MetricsRegistry& metrics_;
    std::shared_ptr<spdlog::logger> logger_;
    std::string fingerprint16_;
    std::vector<std::string> recorded_; // the workflow outputs to record, sorted

    // Read by request_stop() and by the instances; declared before the steps so that it outlives
    // them.
    std::atomic<bool> stop_flag_{false};
    std::atomic<ModuleInstance*> in_flight_{nullptr}; // the instance of the call in flight
    std::atomic<int> cancellers_{0};                  // request_stop() calls using in_flight_
    std::atomic<WorkflowState> state_{WorkflowState::created};
    std::atomic<std::uint64_t> ticks_completed_{0};
    std::atomic<bool> end_reached_{false}; // the ticks are complete or the inputs exhausted

    std::vector<Step> steps_; // in schedule order
    std::map<const ModuleManifest*, std::shared_ptr<LoadedModule>> modules_;
    std::map<std::string, const void*> provider_tables_; // by service key, once loaded
    std::unique_ptr<OutputRecorder> outputs_;
    std::unique_ptr<TraceWriter> trace_;
    std::map<std::string, nlohmann::json> previous_; // last tick's messages, by source endpoint
    std::optional<std::uint64_t> current_tick_;      // the tick running, if any

    mutable std::mutex mutex_;                         // guards the members below
    nlohmann::json latest_ = nlohmann::json::object(); // newest envelope per workflow output
    std::optional<RunRecordWriter> record_;
    std::optional<ErrorRecord> error_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ENGINE_ENGINE_HPP
