# Python

The Python package `praenora.runtime` (distribution `praenora-runtime`) offers what the `praenora`
tool offers, with the same results: validating configurations and documents, listing, inspecting
and checking modules, workflow graphs, the protocol catalog and the schemas, and running workflows
with their status, metrics and outputs. It is a pure-Python layer over an extension that SWIG
generates from a narrow facade of the C++ API, which passes only strings and numbers
([ADR-0018](adr/ADR-0018-python-bindings-with-swig.md)). `praenora` is a namespace package.

It needs Python 3.10 or newer. The examples below were run on macOS arm64 with CPython 3.12,
from the repository root, against the `dev` build.

## Building the bindings

The bindings are built with `PRAENORA_BUILD_PYTHON=ON`, which needs SWIG 4.0 or newer and Python
with its headers (CMake's `Development.Module`). In the local presets, set `PRAENORA_BUILD_PYTHON`
and `Python3_EXECUTABLE` in `dev-local` ([developer-guide.md](developer-guide.md#local-presets)).
The build tree then holds the importable package in `build/<preset>/python/praenora/runtime/`: the
package's `__init__.py`, SWIG's proxy module `_bindings.py` and the extension
`_bindings_native.<suffix>` (for example `_bindings_native.cpython-312-darwin.so`).

The extension is built for one interpreter, the one CMake found, so use that one, with the build
tree on `PYTHONPATH`. The build's cache names it:

```bash
PY=$(sed -n 's/^Python3_EXECUTABLE:[A-Z]*=//p' build/dev/CMakeCache.txt)
PYTHONPATH=build/dev/python "$PY" -c 'import praenora.runtime as pr; print(pr.version()["runtime"])'
```

## A first session

```python
import praenora.runtime as pr

print(pr.version()["runtime"])

config = "examples/workflows/estimate-predict/workflow.yaml"
modules = ["build/dev/modules"]
print(pr.validate(config, module_paths=modules))

for module in pr.list_modules(module_paths=modules):
    print(module["name"], module["version"], module["capabilities"])

print(pr.graph(config=config, format="text"))

with pr.Workflow(config, module_paths=modules, log_level="warn") as wf:
    summary = wf.run()
    print(summary["state"], summary["ticks_completed"], summary["fingerprint"])
    print(wf.status()["workflow"]["health"])
```

`pr.validate()` returns `{'ok': True, 'diagnostics': []}`, and the run ends `completed` after 12
ticks with health `healthy`.

## Functions

Paths are `str` or `os.PathLike`; `module_paths` and `catalog_paths` are lists of paths, which play
the part of the tool's `--module-path` and `--catalog-path` options: the caller's module search
paths ([search order](specs/manifest.md#discovery)) and extra catalog directories
([specs/protocol.md](specs/protocol.md#catalog)). A single path where a list is expected is a
`TypeError`. Results are decoded JSON: what the tool's `--json` output holds as its `result`,
without the `{"ok": true, "result": ...}` around it.

| Function | Returns |
|---|---|
| `version()` | `{"runtime", "sdk", "pmp", "abi": {"major", "minor"}, "formats", "platform", "compiler", "build_type"}`. |
| `validate(config, module_paths=None, catalog_paths=None, load=False)` | The validation report, `{"ok": True, "diagnostics": [...]}`, whose diagnostics are warnings and notes; raises `PraenoraError` when it holds an error. `load=True` also loads each module and checks its identity. |
| `validate_document(path)` | The report for any Praenora document, checked against the schema of its kind; raises like `validate()`. |
| `list_modules(module_paths=None)` | Every manifest on the search paths, in search order: `[{"name", "version", "manifest_path", "library_path", "os", "arch", "capabilities"}]`. |
| `inspect_module(name, version=None, load=False, module_paths=None)` | One module as discovery resolves it (its highest version for this platform by default), with `"describe"` and `"library_sha256"` when `load=True`. |
| `check_module(manifest, vectors=None, *, catalog_paths=None)` | The contract checks' report, `{"kind", "format_version", "passed", "checks"}`; raises `PraenoraError` `FAILED` when a check failed. `catalog_paths` adds the catalog entries the module uses that are not built in, as `--catalog-path` does for `praenora modules check`. |
| `graph(config=None, workflow=None, format="json", *, module_paths=None, catalog_paths=None)` | The graph of the definition `workflow` (`"<name>@<version>"`) or of the one `config` pins: a dict for `"json"`, a string for `"text"`, `"dot"` and `"asyncapi"`. |
| `list_catalog(kind=None, *, catalog_paths=None)` | The catalog's entries, `[{"kind", "id", "version", "status", "summary"}]`, sorted; `kind` is `"workflows"`, `"capabilities"` or `"messages"`. |
| `catalog_entry(id, version, *, catalog_paths=None)` | One entry's document. |
| `list_schemas()` | The embedded schemas, `["<kind>@<major>.<minor>", ...]`. |
| `schema(kind, format_version=None)` | One schema, of the highest version by default. |
| `read_status(path)` | The status-snapshot document of a status file. |

## Workflow

```python
Workflow(config, module_paths=None, output_dir=None, seed=None, log_level=None, *,
         ticks=None, overwrite=False, trace_messages=None, status_file=None,
         log_format=None, log_file=None, catalog_paths=None)
```

The constructor validates the configuration, with its arguments as overrides
([configuration.md](configuration.md#overrides-and-precedence)), and prepares the run; nothing is
loaded yet. `seed` and `ticks` are whole numbers from 0 to 2^53 - 1; `log_level` and `log_format`
take the names of the tool's options in any case; `None` leaves a setting to the configuration. A
`with` block closes the run when it ends.

| Method or property | Does |
|---|---|
| `start()` | Loads the modules and creates and initializes an instance for each step. |
| `tick()` | Runs the next tick: `{"tick", "logical_time", "outputs", "finished"}`, `outputs` naming the workflow outputs it produced. |
| `run(ticks=None)` | Starts the run if needed, then runs ticks until it is finished, or until `ticks` more ran: `{"state", "ticks_completed", "fingerprint"}`. It returns for a completed or stopped run, and raises the failure of a failed one, again on every later call. |
| `request_stop()` | Asks the run to stop: the call in progress is asked to cancel and no further call starts. Safe from any thread. |
| `close()` | Ends a ready or running run as `stopped`, finalizes, destroys and unloads its modules, and refuses later control calls. Idempotent. |
| `status()` | The status snapshot (`{"kind", "format_version", "generated_at", "runtime_version", "pid", "workflow", "steps"}`). |
| `metrics()` | `{"metrics": [...]}`: the metric points, such as `praenora.step.invocations`. |
| `latest_outputs()` | The newest envelope of each workflow output, `{"<output>": envelope}`. |
| `run_record()` | The run record as last written; `None` before the run starts. |
| `state`, `fingerprint` | Properties: the state name and the run fingerprint. |

## Errors

Every failure of the runtime raises `praenora.runtime.PraenoraError`, with the attributes `code`
(for example 301), `name` (`"MODULE_NOT_FOUND"`), `category` (`"validation"`), `message`, `details`
(a dict) and `location` (a dict with any of `file`, `line`, `column` and `pointer`); `str()` gives
`"NAME: message"`. The codes are those of the [error taxonomy](specs/error-taxonomy.md).

`validate()`, `validate_document()` and `check_module()` raise when what they check fails, as the
tool exits with an error: the error carries the first problem's code and location, and `details`
holds the whole report, a validation report (`{"ok": False, "diagnostics": [...]}`) or the check
report. `Workflow(...)` raises the same way when the configuration is invalid.

```python
import praenora.runtime as pr

config = "examples/workflows/estimate-predict/workflow.yaml"
try:
    pr.validate(config)  # no module path: the example modules are not found
except pr.PraenoraError as error:
    print(error)
    print(error.code, error.name, error.category, error.location["line"])
    for diagnostic in error.details["diagnostics"]:
        print(diagnostic["location"]["line"], diagnostic["name"], diagnostic["message"])
```

```text
MODULE_NOT_FOUND: examples/workflows/estimate-predict/workflow.yaml: validation failed: 2 error(s)
301 MODULE_NOT_FOUND validation 28
28 MODULE_NOT_FOUND module example-estimator@0.1.0 was not found; searched '.../build/dev/lib/praenora/modules'
31 MODULE_NOT_FOUND module example-predictor@0.1.0 was not found; searched '.../build/dev/lib/praenora/modules'
```

Wrong arguments are `INVALID_ARGUMENT` (a tick count of `-1`, an unknown log level) or a `TypeError`
(a path of the wrong type).

## Threads and interruption

Every call into the runtime releases the GIL, so while one thread runs a workflow, another may call
its `status()`, `metrics()`, `latest_outputs()` or `request_stop()`. The control calls (`start`,
`tick`, `run`, `close`) are made one at a time.

Two workflows may also run at the same time on two threads, but the runtime serializes the module
calls of one run (and of one `check_module()`) only, not those of different runs. So two workflows,
or a workflow and a `check_module()`, must not run at the same time when they use the same module
library whose manifest keeps the default `features.concurrency: "module"`. Once any call into a
library has become unresponsive, whether it failed the run with `MODULE_UNRESPONSIVE` or was only the
last error of a `finalize` or `destroy` step of a run that ended `completed` or `stopped`, that
library must not be used again in the process: the abandoned call may still be running. Run such
workflows one after another or in separate processes, and restart the interpreter after an
unresponsive call
([specs/threading-and-ownership.md](specs/threading-and-ownership.md#concurrency-across-instances)).

Ctrl+C does not interrupt a blocking `run()`: Python handles the signal only when the call returns
to Python ([open-questions.md](open-questions.md), OQ-25). To stop a run on Ctrl+C, either run the
ticks in a loop, which returns to Python after each tick:

```python
import praenora.runtime as pr

config = "examples/workflows/estimate-predict/workflow.yaml"
with pr.Workflow(config, module_paths=["build/dev/modules"], log_level="warn") as wf:
    wf.start()
    while True:
        result = wf.tick()
        print(result["tick"], result["logical_time"], result["outputs"])
        if result["finished"]:
            break
```

(leaving the `with` block on `KeyboardInterrupt` closes the run as `stopped`), or run it on another
thread and request the stop from the main thread:

```python
import threading

import praenora.runtime as pr

config = "examples/workflows/estimate-predict/workflow.yaml"
with pr.Workflow(config, module_paths=["build/dev/modules"], log_level="warn") as wf:
    runner = threading.Thread(target=wf.run)
    runner.start()
    try:
        while runner.is_alive():
            runner.join(0.1)
    except KeyboardInterrupt:
        wf.request_stop()
        runner.join()
    print(wf.state)
```

## Logging

A run's `log_level`, `log_format` and `log_file` arguments are the run's options of
[specs/observability.md](specs/observability.md#settings), which ranks them as it ranks the tool's
`--log-level`, `--log-format` and `--log-file`. Records go to the process's standard error (file
descriptor 2), and also to the log file when one is set. Two limits:

- A step whose configuration sets its own `log_level` keeps that level: Python cannot make it
  quieter. The example's `predict` step logs at `debug` even with `log_level="warn"` or `"off"`.
- Python cannot turn off the runtime's logging to standard error. To keep the records out of a
  program's output, redirect file descriptor 2, or log to a file and silence the run with
  `log_level="off"` where no step sets its own level.

The few records of the runtime itself, outside a run (such as "runtime ready" at level `debug`),
follow `PRAENORA_LOG_LEVEL` and `PRAENORA_LOG_FORMAT` when they are set.

## Tests

CTest's `python.unittest` (label `python`) runs the `unittest` suite of `tests/python/` with the
interpreter the extension was built for, against the package in the build tree:

```bash
ctest --preset dev -L python -V
```

It sets `PYTHONPATH` to `build/<preset>/python`, the fixture and example locations the tests use
and `PRAENORA_LOG_LEVEL=off`, and unsets the other `PRAENORA_*` variables the runtime reads, so the
caller's environment cannot change the results. In builds with `PRAENORA_SANITIZERS` the test is
registered but disabled, because an instrumented extension does not load into an interpreter that is
not instrumented; `ctest --preset dev-asan -L python`, a run of that label alone, then exits with 8
([testing.md](testing.md#sanitizer-builds)).

## Building a wheel

Not yet verified: building a wheel needs scikit-build-core, which was not available where this was
written. The root [`pyproject.toml`](../pyproject.toml) builds the distribution `praenora-runtime`
with scikit-build-core (0.10 or newer) and SWIG (4.2 or newer, from PyPI or the system). It
configures this CMake project with the Python bindings on and the tests, the examples and the tool
off, so the C++ dependencies must be findable, for example through the `CMAKE_PREFIX_PATH`
environment variable:

```sh
CMAKE_PREFIX_PATH="/path/to/dependency/prefix" python3 -m pip wheel . --no-deps -w "${TMPDIR:-/tmp}/praenora-wheel"
```

A wheel build has only the Python package's install rules, so the wheel holds just
`praenora/runtime/`: `__init__.py`, `_bindings.py`, the extension and the runtime library, which the
extension finds beside itself (an RPATH of `$ORIGIN` on Linux, `@loader_path` on macOS; on Windows
the DLL lies next to the extension). The extension is specific to one Python version (no stable-ABI
wheel; [open-questions.md](open-questions.md), OQ-21).

The runtime's third-party shared libraries are not bundled into the wheel: yaml-cpp, spdlog and fmt,
for example, when they come from Homebrew or a Linux distribution as shared libraries. Such a wheel
works only where those libraries are installed at the same places, so it is not yet a portable
binary; bundling and manylinux wheels are open ([open-questions.md](open-questions.md), OQ-13).

## Platform notes

- **Windows** (not yet verified): the extension's suffix is `.pyd`, and `praenora_runtime.dll` and
  the third-party DLLs are copied beside it in the build tree, where Python looks for an
  extension's libraries. Debug builds link the release Python library.
- **Multi-configuration generators** (not yet verified): every configuration builds into the same
  package directory, so `PYTHONPATH` stays `build/<preset>/python`.
