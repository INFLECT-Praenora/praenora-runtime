# AGENTS.md

Guide for coding agents and human contributors working in `praenora-runtime`. Read it before
changing anything.

## Purpose

Praenora is a configurable system for continuous modeling and forecasting. This repository is its
integration layer:

- **Runtime (host):** `praenora_runtime` (`Praenora::Runtime`) validates a workflow configuration,
  discovers and loads modules, runs the workflow deterministically and reports status.
- **Module SDK:** `Praenora::ModuleSDK`, the C ABI v0 headers plus an optional header-only C++
  adapter, a CMake helper and an in-process test kit.
- **Protocol:** the Praenora Module Protocol (PMP) and Praenora Module Manifest (PMM) documents,
  their JSON Schemas and the protocol catalog.
- **Surfaces:** the C++ API, the `praenora` CLI and the Python package `praenora.runtime`.

Modules implement the science. They live in their own repositories and build against the SDK;
this repository holds only example and test modules.

## Status

The MVP is complete: the runtime (validation, discovery, loading, the engine with its run record,
outputs and message trace, metrics, status and health), the module SDK, the protocol schemas and
catalog, the C++ API, the `praenora` CLI, the Python bindings, the example modules and workflow,
the contract checks, the install rules and CMake package, the performance probe, the tests and the
documentation. It is verified on macOS arm64 with AppleClang (Debug, AddressSanitizer and
ThreadSanitizer builds, and a Python wheel built offline). Linux x86-64 and Windows x86-64 have
presets and portable code but are not yet verified; neither are fetch-mode downloads and a wheel
build that downloads its build requirements, on any platform.

## Setup and commands

**Dependencies.** CMake 3.24+; Ninja (Linux, macOS) or Visual Studio 2022 (Windows); GCC 9+,
Clang 10+, AppleClang 13+ or MSVC 19.29+; nlohmann/json 3.11+, json-schema-validator (pboettch)
2.3+, yaml-cpp 0.8+ (0.7 works through a target shim), spdlog 1.10+, GoogleTest 1.11+; for the
Python bindings SWIG 4.0+ and Python 3.10+ with headers.

- macOS: `brew install cmake ninja swig nlohmann-json yaml-cpp spdlog googletest`
- Ubuntu 24.04+ (not yet verified): `sudo apt install cmake ninja-build g++ nlohmann-json3-dev
  libyaml-cpp-dev libspdlog-dev libgtest-dev swig python3-dev`
- Fedora (not yet verified): `sudo dnf install cmake ninja-build gcc-c++ json-devel yaml-cpp-devel
  spdlog-devel gtest-devel swig python3-devel`
