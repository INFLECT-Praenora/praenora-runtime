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
| `praenora.engine` | A run. | `run.started`, `step.ready`, `invocation.completed`, `tick.completed`, `run.completed`, `run.stopped`, `run.failed`, `run.output_failed`, `trace.skipped`, `teardown.finalize_failed`, `teardown.destroy_failed` (a `destroy` that did not return within its deadline plus the cancellation grace, or whose module host crashed), `teardown.skipped` (an instance left as it is: kept, or gone with the module host that a stop ended before it took a call) |
| `module.<step>` | The messages a step's module logs through the host API, and events about the step's instance. | `call.deadline`, `instance.unresponsive`, `call.refused`, `call.failed`, `library.refused` (a call not made: an abandoned call may still be running in the library), `instance.kept` (an instance kept because its `finalize` or `destroy` was refused), `service.lookup`; for an instance in a module host ([process-isolation.md](process-isolation.md)), with the attributes `module` and `pid`: `module_host.started` (debug), `module_host.exited` (debug), `module_host.terminated` (warn: killed after a call that never returned, after it did not exit when asked, when a stop came during its start, its load or the write of a call, or when it broke the channel's protocol while it was being ended), `module_host.crashed` (error: how it ended, or how it broke the channel's protocol), `module_host.directory_left` (warn: its temporary directory, under the restricted sandbox profile, could not be removed, and is left); for a module host that shares compiled models ([process-isolation.md](process-isolation.md#shared-compiled-models)), with the attributes `module`, `pid`, `provider` and `key`: `compiled_models.hit` (debug, with `size`), `compiled_models.miss` (debug), `compiled_models.saved` (debug, with `size` and `evicted`), `compiled_models.refused` (debug: over a limit, with `key_size` and `size`) |
| `module.check` | Module messages during the contract checks, and events about the checks' instances. | the events of `module.<step>` |
| `provider.<name>` | The messages a service provider logs through its host, with the attribute `provider` (`name@version`), and events about it, in the logs of the run (or, for `validate --load` and the contract checks, of the `Runtime`) that loaded it last ([runtime-services.md](runtime-services.md#loading)); from a provider in a step's module host too, with the same attributes ([process-isolation.md](process-isolation.md#service-providers)). | `provider.loaded` (once per process, a module host's included, with `library_sha256`) |

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
  `cancelled`; an unresponsive call counts as `timed_out`, a crashed one as `failed`), the last,
  total, minimum, maximum and
  mean invocation durations, the times of the last invocation and the last success, and the last
  error. An invoke that never reached the module is not an invocation: one refused because an
  abandoned call may still be running in its library is only the step's last error, and one stopped
  while it waited for its turn at the library is not counted at all
  ([threading-and-ownership.md](threading-and-ownership.md#concurrency-across-instances));
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
  each call; a step whose call failed is shown as `failed` from then on. A step whose module host
  crashed ([process-isolation.md](process-isolation.md#crashes)) is `crashed`, for good.
- An error record is `{code, name, message, tick, at, cause, location, details}`. `cause` is the
  module's error document (one that is not a JSON object is kept as `{"text": ...}`), or null.
  `location` and `details` are those of the error, as `praenora::Error` and the CLI's error
  document hold them: `location` has the parts that are set (`file`, `line`, `column`, `pointer`;
  a column only with a line), or is null when none is, and `details` is the error's details object
  (for a run failure the `step`, `module`, `phase`, `tick`, `cause` and `diagnostics` of
  [error-taxonomy.md](error-taxonomy.md#how-errors-are-reported), and for a step's last error from
  the teardown its `phase`, `finalize` or `destroy`, beside those of its code), or null when it has
  none (text that is not a JSON object is kept as `{"text": ...}`). Every key is always written. A
  record that does not come from an error, such as `RUN_STOPPED`, has a null `location` and null
  `details`. A step's last error stays until a later error replaces it; a stop sets no error.
- The error records of the snapshots that stay in the process (`Workflow::status()`,
  `status_json()`, `StatusSnapshot::to_json()`, Python `Workflow.status()`) hold absolute paths.
  Those of the status file, and of the run record, name the directories the run knows by
  placeholders ([below](#known-directories-in-the-files-of-a-run)).
- Instants are RFC 3339 strings, and an unset instant is `null` (except `generated_at`, which is
  always set). The document is compact JSON.

### Known directories in the files of a run

The status file and the run record ([reproducibility.md](reproducibility.md#run-record)) are
written for others to read. In their error records, every directory that the run knows is named by
a placeholder, so they name neither the machine's directories nor, through the home directory, the
user. What stays in the process (the thrown `praenora::Error`, `RunSummary`, the snapshots above,
the CLI's error output and the logs) keeps absolute paths.

| Placeholder | Directory |
|---|---|
| `<output>` | The run's output directory. |
| `<configuration>` | The directory of the configuration file. |
| `<modules:ORIGIN:INDEX>`, `<modules:installation>` | A module search path of the run, named as the run record's `execution.manifests` names it ([reproducibility.md](reproducibility.md#where-a-manifest-was-found)): ORIGIN `caller`, `configuration` or `environment` and INDEX its position among the non-empty entries of its origin, or the installation's directory. |
| `<providers:ORIGIN:INDEX>`, `<providers:installation>` | A provider search path, named the same way. |
| `<home>` | The user's home directory: `HOME` (when it is unset or empty, the user's entry in the password database) on POSIX systems, `USERPROFILE` on Windows. |

- The directories are fixed when the workflow is opened. Each matches in its lexical, normalized
  form, without a trailing separator, and in its form with symbolic links resolved (on macOS
  `/var/folders/...` and `/private/var/folders/...`); both give its placeholder. A root directory
  (`/`, `C:\`) and an empty or relative path are never matched, so a home directory that is unset,
  empty, relative or a root is skipped.
- A directory is replaced, and the rest of the path kept (`<output>/run-record.json`,
  `<modules:caller:0>/fx-echo/fx-echo.pmm.json`, `<home>/data/x.csv`), only where it is a whole
  path: preceded by the start of the string or by a character that does not continue a path
  segment, and followed by the end of the string, a separator, a character that does not continue
  a path segment, or a run of `.` that is followed by the end of the string or by a character that
  neither continues a segment nor is a separator, as at the end of a sentence. ASCII letters and
  digits, `.`, `_`, `-`, `~`, bytes outside ASCII and separators continue a segment; every other
  character does not, among them quotes, whitespace, `(`, `)`, `,`, `:`, `;`, `=`, `[` and `]`.
  So `/out` is replaced in `'/out/x'` and in `written to /out.`, but not in `/out2`, `/out-old`,
  `/out.d`, `/out./x`, `/out...x` or `/a/out`.
- On macOS, a directory that directly follows `/System/Volumes/Preboot/Cryptexes/<one segment>`,
  that prefix itself beginning at such a boundary, is replaced too, and the prefix is kept: dyld
  (macOS 13 and later) names a library it could not open a second time behind the OS cryptex's
  prefix, as in `'/System/Volumes/Preboot/Cryptexes/OS<modules:caller:0>/fx/lib.dylib' (no such
  file)`.
- Where several directories match at one place, the longest wins (an output directory in the home
  directory gives `<output>`). Of the placeholders of one directory, the first in the table's order
  wins, the search paths in search order.
- A string is scanned from left to right, and replaced text is never scanned again.
- On Windows `/` and `\` are both separators, either matching the other, and letters are compared
  without regard to ASCII case.
- Directories are replaced in the parts of an error record that can name files: its `message`,
  the `file` of its `location`, and every string of its `details` and of its `cause`, however
  deeply nested. Its `name`, `code`, `tick` and `at` and the `pointer` of its `location` (a JSON
  pointer, which can read like a path under a directory at the root, as `/library/path` does
  under a search path `/library`) are left as they are, and so are object keys and values that are
  not strings. Nothing outside the error records changes.
- This is best effort: a path under none of these directories is written as it is, for example a
  data file under `/scratch/<user>` or a user name that a module writes into its error document.
  The boundary rules have two edges: a directory whose name extends a known one by a character
  that does not continue a segment and more is taken for the known one (`/runs/out (old)` gives
  `<output> (old)` for the output directory `/runs/out`), and so is a directory literally named
  like a known one followed by dots, at the end of a text or before a character that ends a path
  (a quote, a space, a parenthesis): `/runs/out.` gives `<output>.`.

## Health

The workflow's `health` is decided by the first rule that applies:

| Health | Rule |
|---|---|
| `unhealthy` | The run failed, or an instance is `failed`, `unresponsive` or `crashed`. |
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
- Its error records name the run's directories by placeholders
  ([above](#known-directories-in-the-files-of-a-run)), so they differ from those of
  `Workflow::status_json()`, which keep absolute paths.
- `praenora status <file> [--json]` and `Runtime::read_status_file()` read it; a file of another kind
  is `DOCUMENT_KIND_MISMATCH`. A network endpoint is future work (OQ-9).

## Message trace

With `outputs.trace_messages: true` (or `praenora run --trace`) and an output directory, a run writes
every message that goes into or comes out of a step, and the runtime services each step looked up,
to `trace/messages.jsonl`:

- Each line is the canonical JSON of one record followed by `\n`: a message record or a lookup
  record. Message records have `direction`, lookup records `phase`.
- A message record is `{"tick", "step", "direction", "port", "envelope"}`, plus `"diagnostics"` when
  the step's result had a diagnostics object. An `in` record is written, before the call, for each
  envelope a step receives; workflow inputs appear there with source `inputs.<name>`. An `out`
  record is written for each envelope a step produces. A result with diagnostics but no outputs
  gets an `out` record with `port` and `envelope` null.
- A lookup record is `{"tick", "step", "phase", "service", "major", "provided", "count"}`: the
  lookups of runtime service `service` at major version `major` (`get_service`,
  [module-abi.md](module-abi.md#host-api)) that one call into the step's instance made. `phase`
  names the call (`create`, `initialize`, `invoke`, `finalize`, `destroy` or `diagnostics`), `tick`
  is the invocation's tick and null for every other call, `provided` says whether a table was
  returned and `count` how many lookups the record stands for. A call has one record per service,
  major version and outcome, in the order of each one's first lookup. A service name that is not
  valid UTF-8 is written with what is not UTF-8 replaced by U+FFFD.
- A call's lookup records are written as soon as it returns: for an invocation, after its `in`
  records and before its `out` records; for `create` and `initialize`, before tick 0, in schedule
  order; for `finalize` and `destroy`, at the end, in teardown order. The lookups of a call that
  does not succeed (an invocation whose result is invalid included), and of the `diagnostics` call
  that a failure makes, are written when the teardown starts, which is the same place, since
  nothing is written in between: a trace that cannot be written never hides how a call ended. Only
  lookups made while a call is in flight are traced, from any thread of the module (a call that
  never returns has those it made before it was abandoned); lookups between calls are only logged.
- The trace is byte-deterministic like the recorded outputs, for modules that are deterministic
  themselves: lookups that a module makes from several threads during one call are recorded in the
  order they happen. A tick cut short by a stop keeps the lines already written. The run record's
  `execution.trace` gives the file, its SHA-256 and the number of envelopes in it (lookup records
  and records with diagnostics only are not counted).
- Without an output directory no trace is written, and a warning (event `trace.skipped`) says so.
- Every runtime-service lookup is also logged (event `service.lookup`, debug, on the step's
  logger), traced or not, whether the service is a provider's or built in. Which provider serves
  each service of a run is recorded in the run record's reproducibility section (`services`), not
  in the trace or the logs ([reproducibility.md](reproducibility.md#run-fingerprint)).

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
- [ADR-0032](../adr/ADR-0032-service-lookups-in-the-message-trace.md): service lookups in the
  message trace.
- [OpenTelemetry logs data model](https://opentelemetry.io/docs/specs/otel/logs/data-model/).
- [error-taxonomy.md](error-taxonomy.md): the codes in error records.
- [user-guide.md](../user-guide.md#status-and-health): status files and health.
- [developer-guide.md](../developer-guide.md#logging): logging and tracing.
