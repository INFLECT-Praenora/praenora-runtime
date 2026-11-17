# Observability

- **Version:** runtime 0.2.0; document format `status-snapshot` 0.1.0
- **Status:** Normative, MVP. Proposed ([ADR-0015](../adr/ADR-0015-observability.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The runtime reports what a run is doing through structured logs, in-process metrics, status
snapshots with a health value, an optional status file that other processes can read, and an
optional message trace. Logs and metrics follow the names of the OpenTelemetry log data model and
metric conventions; no OpenTelemetry exporter is built in ([open-questions.md](../open-questions.md),
OQ-9). Everything here is available from C++, the CLI and Python.

## Logs

The runtime logs through spdlog loggers of its own. It never registers them in spdlog's global
registry and never changes spdlog's default logger, so an application that uses spdlog itself is
unaffected.

| Logger | What it records | Events |
|---|---|---|
| `praenora.runtime` | The `Runtime`. | `runtime.ready` (module and provider search paths, number of modules, providers and catalog entries) |
| `praenora.engine` | A run. | `run.started`, `step.ready`, `invocation.completed`, `tick.completed`, `run.completed`, `run.stopped`, `run.failed`, `run.output_failed`, `trace.skipped`, `teardown.finalize_failed`, `teardown.skipped` |
| `module.<step>` | The messages a step's module logs through the host API, and events about the step's instance. | `call.deadline`, `instance.unresponsive`, `call.refused`, `call.failed`, `service.lookup` |
| `module.check` | Module messages during the contract checks. | |
| `provider.<name>` | The messages a service provider logs through its host, with the attribute `provider` (`name@version`), and events about it, in the logs of the run (or, for `validate --load` and the contract checks, of the `Runtime`) that loaded it last ([runtime-services.md](runtime-services.md#loading)). | `provider.loaded` (once per process, with `library_sha256`) |

Records go to standard error, without colors, unless `RuntimeOptions::logging.to_stderr` is false;
and, when a log file is set, also to that file, which is appended to, created with its parent
directories and written in binary mode. Every record is flushed at once, so the log survives a crash
inside a module.

### Formats

`text` (the default) writes one line per record:

```text
<timestamp> <LEVEL, padded to 5> <logger> [<key>=<value> ...] <message>
2026-11-11T16:59:35.006413000Z DEBUG praenora.engine [run.fingerprint=4664e72704c1c728dea594dfc214e4494d51a852d79e2bb36d6ba61a098ec918 event=invocation.completed tick=0 step=estimate module=example-estimator@0.1.0 duration_ns=232375] invocation completed
```

Attribute values consisting only of `A-Z a-z 0-9 . _ : / @ + -` are written as they are, and other
values as JSON; without attributes the brackets are left out.

`json` writes one JSON object per line, with the field names of the OpenTelemetry log data model:
`{"timestamp", "severity_text", "severity_number", "body", "attributes"}`, where `attributes` starts
with `logger`. Bytes that are not UTF-8 are replaced.

| Level | `severity_text` | `severity_number` |
|---|---|---:|
| `trace` | `TRACE` | 1 |
| `debug` | `DEBUG` | 5 |
| `info` | `INFO` | 9 |
| `warn` | `WARN` | 13 |
| `error` | `ERROR` | 17 |
| `critical` | `CRITICAL` | 21 |

The level `off` records nothing. Timestamps are RFC 3339 UTC with nine fractional digits. Common
attributes are `run.fingerprint` (every record of a run), `event`, `tick`, `step`, `module`
(`name@version`) and `duration_ns`; the attribute name `logger` is reserved.

### Settings

Each logging setting is resolved on its own, from the first source that sets it:

| Setting | Sources, in order |
|---|---|
| Level | The run's option (`--log-level`, `WorkflowOptions::log_level`); `PRAENORA_LOG_LEVEL`; the configuration's `observability.log_level`; `RuntimeOptions::logging.level` (default `info`). |
| Format | The run's option (`--log-format`, `WorkflowOptions::log_format`); `PRAENORA_LOG_FORMAT`; `observability.log_format`; `RuntimeOptions::logging.format` (default `text`). |
| File | The run's option (`--log-file`, `WorkflowOptions::log_file`, where an empty value means none); `observability.log_file`; `RuntimeOptions::logging.file` (default none). |

- The environment variables are read once, when the `Runtime` is constructed, and only when
  `RuntimeOptions::use_environment` is set. An empty variable counts as unset; an invalid value is
  `CONFIG_INVALID`. In documents and the environment, level and format names are lower case.
- A step's `log_level` in the configuration sets the level of that step's module logger, whatever
  the run's level: it can make one module quieter or more verbose than the rest.
- A run's log file MUST NOT lie at or under a file or directory that the run's output directory
  manages (`CONFIG_INVALID`), because an overwrite would remove it
  ([reproducibility.md](reproducibility.md#output-files)).
- Logging never affects the run fingerprint.

## Metrics

Each run keeps an in-process metrics registry:

- per step: the module, the state, invocation counts by outcome (`succeeded`, `failed`, `timed_out`,
  `cancelled`; an unresponsive call counts as `timed_out`), the last, total, minimum, maximum and
  mean invocation durations, the times of the last invocation and the last success, and the last
  error;
- per workflow: the state, the start time, the uptime (on the steady clock, frozen when the run
  ends), the ticks completed, the current tick and its logical time, the last tick's duration and
  the last error.

`Workflow::metrics_json()` (Python `Workflow.metrics()`) exports them as points named in
OpenTelemetry style, `{"metrics": [{"name", "unit", "kind", "attributes", "value" | "summary"}, ...]}`:

| Name | Kind | Unit | Attributes | Value |
|---|---|---|---|---|
| `praenora.step.invocations` | counter | `{invocation}` | `step`, `module`, `outcome` | The step's invocations with that outcome; one point per outcome, zeros included. |
| `praenora.step.invocation.duration` | summary | `ns` | `step`, `module` | `{count, sum, min, max, last}`. |
| `praenora.workflow.ticks` | counter | `{tick}` | `workflow` (`name@version`) | The ticks completed. |
| `praenora.workflow.tick.duration` | gauge | `ns` | `workflow` | The last tick's duration. |
| `praenora.workflow.uptime` | gauge | `ns` | `workflow` | The uptime. |

## Status snapshot

A status snapshot (`kind: status-snapshot`,
[`status-snapshot.0.1.schema.json`](../../spec/schemas/status-snapshot.0.1.schema.json)) is a
consistent copy of the metrics at one moment. `Workflow::status()` and `status_json()` (Python
`Workflow.status()`) return one, and the status file holds one. A snapshot is taken under a mutex
from a copy of the metrics, so it never waits for a module call.

| Field | Content |
|---|---|
| `kind`, `format_version` | `status-snapshot`, `0.1.0`. |
| `generated_at` | When the snapshot was taken. |
| `runtime_version`, `pid` | The runtime's version and the process id. |
| `workflow` | `name`, `version`, `fingerprint`, `state`, `health`, `started_at`, `uptime_ns`, `ticks_completed`, `current_tick` (the last completed tick), `logical_time`, `last_tick_duration_ns`, `last_error`. |
| `steps[]` | Per step, in schedule order: `step`, `module_name`, `module_version`, `state`, `invocations`, `succeeded`, `failed`, `timed_out`, `cancelled`, `last_duration_ns`, `total_duration_ns`, `min_duration_ns`, `max_duration_ns`, `mean_duration_ns`, `last_invocation_at`, `last_success_at`, `last_error`. |

- Workflow states: `created`, `starting`, `ready`, `running`, `stopping`, `completed`, `stopped`,
  `failed`. `stopping` is shown only while a stop or a failure winds the run down.
- Step states are the instance states of the [module ABI](module-abi.md#lifecycle), `busy` during
  each call; a step whose call failed is shown as `failed` from then on.
- An error record is `{code, name, message, tick, at, cause}`, where `cause` is the module's error
  document. A step's last error stays until a later error replaces it; a stop sets no error.
- Instants are RFC 3339 strings, and an unset instant is `null` (except `generated_at`, which is
  always set). The document is compact JSON.

## Health

The workflow's `health` is decided by the first rule that applies:

| Health | Rule |
|---|---|
| `unhealthy` | The run failed, or an instance is `failed` or `unresponsive`. |
| `unknown` | The run has not started (states `created` and `starting`). |
| `degraded` | A timeout, a cancellation or a failure was counted; or the run is `stopping` or `stopped`; or the workflow has a last error, for example a status file that could not be written. |
| `healthy` | Otherwise. |

These rules are provisional (OQ-23).

## Status file

When the configuration's `observability.status_file` or the run's `--status-file` names a file, a
writer keeps a status snapshot there for other processes:

- It writes the file when the run is opened, then, on a background thread named `praenora:status`,
  after every change of the workflow state and every `status_interval` after the previous write
  (only on changes when the interval is 0), and once more when the run is closed.
- Each write replaces the file atomically: it writes `<file>.tmp-<pid>-<n>` and renames it over the
  file, so a reader never sees a partial document. The data is not flushed to the disk.
- A failed write never stops the run: it becomes the workflow's last error (`IO_ERROR`), which makes
  the health `degraded`, and the writer keeps going.
- `praenora status <file> [--json]` and `Runtime::read_status_file()` read it; a file of another kind
  is `DOCUMENT_KIND_MISMATCH`. A network endpoint is future work (OQ-9).

## Message trace

With `outputs.trace_messages: true` (or `praenora run --trace`) and an output directory, a run writes
every message that goes into or comes out of a step to `trace/messages.jsonl`:

- Each line is the canonical JSON of `{"tick", "step", "direction", "port", "envelope"}`, plus
  `"diagnostics"` when the step's result had a diagnostics object, followed by `\n`.
- An `in` record is written, before the call, for each envelope a step receives; workflow inputs
  appear there with source `inputs.<name>`. An `out` record is written for each envelope a step
  produces. A result with diagnostics but no outputs gets an `out` record with `port` and `envelope`
  null.
- The trace is byte-deterministic like the recorded outputs. A tick cut short by a stop keeps the
  lines already written. The run record's `execution.trace` gives the file, its SHA-256 and the
  number of envelopes in it.
- Without an output directory no trace is written, and a warning (event `trace.skipped`) says so.
- Runtime-service lookups are not traced; they are logged (event `service.lookup`, debug, on the
  step's logger), whether the service is a provider's or built in. Which provider serves each
  service of a run is recorded in the run record's reproducibility section (`services`), not in the
  logs ([reproducibility.md](reproducibility.md#run-fingerprint)).

## Debug aids

- A module's `diagnostics()` output, added to a failure's details when the manifest declares
  `features.diagnostics`.
- The message trace.
- `praenora graph` (text, JSON, DOT and AsyncAPI) for a workflow's schedule and connections.
- Named threads: `praenora:<step>` for each instance, `praenora:status` for the status file.
- `PRAENORA_DEBUG_KEEP_LIBRARIES=1`, which keeps every module library loaded for debuggers and leak
  checkers.

## References

- [ADR-0015](../adr/ADR-0015-observability.md): observability.
- [ADR-0020](../adr/ADR-0020-message-envelope-and-asyncapi.md): the message trace's records.
- [OpenTelemetry logs data model](https://opentelemetry.io/docs/specs/otel/logs/data-model/).
- [error-taxonomy.md](error-taxonomy.md): the codes in error records.
- [user-guide.md](../user-guide.md#status-and-health): status files and health.
- [developer-guide.md](../developer-guide.md#logging): logging and tracing.
