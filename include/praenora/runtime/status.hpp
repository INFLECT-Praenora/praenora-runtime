// Run state, step status and the status snapshot that the C++ API, the CLI and the Python package
// report. Durations are nanoseconds (fields ending in _ns); instants are nanoseconds since the Unix
// epoch (UTC; fields ending in _unix_ns).
#ifndef PRAENORA_RUNTIME_STATUS_HPP
#define PRAENORA_RUNTIME_STATUS_HPP

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/export.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace praenora {

enum class WorkflowState {
    created,
    starting,
    ready,
    running,
    stopping,
    completed,
    stopped,
    failed
};

// crashed: the step's module host ended or broke its channel during its load or a call (process
// isolation); the instance is never called again.
enum class StepState {
    loaded,
    created,
    ready,
    busy,
    finalized,
    destroyed,
    failed,
    unresponsive,
    crashed
};

enum class Health { unknown, healthy, degraded, unhealthy };

// How the error records of a status-snapshot document name files (Workflow::status_json()):
// absolute: with absolute paths, as the snapshot holds them in the process;
// placeholders: naming the run's directories by placeholders (<output>, <configuration>,
// <modules:caller:0>, <home>, ...), as the status file does (docs/specs/observability.md).
enum class PathForm { absolute, placeholders };

// The last error of a step or of the workflow. `cause_json` holds the module's own error document
// when a module call failed, otherwise it is empty. `location` and `details_json` are those of the
// error (Error::location() and Error::details_json(), "{}" when it has no details); a record that
// does not come from a praenora::Error leaves them empty.
struct ErrorRecord {
    ErrorCode code = ErrorCode::UNKNOWN;
    std::string message;
    std::optional<std::uint64_t> tick;
    std::int64_t at_unix_ns = 0;
    std::string cause_json;
    SourceLocation location;
    std::string details_json;
};

struct StepStatus {
    std::string step, module_name, module_version;
    StepState state = StepState::loaded;
    std::uint64_t invocations = 0, succeeded = 0, failed = 0, timed_out = 0, cancelled = 0;
    std::int64_t last_duration_ns = 0, total_duration_ns = 0, min_duration_ns = 0,
                 max_duration_ns = 0;
    double mean_duration_ns = 0;
    std::optional<std::int64_t> last_invocation_at_unix_ns, last_success_at_unix_ns;
    std::optional<ErrorRecord> last_error;
};

struct StatusSnapshot {
    std::int64_t generated_at_unix_ns = 0;
    std::string runtime_version;
    std::int64_t pid = 0;
    std::string workflow_name, workflow_version, fingerprint;
    WorkflowState state = WorkflowState::created;
    Health health = Health::unknown;
    std::optional<std::int64_t> started_at_unix_ns;
    std::int64_t uptime_ns = 0;
    std::uint64_t ticks_completed = 0;
    std::optional<std::uint64_t> current_tick;
    std::optional<double> logical_time;
    std::int64_t last_tick_duration_ns = 0;
    std::optional<ErrorRecord> last_error;
    std::vector<StepStatus> steps;

    // The snapshot as a `status-snapshot` document, its error records as they are, with absolute
    // paths. (The status file names the run's directories in them by placeholders.)
    PRAENORA_RUNTIME_API std::string to_json() const;
};

// The result of one tick: its number, its logical time (the end of its window), the workflow
// outputs it produced, and whether the run is finished.
struct TickResult {
    std::uint64_t tick = 0;
    double logical_time = 0;
    std::vector<std::string> outputs;
    bool finished = false;
};

struct RunSummary {
    WorkflowState final_state = WorkflowState::created;
    std::uint64_t ticks_completed = 0;
    std::string fingerprint;
    std::optional<ErrorRecord> error;
};

// The enumerator's name, for example "running".
PRAENORA_RUNTIME_API std::string to_string(WorkflowState state);
PRAENORA_RUNTIME_API std::string to_string(StepState state);
PRAENORA_RUNTIME_API std::string to_string(Health health);

} // namespace praenora

#endif // PRAENORA_RUNTIME_STATUS_HPP
