// The in-process metrics of a run: per-step invocation counts and durations, the workflow's
// state and progress, and the status snapshot, health and metrics export built from them.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_METRICS_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_METRICS_HPP

#include "instance/module_instance.hpp"

#include <praenora/runtime/status.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace praenora::detail {

// Thread-safe: every method takes one mutex for a short copy or update, so status queries from
// other threads never hold up the run for long.
class MetricsRegistry {
public:
    void set_run_info(std::string workflow_name, std::string workflow_version,
                      std::string fingerprint);

    // Adds a step (state `loaded`), or updates the module of one already added. Steps are
    // reported in the order they were first added.
    void register_step(const std::string& step, const std::string& module_name,
                       const std::string& module_version);

    // The calls below throw praenora::Error(INVALID_ARGUMENT) for a step never registered.
    void set_step_state(const std::string& step, StepState state);

    // One finished invocation: its outcome (unresponsive counts as timed_out; crashed and refused
    // count as failed), duration (negative counts as 0) and wall-clock time. last_success_at
    // changes only on success; `error`, when given, becomes the step's last error, which stays
    // until a later error.
    void record_invocation(const std::string& step, CallOutcome outcome,
                           std::chrono::nanoseconds duration, std::int64_t at_unix_ns,
                           std::optional<ErrorRecord> error);

    // A failure of the step outside an invocation (loading its module, create, initialize,
    // finalize): `error` becomes the step's last error; nothing is counted.
    void set_step_error(const std::string& step, ErrorRecord error);

    // Uptime runs from mark_started() until the state becomes completed, stopped or failed.
    void set_workflow_state(WorkflowState state);
    void mark_started(std::int64_t unix_ns);

    // Tick `tick` (numbered from 0) is complete: ticks_completed becomes tick + 1, current_tick
    // `tick`, and logical_time the tick's logical time.
    void record_tick(std::uint64_t tick, double logical_time, std::chrono::nanoseconds duration);

    // The workflow's last error (a run failure, or a problem that degrades the run, such as a
    // failed status-file write).
    void set_last_error(ErrorRecord error);

    // A consistent copy of everything, stamped with the current wall-clock time, the runtime's
    // version and the process id, with the health computed by compute_health().
    StatusSnapshot snapshot() const;

private:
    StepStatus& step_status(const std::string& step);

    mutable std::mutex mutex_;
    StatusSnapshot state_;
    std::map<std::string, std::size_t> steps_; // step id -> index in state_.steps
    std::optional<std::chrono::steady_clock::time_point> started_, ended_;
};

// The health rules, in order:
//   unhealthy: the run failed, or an instance is failed, unresponsive or crashed;
//   unknown:   the run has not started (created, starting);
//   degraded:  a timeout, cancellation or failure was counted, the run is stopping or stopped,
//              or the workflow has a last error (for example a failed status-file write);
//   healthy:   otherwise.
Health compute_health(const StatusSnapshot& snapshot);

// The snapshot's metrics as points named in OpenTelemetry style:
//   {"metrics": [{"name", "unit", "kind": "counter"|"gauge"|"summary", "attributes": {...},
//                 "value": N | "summary": {"count", "sum", "min", "max", "last"}}, ...]}
// Every attribute key carries the prefix "praenora."; per step (attributes praenora.step,
// praenora.module "<name>@<version>"):
//   praenora.step.invocations (counter, unit {invocation}; one point per outcome, attribute
//     praenora.outcome succeeded|failed|timed_out|cancelled, zeros included);
//   praenora.step.invocation.duration (summary, unit ns).
// Per workflow (attribute praenora.workflow "<name>@<version>"): praenora.workflow.ticks
// (counter, unit {tick}), praenora.workflow.tick.duration (gauge, unit ns: the last tick's
// duration) and praenora.workflow.uptime (gauge, unit ns).
std::string metrics_json(const StatusSnapshot& snapshot);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_METRICS_HPP
