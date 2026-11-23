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
formats; the `praenora` command-line tool, which also serves a control and status API over HTTP;
and Python bindings (`praenora.runtime`). The runtime can export its metrics and logs to an
OpenTelemetry collector over OTLP. Heavy engines
come from service providers that the runtime loads on demand: modules simulate models through the
simulation service `praenora.simulation@0`, and translate them from one format to another, such as
Antimony to SBML, through the model conversion service `praenora.model-conversion@0`, without
linking an engine themselves.

## Status

The MVP is complete: the runtime with its validation, engine, run records, status and metrics; the
module SDK (C ABI v0, C++ adapter, in-process test kit, `praenora_add_module()`); the protocol
schemas and catalog; the C++ API, the `praenora` tool and the Python bindings; two example modules
with an example workflow; the contract checks for modules; the installation with its CMake package
(`find_package(Praenora COMPONENTS Runtime ModuleSDK)`); the tests; and the documentation.

Since then: service providers, module manifests that declare the services a module requires, the
engine of each service in the run fingerprint, the simulation service `praenora.simulation@0`
([docs/specs/simulation-service.md](docs/specs/simulation-service.md)) and the model conversion
service `praenora.model-conversion@0`
([docs/specs/model-conversion-service.md](docs/specs/model-conversion-service.md)). Their
providers are optional. `roadrunner`, which simulates SBML models with libRoadRunner, is built only
with `-DPRAENORA_WITH_ROADRUNNER=ON` and an installed libRoadRunner 2.11 with its LLVM
([docs/platforms.md](docs/platforms.md#the-roadrunner-provider)), and is verified with
libRoadRunner 2.11.0 and LLVM 13.0.1 on macOS arm64 and, by CI, on Linux x86-64 and Windows x86-64.
`antimony`, which translates Antimony to SBML and back with libAntimony, is built only with
`-DPRAENORA_WITH_ANTIMONY=ON` and libAntimony 3.3 built from source
([docs/platforms.md](docs/platforms.md#the-antimony-provider)), and is verified with libAntimony
3.3.0 on the same platforms.

Then process isolation: a step's module instance can run in a module host
(`praenora-module-host`), a process of its own with the service providers its module uses, so that
a crash in the module fails only that step and a call that never returns is ended by killing the
module host. Steps in module hosts can share compiled models through a store in the runtime's
process, and on macOS a module host can apply a restricted sandbox profile to itself: no network,
no new processes and no writes outside its own temporary directory
([docs/specs/process-isolation.md](docs/specs/process-isolation.md)). Modules do not change, and
neither do the results. It is verified on macOS arm64 and, by CI, on Linux x86-64; Windows does
not have it in this version.

| Platform | Status |
|---|---|
| macOS arm64 (AppleClang) | verified, locally and by CI |
| Linux x86-64 (GCC, Clang) | verified by CI with GCC 13.3 in a Release build; the other presets and Clang not yet verified |
| Windows x86-64 (Visual Studio 2022) | verified by CI in Release and Debug builds; libraries from vcpkg or conda-forge not yet verified |

Fetch mode is verified by CI for yaml-cpp, spdlog, GoogleTest and cpp-httplib
([docs/releasing.md](docs/releasing.md#what-the-runs-verified)). Not yet verified anywhere:
downloading nlohmann/json and json-schema-validator in fetch mode (CI takes them from the bundle)
and a Python wheel
build that downloads its build requirements (a wheel built offline is verified on macOS arm64).
WebAssembly and compiling module code through libRoadRunner are designs only
([feasibility designs](docs/index.md#feasibility-designs)).

A GitHub Actions workflow builds and tests every pull request to `main` on the four platform keys
of the dependency bundles (Linux x86-64, macOS arm64, Windows x86-64 Release and Debug), against
the pinned bundles of praenora-runtime-deps, and drafts the releases, with an archive per platform
([docs/releasing.md](docs/releasing.md)). It first passed on every platform key on 2026-11-22,
and the first release, v0.2.0, was published from it the same day.

Since that release: a control and status API over HTTP and an OTLP export, both accepted
([ADR-0042](docs/adr/ADR-0042-control-and-status-api-over-http.md),
[ADR-0043](docs/adr/ADR-0043-otlp-export-of-metrics-and-logs.md)). The `praenora` tool serves the
API on a Unix socket or a TCP address, for the run it runs (`praenora run --listen`) or for runs
it opens on request from an operator's configuration directory (`praenora serve`), and
`praenora status --server` reads it: another process can watch a run, stop it, and read its
metrics, run record and latest outputs, with tokens and scopes, an OpenAPI 3.1 document and
Prometheus text ([docs/specs/control-api.md](docs/specs/control-api.md)). The runtime exports its
metrics and logs to an OpenTelemetry collector over OTLP/HTTP with JSON, configured by the
standard `OTEL_*` variables, from C++, the CLI and Python
([docs/specs/observability.md](docs/specs/observability.md#otlp-export)); every attribute key of
the logs and the metrics now carries the prefix `praenora.` (but `logger`). Neither changes a
run's results. They need cpp-httplib 0.67, header-only
([docs/platforms.md](docs/platforms.md#cpp-httplib)), and are verified on macOS arm64 locally and
by CI on every platform key, with cpp-httplib downloaded in fetch mode.

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

**Linux** runs the same commands (CI verifies the `ci-linux-x64-release` preset; a `dev` preset
on Linux is not yet verified). **Windows** builds with Visual Studio, whose builds are
multi-configuration (CI verifies the `ci-windows-x64-*` presets; a `dev` preset on Windows is not
yet verified):

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
  status, watching and running over HTTP, the OTLP export, and troubleshooting.
- [docs/configuration.md](docs/configuration.md): writing a workflow configuration.
- [docs/module-developer-guide.md](docs/module-developer-guide.md): writing a module.
- [docs/python.md](docs/python.md): the Python API.
- [docs/developer-guide.md](docs/developer-guide.md) and [docs/testing.md](docs/testing.md):
  working on the runtime.
- [docs/releasing.md](docs/releasing.md): the CI workflow, the dependency bundles it builds
  against, and making a release.
- [docs/architecture.md](docs/architecture.md): how it fits together.
- [docs/index.md](docs/index.md): every document, the specifications included.
- [AGENTS.md](AGENTS.md): guide for contributors and coding agents (commands, layout, conventions,
  rules).

## Licensing

This repository's files are under the MIT license ([LICENSE](LICENSE)), copyright The Praenora
developers ([ADR-0040](docs/adr/ADR-0040-mit-license.md)). The third-party code that the build
links statically is not: the service providers link their engines (libRoadRunner with LLVM and
libSBML, libAntimony) statically, and each keeps the license of its upstream project. Installations,
and release archives made from them, hold the license files in `share/praenora/licenses/`: this
repository's, those of the dependencies built in the tree, and those of the directories that
`PRAENORA_THIRD_PARTY_LICENSE_DIRS` names, such as the `licenses/` directory of a dependency bundle
([docs/developer-guide.md](docs/developer-guide.md#license-files)). Distributing the providers
raises questions that are still open: the terms of the NLEQ solvers and the statically linked
LGPL-2.1 libSBML (OQ-27), and libAntimony's license and those of its dependencies (OQ-29)
([docs/open-questions.md](docs/open-questions.md)).
