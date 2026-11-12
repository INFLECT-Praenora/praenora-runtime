# User guide

This guide is for people who run workflows: it covers getting the `praenora` tool, its commands,
running the example workflow, the files a run writes, watching a run's status, and what to do when
something fails. How to write a workflow configuration is in [configuration.md](configuration.md);
using the runtime from Python is in [python.md](python.md).

The commands below run from the repository root, against the `dev` build of
[developer-guide.md](developer-guide.md#configure-build-and-test), and were run on macOS arm64.
They are POSIX shell commands; Linux is not yet verified, and on Windows (not yet verified) the tool
is `build\dev\bin\Debug\praenora.exe` ([platforms.md](platforms.md)). Commands that write files
write them under `${TMPDIR:-/tmp}`.

## Getting the tool

A build puts the tool in `build/<preset>/bin`, the example modules in `build/<preset>/modules` and
the runtime library in `build/<preset>/lib`:

```bash
cmake --preset dev
cmake --build --preset dev
./build/dev/bin/praenora version
```

The `dev` preset is a local one: [developer-guide.md](developer-guide.md#local-presets) explains how
to create it, and [platforms.md](platforms.md) lists the dependencies of each platform.

An installation holds the tool, the runtime library, the headers, the CMake package and the
schemas and catalog. The example modules are installed only on request, as the component
`Examples`, into the installation's module directory, where the tool finds them without a search
path. An installation can be moved:

```bash
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install" --component Examples
"${TMPDIR:-/tmp}/praenora-install/bin/praenora" modules list
```

## Commands

| Command | Does |
|---|---|
| `praenora version` | Versions of the runtime, SDK, protocol (PMP), ABI and each document format; platform, compiler and build type. |
| `praenora validate <config>` | Everything a run checks before its first tick, without running: the file, the catalog, the graph, the bindings, the modules (without loading them), the parameters and the input data. `--load` also loads each module library and checks its identity. |
| `praenora validate --document <file>` | Checks any Praenora document (a manifest, a catalog entry, test vectors, a run record...) against the schema of its kind. |
| `praenora modules list` | Every module manifest on the module search paths. |
| `praenora modules inspect <name>[@<version>]` | One module as discovery resolves it (its highest version for this platform by default); `--load` adds its `describe()` document and its library's SHA-256. |
| `praenora modules check <manifest>` | The contract checks of a module ([module-developer-guide.md](module-developer-guide.md#contract-checks)). `--vectors F` uses other test vectors. |
| `praenora graph <config>` or `--workflow <name>@<version>` | A workflow definition's schedule and connections: `--format text` (default), `json`, `dot` or `asyncapi`. |
| `praenora run <config>` | Validates and runs a workflow. Options: `--ticks N`, `--seed S`, `--output-dir D`, `--overwrite`, `--trace`, `--status-file F`. |
| `praenora status <file>` | Prints the status file a run keeps. |
| `praenora catalog list` / `catalog show <id>@<version>` | The protocol catalog's entries (`--kind workflows`, `capabilities` or `messages`), or one entry's document. |
| `praenora schema list` / `schema show <kind>` | The embedded JSON Schemas; `--format-version V` picks a version. |
| `praenora help [<command>]` | The usage of the tool or of a command, as `--help` does. |

Every command takes these options, before or after its arguments:

| Option | Meaning |
|---|---|
| `--json` | Standard output holds exactly one JSON document: `{"ok": true, "result": ...}` or `{"ok": false, "error": ..., "diagnostics": [...]}`. Messages still go to standard error. |
| `--module-path P` | Search `P` for modules first (repeatable). |
| `--catalog-path P` | Add the catalog entries in `P` (repeatable). |
| `--log-level L` | `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off`. |
| `--log-format F` | `text` or `json`. |
| `--log-file F` | Also log to `F`. |
| `-h`, `--help` | The command's usage. |

The exit code says what happened:

| Exit code | Meaning |
|---:|---|
| 0 | Success. |
| 1 | A run or a check failed while executing (a step failed, an output was invalid, a contract check failed). |
| 2 | The command line is wrong. |
| 3 | Validation failed: the printed diagnostics say where. |
| 4 | A module library could not be loaded or checked. |
| 5 | A module call timed out or never returned. |
| 70 | An internal error, usually a defect of the runtime. |
| 74 | An environment error. `IO_ERROR`: a file or directory that is missing or unreadable, such as the configuration, a document, a manifest, a status file or a catalog directory; a file that cannot be written; or an output directory that holds an earlier run (a missing or unreadable data file is `TARGET_DATA_INVALID`, exit 3). `RESOURCE_EXHAUSTED`: the run ran out of memory or a worker thread could not start. |
| 130 | Interrupted or stopped. |

Each failure has an error code with a name, such as `PARAMETERS_INVALID`; the
[error taxonomy](specs/error-taxonomy.md) lists them all with their exit codes.

## Running the example workflow

The example workflow (`examples/workflows/estimate-predict/`, described in its
[README](../examples/workflows/estimate-predict/README.md)) has two steps: `estimate` blends each
tick's observations with the predictions of the tick before, through a delayed feedback connection,
and `predict` predicts forward from the new estimates. Its configuration names no module search
path, so each command gives the directory of the built example modules.

Validate it first; every problem would be reported, located in its file:

```bash
./build/dev/bin/praenora validate examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules
```

It prints `examples/workflows/estimate-predict/workflow.yaml: valid`. Its graph shows the schedule
and the delayed connection:

```bash
./build/dev/bin/praenora graph examples/workflows/estimate-predict/workflow.yaml
```

```text
workflow: example.estimate-predict@0.1.0
schedule: estimate, predict
connections:
  inputs.observations -> estimate.observations
  estimate.state -> predict.state
  predict.predictions -> estimate.prior [delay 1]
  predict.predictions -> outputs.predictions
  estimate.state -> outputs.state
```

`--format dot` prints a Graphviz graph (delayed connections dashed), `--format json` the same
facts as JSON, and `--format asyncapi` an AsyncAPI 3.0.0 document of the workflow's messages.

Then run it, with an output directory and a status file:

```bash
RUN="${TMPDIR:-/tmp}/praenora-example"
./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --output-dir "$RUN" --status-file "$RUN/status.json"
```

The log records go to standard error (the configuration logs its `predict` step at level `debug`),
and the summary to standard output:

```text
fingerprint       0aed6a505e47139a31129591cf69e76fec1366e1dbc590264bfb5b7b3493ab5e
ticks             12
output directory  /tmp/.../praenora-example
state             completed
```

The fingerprint identifies everything that determines the run's results: the same configuration,
data, catalog entries, modules and runtime give the same fingerprint and the same outputs, byte for
byte ([specs/reproducibility.md](specs/reproducibility.md)). It depends on the module libraries'
digests, so it differs from one build to another.

A second run into the same directory is refused with exit code 74, because the directory holds a
run record (`IO_ERROR`: "output directory already contains a run record; use overwrite").
`--overwrite` replaces the earlier run, its status file included; this run keeps a new status file,
traces every message and runs only three ticks:

```bash
./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --output-dir "$RUN" --overwrite --trace --ticks 3 \
    --status-file "$RUN/status.json"
```

Overrides such as `--ticks` and `--seed` are recorded in the run record, and change the
fingerprint; `--output-dir`, `--trace`, `--status-file` and the logging options do not.

## Finding modules

A configuration names modules by name and exact version. The runtime looks for their manifests
(`*.pmm.json`) in each module search path and its immediate subdirectories, and the first match
wins; [specs/manifest.md](specs/manifest.md#discovery) lists the search paths in the order they are
searched. A module is native code that runs with the tool's privileges, so put only modules you
trust on these paths ([specs/module-abi.md](specs/module-abi.md#trust)).

Instead of `--module-path`, the environment variable `PRAENORA_MODULE_PATH` can name the directory:

```bash
PRAENORA_MODULE_PATH=build/dev/modules ./build/dev/bin/praenora modules list
```

`praenora modules list` shows what was found, other platforms' manifests included, and
`praenora modules inspect <name>@<version> --load` loads one module to show what it reports about
itself. Discovery reads manifests only: no module code runs until a run starts, `--load` is given
or the contract checks run.

## What a run writes

Without an output directory (`--output-dir`, or `outputs.directory` in the configuration) a run
writes nothing to disk. With one, it writes:

| File | Content |
|---|---|
| `run-record.json` | The run record: the `reproducibility` section that the fingerprint hashes (versions, workflow and catalog digests, time base, seed, ticks, timeouts, input digests, and per step the module, its manifest and library digests, parameters and seed), and the `execution` section (start and finish times, status, ticks completed, the error of a failed run, overrides, platform, each output file's digest and message count, the final status snapshot). It is written when the run starts and rewritten when it ends. |
| `outputs/<output>.jsonl` | One envelope per line for each recorded workflow output (`outputs.record`, by default all of them). |
| `trace/messages.jsonl` | With `--trace` (or `outputs.trace_messages: true`): every envelope into and out of a step, as `{"tick", "step", "direction", "port", "envelope"}`, plus the result's `diagnostics` when it has them. |
| `status.json` | The status file, when it is configured inside the directory. |

Every file is UTF-8 with `\n` line endings, and the outputs and the trace are byte-deterministic.
An envelope carries the data of one output and where it came from:

```bash
ls "$RUN" "$RUN/outputs"
head -n 1 "$RUN/outputs/state.jsonl"
```

```text
{"data":{"estimates":[{"basis":"observation","series_id":"S01","time":0.0,"value":10.0,"variable_id":"population"},{"basis":"observation","series_id":"S02","time":0.54,"value":28.191,"variable_id":"population"}],"time_unit":"d"},"id":"<16 hex digits>:0:estimate.state","logical_time":1.0,"pmp_version":"0.1.0","source":"estimate.state","tick":0,"type":"example.state-estimate","type_version":"0.1.0"}
```

An overwrite removes only these managed files (`run-record.json`, `outputs/`, `trace/`,
`status.json`); other contents of the directory stay. A log file must not lie among the managed
files, because an overwrite would remove it (`CONFIG_INVALID`); `logs/praenora.log` inside the
directory is fine. The formats are specified in
[specs/reproducibility.md](specs/reproducibility.md#output-files) and
[specs/protocol.md](specs/protocol.md#envelope).

## Status and health

A status file (`--status-file`, or `observability.status_file`) holds a status snapshot that the run
rewrites atomically at each state change and every `status_interval` (1 s by default), so another
process can read it at any time:

```bash
./build/dev/bin/praenora status "$RUN/status.json"
```

```text
workflow         example.estimate-predict@0.1.0
state            completed (health healthy)
ticks completed  3
fingerprint      9f20a99603afc90d2549e819f646a61cf3f544871b3bc2c0b8c90495aaa73220
started at       2026-11-11T19:23:59.185074000Z
generated at     2026-11-11T19:23:59.242441000Z
process          88909
steps:
  estimate  example-estimator@0.1.0  destroyed  3 invocations (3 succeeded, 0 failed, 0 timed out, 0 cancelled)
  predict   example-predictor@0.1.0  destroyed  3 invocations (3 succeeded, 0 failed, 0 timed out, 0 cancelled)
```

A workflow's state is one of `created`, `starting`, `ready`, `running`, `stopping`, `completed`,
`stopped` and `failed` ([architecture.md](architecture.md#states)). Its health is `unknown` before
it starts, `unhealthy` when the run failed or an instance failed or never returned, `degraded` when
a timeout, cancellation or failure was counted without failing the run, when the run is stopping or
stopped, or when the status file could not be written, and `healthy` otherwise. With `--json`,
`praenora status` prints `{"ok": true, "result": <the snapshot>}`, as every command wraps its
result; the snapshot's fields are specified in
[specs/observability.md](specs/observability.md#status-snapshot).

## Stopping a run

Ctrl+C or SIGTERM asks a run to stop: the module call in progress is asked to cancel, no further
call starts, the modules are finalized and destroyed, the run record is written with status
`stopped`, and the tool exits with 130. A second signal ends the tool at once. A tool started with
SIGINT ignored, as a shell starts a background command, keeps ignoring it.

## Logging

Records go to standard error, and also to a file with `--log-file`. The level, format and file are
set by the command's options (`--log-level`, `--log-format`, `--log-file`), the environment or the
configuration's `observability` section; which source wins is specified in
[specs/observability.md](specs/observability.md#settings). `--log-format json` writes one JSON
object per record, with OpenTelemetry field names:

```text
{"timestamp":"2026-11-11T18:34:17.178992000Z","severity_text":"INFO","severity_number":9,"body":"run started","attributes":{"logger":"praenora.engine","run.fingerprint":"a4d251a5be737c49fe7cc73b30aa65367f3bc8e55cb2818ebb12bbafed93f46d","event":"run.started","workflow":"example.estimate-predict@0.1.0"}}
```

A step whose configuration sets its own `log_level` keeps that level whatever the run's level: the
example's `predict` step logs at `debug` even with `--log-level warn`. Logging never changes the
fingerprint. The loggers, events and formats are in
[specs/observability.md](specs/observability.md#logs).

## Troubleshooting

Validation reports every problem it finds, one line each, in a compiler-style format:
`file:line:col: severity[NAME]: pointer: message`. For example, a copy of the example with a gain
above the module's maximum:

```bash
BAD="${TMPDIR:-/tmp}/praenora-bad"
mkdir -p "$BAD/data"
cp examples/workflows/estimate-predict/data/observations.csv "$BAD/data/"
sed 's/gain: 0.6/gain: 1.5/' examples/workflows/estimate-predict/workflow.yaml > "$BAD/workflow.yaml"
./build/dev/bin/praenora validate "$BAD/workflow.yaml" --module-path build/dev/modules
```

```text
/tmp/.../praenora-bad/workflow.yaml:29:25: error[PARAMETERS_INVALID]: /steps/estimate/parameters/gain: instance exceeds maximum of 1.0 (parameter schema of module example-estimator@0.1.0)
praenora: /tmp/.../praenora-bad/workflow.yaml: validation failed: 1 error(s)
```

The command exits with 3. With `--json`, standard output holds
`{"ok": false, "error": ..., "diagnostics": [...]}`, whose `diagnostics` list holds the same
diagnostics, each with its code, name, location (file, line, column, JSON pointer) and, when set,
step and module.

| Symptom | Likely cause and fix |
|---|---|
| `MODULE_NOT_FOUND`, exit 3 | No search path holds the module at that version. The message lists the searched paths and the versions found; add `--module-path` or set `PRAENORA_MODULE_PATH`, and check with `praenora modules list`. |
| `PLATFORM_MISMATCH` | Only manifests for other platforms were found: build or install the module for this one. |
| `PARAMETERS_INVALID` | A step's parameter breaks the module's parameter schema; `praenora modules inspect <name>@<version>` shows the module, and its manifest holds the schema. |
| `SCHEMA_VIOLATION` | A misspelled key or a value of the wrong type, at the JSON pointer given. |
| `TARGET_DATA_INVALID` | The input data: the message names the file and line ([configuration.md](configuration.md#observation-csv)). |
| `IO_ERROR`, exit 74 | A missing or unreadable file or directory (the configuration, a manifest, a catalog directory), a file that cannot be written, or an output directory with an earlier run (add `--overwrite`). A missing data file is `TARGET_DATA_INVALID` instead (exit 3). |
| `LIBRARY_LOAD_FAILED`, exit 4 | The operating system could not load the module library, for example because a library it needs is missing; the message holds the system's text. |
| `STEP_FAILED`, exit 1 | A module returned an error. Its own code and message are in the error's `cause`; its log records are on the logger `module.<step>`. |
| `STEP_TIMEOUT`, `MODULE_UNRESPONSIVE`, exit 5 | A call ran past its deadline (`run.timeouts`, `steps.<step>.timeouts`); an unresponsive module ignored the cancellation and never returned. |
| `RUN_STOPPED`, exit 130 | The run was stopped on request. |

The [error taxonomy's triage table](specs/error-taxonomy.md#triage) says where to look for every
code. For problems with a module itself, `praenora modules check <manifest>` runs its contract
checks and names the check that fails.
