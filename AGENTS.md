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
documentation. It is verified on macOS arm64 with AppleClang (Debug, Release, AddressSanitizer
and ThreadSanitizer builds, and a Python wheel built offline). Linux x86-64 and Windows x86-64 have
presets and portable code but are not yet verified; neither are fetch-mode downloads and a wheel
build that downloads its build requirements, on any platform.

Since the MVP: service providers (engines loaded on demand, one per process, listed by
`praenora providers list`), module manifests 0.2 that declare the services a module requires, run
records 0.2 that put each service's provider and engine into the fingerprint, and the simulation
service `praenora.simulation@0` (`simulation.h`; level A simulates, level B hands out the compiled
model's functions) with the fixture provider `test-simulation`, and the model conversion service
`praenora.model-conversion@0` (`model_conversion.h`; translating one model source, such as Antimony
to SBML and back) with the fixture provider `test-conversion`. The optional provider `roadrunner`
(libRoadRunner) is built only with `PRAENORA_WITH_ROADRUNNER=ON`; it is verified on macOS arm64
with libRoadRunner 2.11.0 and LLVM 13.0.1 (every test of a Debug build; the labels `unit`,
`contract`, `negative`, `lifecycle`, `integration` and `install` of a ThreadSanitizer build). The
optional provider `antimony` (libAntimony) is built only with `PRAENORA_WITH_ANTIMONY=ON`; it is
verified on macOS arm64 with libAntimony 3.3.0, built from source against libRoadRunner 2.11.0's
dependencies (every test of a Debug build, alone and with the roadrunner provider; the labels
`unit`, `contract` and `integration` of a ThreadSanitizer build). Compiling module code through
libRoadRunner (level C) is future work, and so are hierarchical SBML output, CellML and a
WebAssembly build of the conversion engine.

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
- For sanitizer builds, `-DPRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON` builds json-schema-validator in
  the tree from its pinned commit, with the sanitizer flags, instead of linking the installed,
  uninstrumented library; it downloads the sources unless
  `FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON_SCHEMA_VALIDATOR` names a local copy
  ([docs/platforms.md](docs/platforms.md#json-schema-validator)).
- Optional, for the roadrunner provider only: an installed libRoadRunner 2.11 (its static library
  `roadrunner-static` and the packages it links) and the LLVM 13 it was built with, never
  downloaded; [docs/platforms.md](docs/platforms.md#the-roadrunner-provider) says what to install
  and how CMake finds it.
- Optional, for the antimony provider only: libAntimony 3.3 built from source as a static library
  (`antimony-static`) against libSBML (with comp), SBMLNetwork and expat, such as libRoadRunner's
  dependencies, never downloaded; [docs/platforms.md](docs/platforms.md#the-antimony-provider) says
  how to build it and how CMake finds it.
- Optional, for the AsyncAPI parser tests only: Node.js and `@asyncapi/parser` 3.6.6, installed
  with npm into a directory outside the checkout, never downloaded by the build;
  [docs/testing.md](docs/testing.md#the-official-asyncapi-parser) says how.

**Local presets.** Copy `CMakeUserPresets.json.example` to `CMakeUserPresets.json` (git-ignored)
and edit its hidden `dev-local` preset: `CMAKE_PREFIX_PATH` (dependency prefixes, `;`-separated),
`Python3_EXECUTABLE` and `PRAENORA_FETCH_DEPENDENCIES`. On macOS, add
`CMAKE_OSX_DEPLOYMENT_TARGET` only when your dependency binaries target a newer macOS than the
project default of 11.0 (Homebrew builds for the running release); otherwise the linker warns
about every such library. The `dev`, `dev-release`, `dev-asan` and `dev-tsan` presets inherit
`macos-arm64-*`; on Linux or Windows, inherit `linux-x64-*` or `windows-x64-*` instead. For the
roadrunner provider, add a hidden `dev-roadrunner-local` preset (the libRoadRunner and LLVM
prefixes in `CMAKE_PREFIX_PATH`, `LLVM_INSTALL_PREFIX`, `PRAENORA_WITH_ROADRUNNER=ON`) and the
presets `dev-roadrunner` and `dev-roadrunner-tsan`, which inherit it before `dev-local`
([docs/developer-guide.md](docs/developer-guide.md#the-roadrunner-provider)). For the antimony
provider, likewise a hidden `dev-antimony-local` preset (the libAntimony and libSBML prefixes,
`PRAENORA_WITH_ANTIMONY=ON`) with the presets `dev-antimony` and `dev-antimony-tsan`, and for both
providers a hidden `dev-engines-local` preset (both prefix lists, `LLVM_INSTALL_PREFIX`, both
options) with the preset `dev-engines`
([docs/developer-guide.md](docs/developer-guide.md#the-antimony-provider)).

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
./build/dev/bin/praenora providers list --provider-path build/dev/test-providers/test-simulation
ctest --preset dev-roadrunner                  # with the roadrunner provider (local presets)
ctest --preset dev-roadrunner-tsan -L contract # its concurrency checks under ThreadSanitizer
ctest --preset dev-antimony                    # with the antimony provider (local presets)
ctest --preset dev-antimony-tsan -L 'contract|integration|unit'
ctest --preset dev-engines                     # both providers, in one process too
```

The Python line needs the interpreter the build found (`Python3_EXECUTABLE`): the extension is
built for one Python version. An installation holds the `praenora` tool, the runtime library,
the headers, the CMake package (`lib/cmake/Praenora`: `find_package(Praenora COMPONENTS Runtime
ModuleSDK)`; `ModuleSDK` alone needs no other package), the schemas and catalog
(`share/praenora`) and, when built, the roadrunner provider (`lib/praenora/providers/roadrunner`)
and the antimony provider (`lib/praenora/providers/antimony`); it can be moved. The example modules
are installed only with `PRAENORA_INSTALL_EXAMPLES=ON` or `--component Examples`; the fixture
providers of the tests (`build/<preset>/test-providers`) are never installed. A wheel build
(scikit-build-core) has only the Python package's install rules.

Project presets: `linux-x64-{debug,release,asan,tsan}`, `macos-arm64-{debug,release,asan,tsan}` and
`windows-x64-{debug,release}`, each with configure, build and test presets. Labels: `unit`,
`protocol`, `contract`, `negative`, `lifecycle`, `integration`, `cli`, `python`, `install`, `perf`,
`docs`. The `contract` label also holds the tests of provider libraries as files,
`providers.<provider>.exports` (the library exports only the provider entry point) and
`providers.<provider>.processes` (two fresh processes print the same bytes), for `test-simulation`
and, when it is built, `roadrunner`, with `praenora_simulation_print`, and for `test-conversion`
and, when it is built, `antimony`, with `praenora_conversion_print`. It also holds
`providers.run_twice.shows_first_difference`, which checks how the driver of the `processes` tests
shows two outputs that differ, and a run that fails. The `cli` label
holds the CLI driver tests, named `cli.<command>.<case>`; `cli.driver.passes_empty_arguments`, which
checks that each argument of a command reaches its program, an empty one included, and
`cli.driver.shows_failing_command`, which checks how that driver shows a failing command; and the
`CliTable.*` and `Frontend.*` unit tests of the tool's helpers and of what it shares with the Python
extension (`ctest --preset dev -L cli`). The `python` label
holds one CTest test, `python.unittest`, which runs the unittest suite of `tests/python` against the
package in `build/<preset>/python`; with `PRAENORA_SANITIZERS` set it is registered but disabled,
because an instrumented extension does not load into an interpreter that is not instrumented. So a
label-only run of a sanitizer tree (`ctest --preset dev-asan -L python`, or `dev-tsan`) runs no test
and exits 8, since the presets set `noTestsAction: error`; a full run of those trees is unaffected.
The `install` label holds `install.consumer`: its driver, `tests/install/run_install_test.cmake`,
installs the build into `build/<preset>/tests/install/work`, builds the consumer projects next to it
and lists every step it checks; with the roadrunner or the antimony provider it also checks each
installed provider, which the installed tool lists as usable before and after the installation is
moved. Builds without install rules (`PRAENORA_INSTALL=OFF`) do not have it. The `perf` label holds
`perf.validation_strict` and `perf.validation_off`, which run `praenora_perf` for 2000 ticks and
print its JSON report; they fail only when the probe fails or prints no report, never on timings.
Both labels also pass in the sanitizer trees, whose flags reach the consumer builds. The `docs`
label holds `docs.consistency`, the CMake script `tests/docs/check_docs.cmake`: every `ErrorCode`
name is in `docs/specs/error-taxonomy.md`, every `spec/schemas/*.json` name in
`docs/specs/protocol.md`, every ADR in `docs/adr/README.md`, every relative Markdown link of
`README.md`, `AGENTS.md` and `docs/` resolves, and the open-questions register has its 30 rows.
Build options are in `cmake/PraenoraOptions.cmake`, among them `PRAENORA_WITH_ROADRUNNER` (OFF),
which builds the roadrunner provider; with it, the cache variable `LLVM_INSTALL_PREFIX`
(`cmake/PraenoraRoadrunner.cmake`) names the LLVM that libRoadRunner was built with;
`PRAENORA_WITH_ANTIMONY` (OFF), which builds the antimony provider
(`cmake/PraenoraAntimony.cmake`); `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR` (OFF), which builds
json-schema-validator from its pinned sources, for the sanitizer builds
(`cmake/PraenoraDependencies.cmake`); `PRAENORA_ASYNCAPI_SCHEMA` (empty), the absolute path of a
local copy of the official AsyncAPI 3.0.0 JSON Schema, never downloaded, against which three
`GraphExport.*` tests of the `protocol` label then check `spec/asyncapi/`; and
`PRAENORA_ASYNCAPI_PARSER` (empty), the absolute path of a directory into which the official
AsyncAPI parser (`@asyncapi/parser` 3.6.6, from npm) is installed, never downloaded, with which
Node.js then runs two `asyncapi.parser.*` tests of the `protocol` label on `spec/asyncapi/`;
[docs/testing.md](docs/testing.md) has the details.

## Repository map

```
CMakeLists.txt CMakePresets.json CMakeUserPresets.json.example AGENTS.md README.md
pyproject.toml                Python distribution praenora-runtime (scikit-build-core)
cmake/                        options, versions, compiler checks, warnings, sanitizers,
                              dependencies, praenora_add_module(), praenora_embed_resources(),
                              install rules and package configuration (PraenoraInstall.cmake,
                              PraenoraConfig.cmake.in); praenora_add_provider()
                              (PraenoraProvider.cmake, not installed); the roadrunner and
                              antimony providers' dependencies (PraenoraRoadrunner.cmake,
                              PraenoraAntimony.cmake and its probe praenora_antimony_probe.cpp,
                              and what both share, PraenoraEngineDependencies.cmake)
include/praenora/runtime/     public C++ API (runtime.hpp, workflow.hpp, version.hpp, error.hpp,
                              status.hpp, options.hpp; export.h is generated)
include/praenora/sdk/         SDK: abi.h, status.h, simulation.h (the simulation service),
                              model_conversion.h (the model conversion service), module.hpp,
                              testing.hpp (version.h generated)
src/runtime/                  runtime internals, one directory per component (api/, catalog/,
                              compat/, config/, core/, discovery/, document/, engine/, graph/,
                              instance/, io/, loader/, observability/, platform/{posix,windows}/,
                              provider/ (provider manifests, index and loader; the internal
                              provider interface provider_abi.h))
src/providers/                service providers built with the runtime: roadrunner/
                              (libRoadRunner; only with PRAENORA_WITH_ROADRUNNER=ON), antimony/
                              (libAntimony; only with PRAENORA_WITH_ANTIMONY=ON), common/ (what
                              providers share: cancellable_lock.hpp, header-only, never installed)
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
                              driver run_perf.cmake), providers/ (tests of provider libraries:
                              check_exports.cmake, run_twice.cmake, check_run_twice.cmake (its
                              check), praenora_simulation_print and praenora_conversion_print),
                              docs/ (documentation test check_docs.cmake), support/ (test
                              helpers, and run_command.cmake, which the CMake drivers share);
                              fixtures/: modules/ (fixture modules), providers/ (fixture
                              providers, praenora_add_fixture_provider(); test-simulation and
                              test-conversion offer the public services), models/ (sbml/ and
                              test-decay/ models of the simulation service, test-decay-text/
                              sources of the model conversion service, antimony/ Antimony models
                              and the antimony provider's expected outputs), documents/,
                              catalog/, catalog-bad/, catalog-shared-id/, manifests/ (bad module
                              and provider manifests, search-path layouts), configs/ (good, bad
                              and CLI configurations), data/ (target data), golden/ (golden
                              outputs)
tools/                        developer scripts (generate_example_data.py)
docs/                         index.md, the guides (architecture, user-guide, configuration,
                              developer-guide, module-developer-guide, testing, python,
                              platforms), specs/ (normative specifications), feasibility/
                              (feasibility documents), adr/ (decision records),
                              open-questions.md
```

## Architecture in six bullets

- The runtime compiles once into the OBJECT library `praenora_runtime_objects`, linked into the
  shared `praenora_runtime` (only the public API is exported) and into white-box test executables.
  Its two front ends, the CLI and the Python extension, use only the public API and share their
  documents, failures and names through the header-only internal `praenora_frontend`.
- Modules are independently built `MODULE` libraries, found through `*.pmm.json` manifests on
  module search paths, loaded with `dlopen`/`LoadLibraryExW` and called only through the ABI v0
  function table returned by `praenora_module_get_api`.
- Each module instance has its own worker thread and calls into it are serialized; calls into a
  library that allows no concurrent calls (`features.concurrency: "module"`) take turns across the
  whole process, at one gate per loaded library. The host enforces timeouts; cancellation is
  cooperative.
- Heavy engines come from service providers: separately built `MODULE` libraries, found through
  `*.provider.json` manifests on provider search paths and loaded on demand, when a run,
  `validate --load` or the contract checks need one of their services; each is started once and
  kept loaded, so a process holds one engine per provider. Modules declare the services they
  require or can use in their manifests (PMM 0.2), reach them only through the host API's
  `get_service` and never link an engine; each service a run uses enters its fingerprint. The
  repository builds two, each only with its build option: `roadrunner` (the simulation service,
  libRoadRunner) and `antimony` (the model conversion service, libAntimony).
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
- **Versions are not bumped for now.** Nothing outside this repository uses the runtime, the SDK,
  the ABI, the document formats or the catalog yet, so compatibility with earlier versions is not a
  concern. Every version stays as it is: the runtime package, the SDK, PMP, the module ABI, the
  provider interface, the simulation service, the model conversion service, each document format,
  each catalog entry and the modules of this repository. Make each change within the current
  version: edit the schema file, header, catalog entry, documentation, tests and fixtures it
  affects. Never raise a version number, and never add a schema file or a catalog entry file for a
  new version. This holds until the maintainers decide to bump versions again; until then, the bump
  rules of
  [docs/specs/compatibility-policy.md](docs/specs/compatibility-policy.md#changing-a-version) and
  the specifications' rules that a change is a new version describe what will apply then, not what
  to do now.
- An ABI, protocol or document-format change adds an ADR in `docs/adr/`. Once versions are bumped
  again, it also bumps its version (ABI: `include/praenora/sdk/abi.h`; SDK, PMP and document
  formats: `cmake/PraenoraVersions.cmake`; runtime package: `project(VERSION)` in
  `CMakeLists.txt`).
- Resolving an open question updates its row in `docs/open-questions.md` and adds or supersedes an
  ADR. Calls made while building go into the relevant ADR as "Builder notes" (Proposed).
- Golden files, and the committed AsyncAPI documents in `spec/asyncapi/`, are refreshed only with
  `PRAENORA_UPDATE_GOLDEN=1`, never implicitly; regenerate the AsyncAPI documents whenever a
  workflow definition, a message type or the envelope schema changes, and, with a local copy of
  the official AsyncAPI 3.0.0 schema at hand, check them against it (`PRAENORA_ASYNCAPI_SCHEMA`),
  and with the official AsyncAPI parser installed, through it (`PRAENORA_ASYNCAPI_PARSER`).

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
- Normative specifications: `docs/specs/` (ABI, protocol, manifest, runtime services and
  providers, the simulation service, the model conversion service, workflow definitions,
  compatibility, error taxonomy, threading and ownership, time and timeouts, observability,
  reproducibility, the draft starting workflow), listed in [docs/index.md](docs/index.md); the
  three on services are [runtime-services.md](docs/specs/runtime-services.md),
  [simulation-service.md](docs/specs/simulation-service.md) and
  [model-conversion-service.md](docs/specs/model-conversion-service.md).
- Feasibility documents: [WebAssembly](docs/feasibility/webassembly.md) is a design, nothing of
  which is built; [libRoadRunner](docs/feasibility/libroadrunner.md) holds the upstream facts
  behind the built simulation service and its provider `roadrunner`, and the design of level C
  (future work); [libAntimony](docs/feasibility/libantimony.md) holds the upstream facts behind the
  built model conversion service and its provider `antimony`, and its future work.
- [docs/adr/README.md](docs/adr/README.md): architecture decision records and their process.
- [docs/open-questions.md](docs/open-questions.md): open-questions register.
