# ADR-0015: Observability: spdlog, OpenTelemetry-aligned logs and metrics, status snapshots and status file

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Operators and scripts need to know what a run is doing, how it is performing and why it failed, from C++, the CLI and Python, possibly from another process. A host application embedding the runtime may use spdlog itself.

## Decision

- **Logs:** the runtime owns its own spdlog logger objects and never registers them globally or changes spdlog's default logger. Logger names include `praenora.runtime`, `praenora.engine`, `praenora.loader`, `praenora.config` and `module.<step>`. Defaults: level `info`, format `text`, output to stderr with an optional file. Level precedence is CLI, then `PRAENORA_LOG_LEVEL`, then the configuration file; a step's `log_level` filters that step's module logger.
- **Formats:** a text line, or JSON lines using OpenTelemetry log data model field names (`timestamp`, `severity_text`, `severity_number`, `body`, `attributes`).
- **Metrics:** an in-process registry keeps per-step invocation counts by outcome, duration statistics (`_ns`), last invocation and success times, last error and lifecycle state, plus per-workflow state, ticks, uptime and last error. `metrics()` exports points named in OpenTelemetry style (for example `praenora.step.invocations`). OTLP export is future work (OQ-9).
- **Status snapshot** (`kind: status-snapshot`) comes from a mutex-protected copy so queries never block execution for long.
- **Health:** `unknown` before start; `unhealthy` if the run failed or an instance is `failed` or `unresponsive`; `degraded` if any timeout, cancellation or failure was counted without failing the run, or the run was stopped, or a status-file write failed; otherwise `healthy`. Provisional (OQ-23).
- **Cross-process queries:** with `status_file` set, a background writer atomically replaces the file every `status_interval` (default 1 s) and at state changes; `praenora status <file>` renders it. A network endpoint is future work (OQ-9).
- **Debug aids:** module `diagnostics()`, message tracing, `praenora graph`, named worker threads, `PRAENORA_DEBUG_KEEP_LIBRARIES`.
- The ABI log callback is plain text only (OQ-24).

### Builder notes (Proposed)

