# praenora-runtime
The Praenora digital twin virtual representation runtime

## Overview

Praenora is a configurable system for continuous modeling and forecasting. It takes in target data
as the data arrives, runs a workflow of scientific modules on every update (a *tick*), and keeps
its predictions current. The modules are developed in their own, separately versioned repositories.

`praenora-runtime` is the integration layer that holds those modules together. It provides the
runtime library (`Praenora::Runtime`), which validates a workflow configuration, discovers and
loads modules, and runs the workflow deterministically; the module SDK (`Praenora::ModuleSDK`)
with its stable C ABI; the Praenora Module Protocol (PMP) and Praenora Module Manifest (PMM)
formats; the `praenora` command-line tool; and Python bindings (`praenora.runtime`).

## Status

The MVP is complete: the runtime with its validation, engine, run records, status and metrics; the
module SDK (C ABI v0, C++ adapter, in-process test kit, `praenora_add_module()`); the protocol
schemas and catalog; the C++ API, the `praenora` tool and the Python bindings; two example modules
with an example workflow; the contract checks for modules; the installation with its CMake package
(`find_package(Praenora COMPONENTS Runtime ModuleSDK)`); the tests; and the documentation.

| Platform | Status |
|---|---|
| macOS arm64 (AppleClang) | verified |
| Linux x86-64 (GCC, Clang) | presets and portable code; not yet verified |
| Windows x86-64 (Visual Studio 2022) | presets and portable code; not yet verified |

Not yet verified anywhere: downloading dependencies in fetch mode, and a Python wheel build that
downloads its build requirements (a wheel built offline is verified on macOS arm64).
WebAssembly and the libRoadRunner and libAntimony services are designs only
([feasibility designs](docs/index.md#feasibility-designs)).

## Quick start

You need CMake 3.24 or newer, Ninja (Visual Studio 2022 on Windows), a C++17 compiler and the
dependencies listed in [docs/platforms.md](docs/platforms.md) for your platform. The build uses
installed packages only; `-DPRAENORA_FETCH_DEPENDENCIES=ON` downloads missing ones at pinned
versions.

Where your dependencies are installed goes into a local, git-ignored `CMakeUserPresets.json`.
Create it from the example, then set your dependency prefixes (`CMAKE_PREFIX_PATH`) and Python
interpreter in its `dev-local` preset:

```bash
cp CMakeUserPresets.json.example CMakeUserPresets.json
```

Its `dev*` presets inherit the macOS presets; on Linux or Windows, change each `inherits` entry to
the matching `linux-x64-*` or `windows-x64-*` preset. On macOS, when your dependency binaries were
built for a newer macOS than the project's default target of 11.0 (Homebrew builds for the running
release), also set `CMAKE_OSX_DEPLOYMENT_TARGET` there to that version, for example `"15.0"`, or
the linker warns about every such library
([docs/developer-guide.md](docs/developer-guide.md#local-presets)).

**macOS:**

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/praenora version
```

**Linux** (not yet verified) runs the same commands. **Windows** (not yet verified) builds with
Visual Studio, whose builds are multi-configuration:

```bat
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
build\dev\bin\Debug\praenora.exe version
```

The build puts the example modules in `build/dev/modules`. With them, the `praenora` tool
validates and runs the example workflow (macOS commands; output files go to a scratch directory):

```bash
P=./build/dev/bin/praenora
CONFIG=examples/workflows/estimate-predict/workflow.yaml
RUN="${TMPDIR:-/tmp}/praenora-example"
$P validate $CONFIG --module-path build/dev/modules
$P run $CONFIG --module-path build/dev/modules --output-dir "$RUN" --status-file "$RUN/status.json"
$P status "$RUN/status.json"
$P graph $CONFIG --format dot
$P modules list --module-path build/dev/modules
$P modules check build/dev/modules/example-estimator/example-estimator.pmm.json
$P catalog list
$P schema show module-manifest
```

`validate` reports every problem, located in its file; `run` writes the run record and the
workflow's outputs to the output directory (`--overwrite` replaces an earlier run there); `status`
shows the run's state, ticks and steps. Add `--json` to any command for exactly one JSON document on
standard output (`{"ok": true, "result": ...}` or `{"ok": false, "error": ..., "diagnostics":
[...]}`), and `--help` for its options. The exit code tells what happened: 0 ok, 1 execution
failed, 2 usage, 3 validation, 4 load, 5 timeout, 70 internal, 74 I/O, 130 interrupted or stopped.

## Where to go next

- [docs/user-guide.md](docs/user-guide.md): the `praenora` tool, the example workflow, outputs,
  status and troubleshooting.
- [docs/configuration.md](docs/configuration.md): writing a workflow configuration.
- [docs/module-developer-guide.md](docs/module-developer-guide.md): writing a module.
- [docs/python.md](docs/python.md): the Python API.
- [docs/developer-guide.md](docs/developer-guide.md) and [docs/testing.md](docs/testing.md):
  working on the runtime.
- [docs/architecture.md](docs/architecture.md): how it fits together.
- [docs/index.md](docs/index.md): every document, the specifications included.
- [AGENTS.md](AGENTS.md): guide for contributors and coding agents (commands, layout, conventions,
  rules).

## Licensing

Licensing has not been decided yet.