- Windows (not yet verified): Visual Studio 2022 with the libraries from vcpkg or conda-forge.
- json-schema-validator is needed on every platform and is not packaged by Homebrew, apt or dnf.
  Build it into a local prefix with the commands in
  [docs/platforms.md](docs/platforms.md#json-schema-validator), which clone it outside the
  checkout, then list that prefix in `CMAKE_PREFIX_PATH` when configuring this project.
- Instead of installing packages, configure with `-DPRAENORA_FETCH_DEPENDENCIES=ON`: installed
  packages are still preferred and only missing ones are downloaded, at pinned commits. A default
  build never uses the network.

**Local presets.** Copy `CMakeUserPresets.json.example` to `CMakeUserPresets.json` (git-ignored)
and edit its hidden `dev-local` preset: `CMAKE_PREFIX_PATH` (dependency prefixes, `;`-separated),
`Python3_EXECUTABLE` and `PRAENORA_FETCH_DEPENDENCIES`. On macOS, add
`CMAKE_OSX_DEPLOYMENT_TARGET` only when your dependency binaries target a newer macOS than the
project default of 11.0 (Homebrew builds for the running release); otherwise the linker warns
about every such library. The `dev`, `dev-release`, `dev-asan` and `dev-tsan` presets inherit
`macos-arm64-*`; on Linux or Windows, inherit `linux-x64-*` or `windows-x64-*` instead.

**Configure, build, test.** Trees go to `build/<preset>`; installs go to `install/<preset>`.

```bash
cmake --preset dev                             # prints one line per dependency
cmake --build --preset dev
ctest --preset dev                             # output is shown for failing tests
./build/dev/bin/praenora version --json
ctest --preset dev -L unit                     # one label
ctest --preset dev -R '^Version\.'             # one suite (CTest names are Suite.Case)
ctest --preset dev --output-junit junit.xml    # JUnit XML in build/dev/junit.xml
ctest --preset dev -L python -V                # the Python tests, each one listed
PYTHONPATH=build/dev/python python3 -c 'import praenora.runtime as pr; print(pr.version())'
cmake --install build/dev                      # into install/dev; --prefix <dir> for another
cmake --install build/dev --component Examples # the example modules, into lib/praenora/modules
ctest --preset dev -L perf -V                  # the performance probe's two JSON reports
ctest --preset dev -L docs -V                  # the documentation checked against the tree
```

The Python line needs the interpreter the build found (`Python3_EXECUTABLE`): the extension is
built for one Python version. An installation holds the `praenora` tool, the runtime library,
the headers, the CMake package (`lib/cmake/Praenora`: `find_package(Praenora COMPONENTS Runtime
ModuleSDK)`; `ModuleSDK` alone needs no other package) and the schemas and catalog
(`share/praenora`); it can be moved. The example modules are installed only with
`PRAENORA_INSTALL_EXAMPLES=ON` or `--component Examples`. A wheel build (scikit-build-core) has
only the Python package's install rules.

Project presets: `linux-x64-{debug,release,asan,tsan}`, `macos-arm64-{debug,release,asan,tsan}` and
`windows-x64-{debug,release}`, each with configure, build and test presets. Labels: `unit`,
`protocol`, `contract`, `negative`, `lifecycle`, `integration`, `cli`, `python`, `install`, `perf`,
`docs`. The `cli` label holds the CLI driver tests, named `cli.<command>.<case>`, and the
`CliTable.*` and `Frontend.*` unit tests of the tool's helpers and of what it shares with the Python
extension (`ctest --preset dev -L cli`). The `python` label
holds one CTest test, `python.unittest`, which runs the unittest suite of `tests/python` against the
package in `build/<preset>/python`; with `PRAENORA_SANITIZERS` set it is registered but disabled,
because an instrumented extension does not load into an interpreter that is not instrumented. So a
label-only run of a sanitizer tree (`ctest --preset dev-asan -L python`, or `dev-tsan`) runs no test
and exits 8, since the presets set `noTestsAction: error`; a full run of those trees is unaffected.
The `install` label holds `install.consumer`: its driver, `tests/install/run_install_test.cmake`,
installs the build into `build/<preset>/tests/install/work`, builds the consumer projects next to it
and lists every step it checks; builds without install rules (`PRAENORA_INSTALL=OFF`) do not have
it. The `perf` label holds `perf.validation_strict` and `perf.validation_off`, which run
`praenora_perf` for 2000 ticks and print its JSON report; they fail only when the probe fails or
prints no report, never on timings. Both labels also pass in the sanitizer trees, whose flags reach
the consumer builds. The `docs` label holds `docs.consistency`, the CMake script
`tests/docs/check_docs.cmake`: every `ErrorCode` name is in `docs/specs/error-taxonomy.md`, every
`spec/schemas/*.json` name in `docs/specs/protocol.md`, every ADR in `docs/adr/README.md`, every
relative Markdown link of `README.md`, `AGENTS.md` and `docs/` resolves, and the open-questions
register has its 26 rows. Build options are in `cmake/PraenoraOptions.cmake`;
[docs/testing.md](docs/testing.md) has the details.

## Repository map

```
CMakeLists.txt CMakePresets.json CMakeUserPresets.json.example AGENTS.md README.md
pyproject.toml                Python distribution praenora-runtime (scikit-build-core)
cmake/                        options, versions, compiler checks, warnings, sanitizers,
                              dependencies, praenora_add_module(), praenora_embed_resources(),
                              install rules and package configuration (PraenoraInstall.cmake,
                              PraenoraConfig.cmake.in)
include/praenora/runtime/     public C++ API (runtime.hpp, workflow.hpp, version.hpp, error.hpp,
                              status.hpp, options.hpp; export.h is generated)
include/praenora/sdk/         SDK: abi.h, status.h, module.hpp, testing.hpp (version.h generated)
src/runtime/                  runtime internals, one directory per component (api/, catalog/,
                              compat/, config/, core/, discovery/, document/, engine/, graph/,
                              instance/, io/, loader/, observability/, platform/{posix,windows}/)
src/sdk/                      Praenora::ModuleSDK target
src/frontend/                 what the CLI and the Python extension share: frontend.hpp
                              (praenora::frontend; header-only, internal, never installed)
src/cli/                      praenora command-line tool: main.cpp, args, output, signals,
                              commands/ (one file per command)
src/python/                   Python extension: SWIG interface praenora_runtime.i and the facade
                              (facade.hpp, facade.cpp) it wraps
python/praenora/runtime/      pure-Python package praenora.runtime (praenora: namespace package)
spec/                         schemas/, catalog/, asyncapi/ (generated AsyncAPI documents,
                              committed; refreshed with PRAENORA_UPDATE_GOLDEN=1)
examples/                     modules/estimator/, modules/predictor/ (the example modules) and
                              workflows/estimate-predict/ (configuration, data, README)
tests/                        unit/, protocol/, negative/, contract/, lifecycle/, integration/,
                              cli/ (CLI tests and their driver run_cli.cmake), python/ (unittest
                              suite of praenora.runtime), install/ (install test: its driver
                              run_install_test.cmake and the projects consumer-module/ and
                              consumer-app/), perf/ (performance probe praenora_perf and its
                              driver run_perf.cmake), docs/ (documentation test
                              check_docs.cmake), support/ (test helpers, and
                              run_command.cmake, which the three drivers share);
                              fixtures/: modules/ (fixture modules), documents/, catalog/,
                              catalog-bad/, manifests/ (bad manifests, search-path layouts),
                              configs/ (good, bad and CLI configurations), data/ (target data),
                              golden/ (golden outputs)
tools/                        developer scripts (generate_example_data.py)
docs/                         index.md, the guides (architecture, user-guide, configuration,
                              developer-guide, module-developer-guide, testing, python,
                              platforms), specs/ (normative specifications), feasibility/
                              (designs only), adr/ (decision records), open-questions.md
```

## Architecture in five bullets

- The runtime compiles once into the OBJECT library `praenora_runtime_objects`, linked into the
  shared `praenora_runtime` (only the public API is exported) and into white-box test executables.
  Its two front ends, the CLI and the Python extension, use only the public API and share their
  documents, failures and names through the header-only internal `praenora_frontend`.
- Modules are independently built `MODULE` libraries, found through `*.pmm.json` manifests on
  module search paths, loaded with `dlopen`/`LoadLibraryExW` and called only through the ABI v0
  function table returned by `praenora_module_get_api`.
- Each module instance has its own worker thread and calls into it are serialized. The host
  enforces timeouts; cancellation is cooperative.
- A YAML workflow configuration pins a versioned workflow definition from the protocol catalog and
  binds its steps to modules; connections come from the definition, never from the YAML. Every
  document is validated against an embedded JSON Schema (draft-07) before anything runs.
- Execution is a sequence of synchronous ticks in deterministic order; feedback uses connections
  with `delay: 1`. Each run has a fingerprint and a run record; status, metrics and logs are
  available from C++, the CLI and Python.

## Conventions

- **Language.** C++17 without extensions; C99 for the ABI headers. snake_case functions, variables
  and files; PascalCase types; internals in `praenora::detail` under `src/runtime/<component>/`;
  what both front ends need in `praenora::frontend` (`src/frontend/`), never copied into one.
  Format with `.clang-format`; `.clang-tidy` holds the static checks.
- **Errors.** Throw `praenora::Error` with an error-taxonomy code. Validation collects every
  diagnostic in a report instead of stopping at the first.
- **Public headers** use only the standard library and Praenora types (no nlohmann, spdlog or
  yaml-cpp types); third-party dependencies link PRIVATE; exports use `PRAENORA_RUNTIME_API`.
- **ABI.** Only C types cross the module boundary (fixed-width integers, `size_t`, pointers,
  length-delimited UTF-8); no C++ types, exceptions or allocator ownership. Minor ABI changes only
  append fields and function pointers.
- **Determinism.** No unordered-container iteration reaches an output; no wall-clock time in
  envelopes or reproducibility data; randomness only from seeds derived from the run seed.
  Canonical JSON is nlohmann `dump()` without indentation.
- **Text.** UTF-8 everywhere, LF line endings (`.gitattributes`, `.editorconfig`); paths travel as
  UTF-8 strings.
- **Tests.** Add GoogleTest executables with `praenora_add_gtest()` (`tests/CMakeLists.txt`) and
  fixture modules with `praenora_add_fixture_module()` (`tests/fixtures/modules/CMakeLists.txt`).
  Tests are named `Suite.Case` in PascalCase, where the suite names the component (for example
  `Loader.LoadsModuleFromUnicodePath`); the case may end in a `_WhenCondition` suffix. Python
  tests are stdlib `unittest` modules `tests/python/test_*.py` (classes `<Topic>Test`, methods
  `test_<what>`), which `python.unittest` discovers; what they share (CTest's environment, fixture
  paths) is in `tests/python/support.py`. Write temporary files only under the system
  temporary directory or the build tree. Keep the build warning-free
  (`PRAENORA_WARNINGS_AS_ERRORS=ON` enforces it).
- **Agent standards.** Use `AGENTS.md` (this file) and the `agentskills.io` standard for agent skills. 
  Use local symlinks as necessary for support in non-supporting harnesses.

## Change rules

- Schema, catalog and documentation changes travel together.
- An ABI or protocol change bumps its version (ABI: `include/praenora/sdk/abi.h`; SDK, PMP and
  document formats: `cmake/PraenoraVersions.cmake`; runtime package: `project(VERSION)` in
  `CMakeLists.txt`) and adds an ADR in `docs/adr/`.
- Resolving an open question updates its row in `docs/open-questions.md` and adds or supersedes an
  ADR. Calls made while building go into the relevant ADR as "Builder notes" (Proposed).
- Golden files, and the committed AsyncAPI documents in `spec/asyncapi/`, are refreshed only with
  `PRAENORA_UPDATE_GOLDEN=1`, never implicitly; regenerate the AsyncAPI documents whenever a
  workflow definition, a message type or the envelope schema changes.

## Prohibitions

- No license file and no `license` field anywhere: licensing has not been decided.
- No vendored dependencies: they come from `find_package` or, opt-in, pinned FetchContent.
- No network access in tests or in a default build.
- `BUILD_SHARED_LIBS` is never read: the runtime is always SHARED, modules always MODULE.
- Never commit build trees, install trees or `CMakeUserPresets.json`.

## Docs map

- [README.md](README.md): overview and quick start.
- [docs/index.md](docs/index.md): map of all documentation.
- Guides: [architecture](docs/architecture.md), [user guide](docs/user-guide.md),
  [configuration](docs/configuration.md), [developer guide](docs/developer-guide.md),
  [module developer guide](docs/module-developer-guide.md), [testing](docs/testing.md),
  [Python](docs/python.md), [platforms](docs/platforms.md).
- Normative specifications: `docs/specs/` (ABI, protocol, manifest, workflow definitions,
  compatibility, error taxonomy, threading and ownership, time and timeouts, observability,
  reproducibility, the draft starting workflow), listed in [docs/index.md](docs/index.md).
- Feasibility designs, nothing of which is built:
  [WebAssembly](docs/feasibility/webassembly.md),
  [libRoadRunner](docs/feasibility/libroadrunner.md),
  [libAntimony](docs/feasibility/libantimony.md).
- [docs/adr/README.md](docs/adr/README.md): architecture decision records and their process.
- [docs/open-questions.md](docs/open-questions.md): open-questions register.