- Severity texts are `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR` and `CRITICAL` (the configured level names in upper case); severity numbers are 1, 5, 9, 13, 17 and 21. Level names are accepted in lower case only.
- Text lines list the attributes in the order given, values unquoted when they consist of `A-Z a-z 0-9 . _ : / @ + -` and JSON-quoted otherwise; without attributes the brackets are omitted. JSON lines write `timestamp`, `severity_text`, `severity_number`, `body`, `attributes` in that order, `attributes` starting with `logger`; the attribute key `logger` is reserved. Invalid UTF-8 is replaced in JSON lines.
- Each `LogHub` fans out to its sinks through one spdlog distribution sink, so a sink added later (for example a test's capture) also receives events from loggers created earlier. Loggers flush after every event, so the log survives a crash inside a module. The log file is appended to, its parent directories are created, and it is written in binary mode.
- The metrics registry guards its state with one mutex; a snapshot is a copy taken under it. Steps are reported in the order they were registered; a call naming an unregistered step is `INVALID_ARGUMENT`. An unresponsive call counts as `timed_out` (the step status has no counter of its own for it), and a negative duration counts as 0. The last error of a step stays until a later error replaces it. `record_tick(t, …)` marks tick `t` complete: `ticks_completed` becomes `t + 1`, `current_tick` is `t` (the last completed tick) and `logical_time` its window end. Uptime runs on the steady clock from `mark_started()` until the state becomes `completed`, `stopped` or `failed`, and then stays fixed.
- Health is decided in this order: `unhealthy` (the run failed, or an instance is `failed` or `unresponsive`), `unknown` (states `created` and `starting`), `degraded` (a timeout, cancellation or failure was counted, the run is `stopping` or `stopped`, or the workflow has a last error), `healthy`. A status-file write error reaches health through the workflow's last error: the status-file writer reports it to its error handler, and the run records it there.
- The status snapshot document uses the field names of the `status-snapshot` schema. Instants are RFC 3339 strings, and an unset instant (0) is `null`, except `generated_at`, which is always written. A module's error document becomes `cause`; one that is not a JSON object is kept as `{"text": "…"}`. Bytes that are not UTF-8 (in module messages, for example) are replaced, and the document is compact JSON.
- Metrics export: units `{invocation}`, `{tick}` and `ns`; per step one `praenora.step.invocations` counter for each outcome (`succeeded`, `failed`, `timed_out`, `cancelled`), zeros included, with attributes `step`, `module` (`name@version`) and `outcome`, and one `praenora.step.invocation.duration` summary (`count`, `sum`, `min`, `max`, `last`). The workflow points carry the attribute `workflow` (`name@version`); `praenora.workflow.tick.duration` is a gauge of the last tick's duration, because the snapshot keeps no history of tick durations.
- The status-file writer writes once in its constructor, before it returns; then on a background thread named `praenora:status`, after `notify()` (notifications before a write make one write) and every `status_interval` after the previous write (never for an interval of 0); and once more in its destructor, after the thread has stopped. Long intervals are waited in steps of at most 24 hours. Failures never throw: they go to an optional error handler (a fourth constructor parameter, interface addition) as `IO_ERROR` for a write and `INTERNAL` for a producer that threw, and the writer keeps going.
- The engine registers the run and its steps with the metrics registry in its constructor, so a status-file writer built after it starts with them. A step's state follows its instance (`loaded`, `created`, `ready`, `finalized`, `destroyed`), with `busy` during each call the engine makes into it (create, initialize, invoke, finalize, destroy). A step whose call failed (an invocation that failed, timed out or returned an invalid result, or a failed finalize) shows `failed` from then on, and an abandoned one `unresponsive`. A failure that ends the run is the workflow's last error and the failing step's: an invocation records it with its count, and a failure in loading the module, create or initialize sets it on its own (`MetricsRegistry::set_step_error`, an interface addition). A finalize that fails at the end sets only the step's, so the failure that ended the run stays the workflow's. A stop sets none. `EngineSettings::on_state_change` (an interface addition) is called after every workflow state change, so a status-file writer can be notified at once.
- Atomic file replacement (status file, run record) writes `<file>.tmp-<pid>-<n>` and renames it over the file; the counter `n` keeps concurrent writers of one file in one process apart. On Windows the rename (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`) is retried up to 10 times, 10 ms apart, while another process holds the file (sharing or access errors). The data is not flushed with fsync: readers never see a partial file, but the content is not guaranteed to survive a crash of the machine.
- Logging settings resolve each on its own, so that one CLI flag does not reset the configuration file's other settings. For a run: level and format come from `WorkflowOptions::log_level` and `log_format` (CLI flags and API options), else `PRAENORA_LOG_LEVEL` and `PRAENORA_LOG_FORMAT` (only with `RuntimeOptions::use_environment`), else the configuration's `observability.log_level` and `log_format` when the file sets them, else `RuntimeOptions::logging`; the log file comes from `WorkflowOptions::log_file` (relative to the current directory; an empty one means no log file), else `observability.log_file`, else `RuntimeOptions::logging.file`; `to_stderr` always comes from `RuntimeOptions::logging`. The `Runtime`'s own logger (`praenora.runtime`) uses `RuntimeOptions::logging` with the environment's level and format put over it. A step's `log_level` still sets that step's module logger, whatever the run's level.
- The environment is read once, when the `Runtime` is constructed. An empty variable counts as unset; an invalid value makes the constructor throw `CONFIG_INVALID`, naming the variable. Each workflow logs through a log hub of its own, built with its resolved settings. Logging is never part of the run fingerprint.
- The runtime's loggers are `praenora.runtime` (the `Runtime`), `praenora.engine` (a run, the loading of its modules included), `module.<step>` (a step's module messages and the events of its instance) and `module.check` (module messages during the contract checks); there are no `praenora.loader` or `praenora.config` loggers, because configuration and loading problems reach the caller as errors and validation diagnostics. Events per step, call or tick (`step.ready`, `invocation.completed`, `tick.completed`, `service.lookup`) are at debug level, and `invocation.completed` and `tick.completed` carry their durations in the `duration_ns` attribute rather than in the message text; `run.started`, `run.completed` and `run.stopped` are at info level.
- A module's `diagnostics()` output appears only in the details of a failure, when the manifest declares `features.diagnostics`; the message trace carries each result's `diagnostics` object instead.
- Later: [ADR-0028](ADR-0028-error-records-keep-location-and-details.md) gives every error record the location and details of its error (`location` and `details` in the document).
- Later: [ADR-0042](ADR-0042-control-and-status-api-over-http.md) (Accepted) adds the network endpoint that this record left to OQ-9: the `praenora` tool serves a run's status snapshot, metrics, run record and latest outputs over HTTP, on a Unix socket or a TCP address (`praenora run --listen`, `praenora serve`, read with `praenora status --server`), the status in the status file's form (`Workflow::status_json(PathForm::placeholders)`), and every run's metrics as Prometheus text ([control-api.md](../specs/control-api.md)); the runtime library itself still opens no listener.
- Later: [ADR-0043](ADR-0043-otlp-export-of-metrics-and-logs.md) (Accepted) adds the OTLP export that this record left to OQ-9: the runtime exports its metrics and logs to an OpenTelemetry collector over OTLP/HTTP with JSON, configured by the `OTEL_*` variables or `RuntimeOptions::telemetry`, without the opentelemetry-cpp SDK, and a failed export never changes health ([observability.md](../specs/observability.md#otlp-export)). It also gives every attribute key of the logs and of the metrics export the prefix `praenora.` (`praenora.run.fingerprint`, `praenora.event`, `praenora.tick`, `praenora.step`, `praenora.module`, `praenora.duration_ns`, `praenora.outcome`, `praenora.workflow`, ...); `logger` keeps its name and is the OTLP scope's name. Tracing spans are OQ-33.

## Alternatives considered

- **Printf-style logging to stderr.** No structure and no per-module filtering.
- **Global spdlog default logger.** Interferes with host applications.
- **Adopt the OpenTelemetry C++ SDK now.** Heavy dependency; the MVP only aligns names (OQ-9).
- **Status only through a network endpoint.** Out of scope for the MVP.

## Consequences

- Logs can be consumed by tools expecting OpenTelemetry field names.
- Cross-process status needs no network code.
- Health thresholds are simple and may need tuning.

## References

- [docs/specs/observability.md](../specs/observability.md)
- [OpenTelemetry logs data model](https://opentelemetry.io/docs/specs/otel/logs/data-model/)
- [spdlog](https://github.com/gabime/spdlog)
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339)
