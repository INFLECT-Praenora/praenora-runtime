# Developer guide

This guide is for people who change `praenora-runtime`: building it, debugging it and the modules
it loads, its logs and traces, the sanitizer builds, adding modules to a local setup, the coding
conventions and the decision records. [AGENTS.md](../AGENTS.md) is the short version; the
component structure is in [architecture.md](architecture.md) and the tests in
[testing.md](testing.md).

Commands run from the repository root and were run on macOS arm64 unless they are marked "not yet
verified". Commands that write files outside the build tree write them under `${TMPDIR:-/tmp}`.

## Dependencies

The build needs CMake 3.24 or newer, Ninja (Linux and macOS) or Visual Studio 2022 (Windows), a
C++17 compiler (GCC 9, Clang 10, AppleClang 13 or MSVC 19.29 at least) and these packages, found
with `find_package` in CONFIG mode:

| Package | Minimum | Used by |
|---|---|---|
| nlohmann/json | 3.11 | the runtime, the CLI, the examples |
| json-schema-validator (pboettch) | 2.3 | the runtime |
| yaml-cpp | 0.8 (0.7 through a target shim) | the runtime |
| spdlog (with fmt) | 1.10 | the runtime |
| GoogleTest | 1.11 | the tests |
| SWIG | 4.0 | the Python bindings |
| Python, with its headers | 3.10 | the Python bindings |
| libRoadRunner (C++, `roadrunner-static`) and the LLVM it was built with | 2.11 | the roadrunner provider, with `PRAENORA_WITH_ROADRUNNER` only ([platforms.md](platforms.md#the-roadrunner-provider)) |
| libAntimony (`antimony-static`, built from source) and the libSBML, SBMLNetwork and expat it was built against | 3.3 | the antimony provider, with `PRAENORA_WITH_ANTIMONY` only ([platforms.md](platforms.md#the-antimony-provider)) |

The third-party libraries link privately into the runtime, so neither the installed headers nor the
installed CMake package mention them. The commands that install them on each platform are in
[platforms.md](platforms.md); json-schema-validator, which Homebrew, apt and dnf do not package, is
built from source as [platforms.md](platforms.md#json-schema-validator) shows.

**Offline by default.** A default configuration uses installed packages only and never touches the
network. With `PRAENORA_FETCH_DEPENDENCIES=ON`, CMake's FetchContent still tries `find_package`
first and downloads only what is missing, at the commits pinned in
[`cmake/PraenoraDependencies.cmake`](../cmake/PraenoraDependencies.cmake)
([ADR-0017](adr/ADR-0017-dependency-management.md)):

```bash
cmake --preset dev -DPRAENORA_FETCH_DEPENDENCIES=ON
```

With every package installed, as where this was run, that configuration finds them all and downloads
nothing; an actual download is not yet verified. Configuring prints one line per dependency, saying
where it came from:

```text
-- Praenora: dependency nlohmann_json 3.12.0 (found at /opt/homebrew/share/cmake/nlohmann_json)
-- Praenora: dependency yaml-cpp 0.9.0 (found at /opt/homebrew/lib/cmake/yaml-cpp)
```

## Local presets

[`CMakePresets.json`](../CMakePresets.json) holds the project presets: `linux-x64-debug`,
`linux-x64-release`, `linux-x64-asan`, `linux-x64-tsan`, `macos-arm64-debug`, `macos-arm64-release`,
`macos-arm64-asan`, `macos-arm64-tsan`, `windows-x64-debug` and `windows-x64-release`, each with a
build and a test preset. Each preset is available only on its host system. Build trees go to
`build/<preset>`, installations to `install/<preset>`.

Where your dependencies live is local knowledge, so it goes into `CMakeUserPresets.json`, which git
ignores. Create it from the example:

```bash
cp CMakeUserPresets.json.example CMakeUserPresets.json
```

Then edit its hidden `dev-local` preset:

| Cache variable | Set it to |
|---|---|
| `CMAKE_PREFIX_PATH` | Your dependency prefixes, separated by `;` (for example Homebrew's prefix and the prefix of json-schema-validator). |
| `Python3_EXECUTABLE` | The interpreter to build the Python bindings for. |
| `PRAENORA_BUILD_PYTHON` | `ON` to build the bindings (needs SWIG and Python), `OFF` otherwise. |
| `PRAENORA_FETCH_DEPENDENCIES` | `OFF`, or `ON` to download missing dependencies. |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | macOS only, and only when needed: see below. |

The example's `dev`, `dev-release`, `dev-asan` and `dev-tsan` presets inherit `dev-local` and the
`macos-arm64-*` presets. On Linux, make each of them inherit the matching `linux-x64-*` preset
instead (in the configure, build and test presets); on Windows, `windows-x64-debug` or
`windows-x64-release`.

**macOS deployment target.** The project builds for macOS 11.0 and later. Binaries from a package
manager target the macOS release they were built for (Homebrew builds for the running release), and
linking them into a build for an older target makes the linker warn about each such library. When
your dependencies target a newer macOS, set `CMAKE_OSX_DEPLOYMENT_TARGET` in `dev-local` to that
version, for example `"15.0"`. The project default stays 11.0; the setting is local.

## Configure, build and test

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The test presets show the output of failing tests and fail when no test matches. Running subsets
of the tests is described in [testing.md](testing.md#running-the-tests).

Options are cache variables, set in a preset or on the configure line
([`cmake/PraenoraOptions.cmake`](../cmake/PraenoraOptions.cmake)):

| Option | Default | Effect |
|---|---|---|
| `PRAENORA_BUILD_TESTS` | ON at top level | The tests (GoogleTest, CTest labels). |
| `PRAENORA_BUILD_EXAMPLES` | ON at top level | The example modules (`build/<preset>/modules`). |
| `PRAENORA_BUILD_CLI` | ON | The `praenora` tool. |
| `PRAENORA_BUILD_PYTHON` | OFF | The Python bindings ([python.md](python.md)). |
| `PRAENORA_FETCH_DEPENDENCIES` | OFF | Download missing dependencies at pinned commits. |
| `PRAENORA_INSTALL` | ON at top level | The install rules (none in a Python wheel build). |
| `PRAENORA_INSTALL_EXAMPLES` | OFF | Install the example modules with every installation. |
| `PRAENORA_WARNINGS_AS_ERRORS` | OFF | Turn compiler warnings into errors. |
| `PRAENORA_SANITIZERS` | empty | `address;undefined` or `thread`, for GCC and Clang. |
| `PRAENORA_WITH_ROADRUNNER` | OFF | The roadrunner service provider ([below](#the-roadrunner-provider)). |
| `LLVM_INSTALL_PREFIX` | empty | With `PRAENORA_WITH_ROADRUNNER`: the LLVM that libRoadRunner was built with. |
| `PRAENORA_WITH_ANTIMONY` | OFF | The antimony service provider ([below](#the-antimony-provider)). |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | 11.0 | The oldest macOS the build runs on. |

`BUILD_SHARED_LIBS` is never read: the runtime is always a shared library and modules are always
`MODULE` libraries. The tree builds without warnings; keep it so:

```bash
cmake --preset dev -DPRAENORA_WARNINGS_AS_ERRORS=ON
cmake --build --preset dev
```

The build tree holds:

| Path | Content |
|---|---|
| `build/<preset>/bin/` | `praenora`, the test executables and `praenora_perf` (on Windows also the DLLs, below a configuration directory). |
| `build/<preset>/lib/` | The runtime library. |
| `build/<preset>/modules/<name>/` | The example modules: library, manifest and test vectors. |
| `build/<preset>/test-modules/<name>/` | The fixture modules of the tests. |
| `build/<preset>/test-providers/<name>/` | The fixture service providers of the tests. |
| `build/<preset>/lib/praenora/providers/roadrunner/` | The roadrunner provider, with `PRAENORA_WITH_ROADRUNNER`: where the build tree's runtime looks for the providers of its installation. |
| `build/<preset>/lib/praenora/providers/antimony/` | The antimony provider, with `PRAENORA_WITH_ANTIMONY`, beside it. |
| `build/<preset>/python/` | The importable Python package, with `PRAENORA_BUILD_PYTHON`. |
| `build/<preset>/include/` | Generated headers: `praenora/runtime/export.h`, `praenora/sdk/version.h`. |

An installation goes elsewhere with `--prefix`; the example modules come with the component
`Examples`:

```bash
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install" --component Examples
```

### The roadrunner provider

The service provider `roadrunner`
([specs/simulation-service.md](specs/simulation-service.md#the-roadrunner-provider)) is built with
`PRAENORA_WITH_ROADRUNNER=ON`, from an installed libRoadRunner 2.11 and the LLVM it was built with;
[platforms.md](platforms.md#the-roadrunner-provider) says what to install and how CMake finds it.
Keep it in presets of its own, so the `dev` presets stay as they are: in `CMakeUserPresets.json`, a
hidden `dev-roadrunner-local` preset with the prefixes, and presets that inherit it before
`dev-local`:

```json
{
  "name": "dev-roadrunner",
  "displayName": "Local development with the roadrunner provider, Debug",
  "inherits": ["macos-arm64-debug", "dev-roadrunner-local", "dev-local"]
},
{
  "name": "dev-roadrunner-tsan",
  "displayName": "Local development with the roadrunner provider, ThreadSanitizer",
  "inherits": ["macos-arm64-tsan", "dev-roadrunner-local", "dev-local"]
}
```

with build and test presets of the same names. The `CMAKE_PREFIX_PATH` of `dev-roadrunner-local`
replaces the one of `dev-local`, so it lists the other dependency prefixes too, and the macOS
deployment target must be at least 14.0. Then:

```bash
cmake --preset dev-roadrunner
cmake --build --preset dev-roadrunner
ctest --preset dev-roadrunner
./build/dev-roadrunner/bin/praenora providers list
```

The build tree's tool lists `roadrunner` without a provider path. The tests add the service checks
against the provider, its own tests (`RoadrunnerProvider.*`) and the tests of its library file
(`providers.roadrunner.*`); [testing.md](testing.md#the-roadrunner-provider) lists them. The
provider links libRoadRunner statically, so the first build links for a while longer, and its
tests load a large library in each test process. Configuring a tree without the option removes the
provider that an earlier configuration of that tree built into it, so the tree's runtime no longer
finds it.

### The antimony provider

The service provider `antimony`
([specs/model-conversion-service.md](specs/model-conversion-service.md#the-antimony-provider)) is
built with `PRAENORA_WITH_ANTIMONY=ON`, from libAntimony 3.3 built from source against
libRoadRunner's dependencies; [platforms.md](platforms.md#the-antimony-provider) says how to build
it and how CMake finds it. As for roadrunner, keep it in presets of its own: in
`CMakeUserPresets.json`, a hidden `dev-antimony-local` preset with the prefixes, presets that
inherit it before `dev-local`, and, for both providers in one build, a hidden `dev-engines-local`
preset whose `CMAKE_PREFIX_PATH` joins the two lists, with `LLVM_INSTALL_PREFIX` and both options:

```json
{
  "name": "dev-antimony",
  "displayName": "Local development with the antimony provider, Debug",
  "inherits": ["macos-arm64-debug", "dev-antimony-local", "dev-local"]
},
{
  "name": "dev-antimony-tsan",
  "displayName": "Local development with the antimony provider, ThreadSanitizer",
  "inherits": ["macos-arm64-tsan", "dev-antimony-local", "dev-local"]
},
{
  "name": "dev-engines",
  "displayName": "Local development with the roadrunner and antimony providers, Debug",
  "inherits": ["macos-arm64-debug", "dev-engines-local", "dev-local"]
}
```

with build and test presets of the same names (`CMakeUserPresets.json.example` holds them all). The
macOS deployment target must be at least 14.0 here too. Then:

```bash
cmake --preset dev-antimony
cmake --build --preset dev-antimony
ctest --preset dev-antimony
ctest --preset dev-antimony-tsan -L 'contract|integration|unit'
ctest --preset dev-engines
./build/dev-antimony/bin/praenora providers list
```

The build tree's tool lists `antimony` without a provider path. The tests add the service checks
against the provider, its own tests (`AntimonyProvider.*`), the tests of its library file
(`providers.antimony.*`) and, in `dev-engines`, an integration test that converts an Antimony
model with `antimony` and simulates it with `roadrunner` in one process. The unit tests of its
rules that read a source's text alone (`AntimonyImportCheck.*`, `AntimonyTextCheck.*`,
`SbmlTextCheck.*`) need no libAntimony and run in every build. Configuring a tree without the
option removes the provider from it, as for roadrunner.

## Debugging modules

Modules run inside the runtime's process, so a native debugger sees the runtime and every module
together. A `Debug` build (the `dev` preset) builds the example and fixture modules with debug
information. Points to know:

- **Module libraries load late.** A module's library is opened when a run starts (or by
  `validate --load`, `modules inspect --load` and `modules check`), so a breakpoint in module code
  stays pending until then. The entry point `praenora_module_get_api` is a good first breakpoint.
- **Each instance has its own thread.** Every call into a step's instance runs on a worker thread
  named `praenora:<step>`; the thread that runs the ticks waits for it on a deadline.
- **Deadlines keep running.** While a module is stopped at a breakpoint, its call's deadline
  passes: the run then fails with `STEP_TIMEOUT`, or `MODULE_UNRESPONSIVE` when the call does not
  return within the cancellation grace. Debug with a copy of the configuration whose timeouts are
  disabled:

```bash
DBG="${TMPDIR:-/tmp}/praenora-debug"
mkdir -p "$DBG/data"
cp examples/workflows/estimate-predict/data/observations.csv "$DBG/data/"
sed -e 's/timeouts: { invoke: 30s, lifecycle: 10s, cancel_grace: 2s }/timeouts: { invoke: none, lifecycle: none }/' \
    -e 's/timeouts: { invoke: 5s }/timeouts: { invoke: none }/' \
    examples/workflows/estimate-predict/workflow.yaml > "$DBG/workflow.yaml"
./build/dev/bin/praenora run "$DBG/workflow.yaml" --module-path build/dev/modules --ticks 1
```

**lldb (macOS).** Not yet verified: the environment where this guide was checked does not allow a
debugger to start a process. Start the tool under lldb, then give these commands at the `(lldb)`
prompt (line 122 of the example estimator is the first line of its `invoke`):

```sh
lldb -- ./build/dev/bin/praenora run "$DBG/workflow.yaml" --module-path build/dev/modules --ticks 1
```

```text
breakpoint set --name praenora_module_get_api
breakpoint set --file estimator.cpp --line 122
run
thread list
bt
```

**gdb (Linux).** Not yet verified. With the `dev` preset inheriting `linux-x64-debug`, start the
tool under gdb, then give these commands at the `(gdb)` prompt:

```sh
gdb --args ./build/dev/bin/praenora run "$DBG/workflow.yaml" --module-path build/dev/modules --ticks 1
```

```text
set breakpoint pending on
break praenora_module_get_api
break estimator.cpp:122
run
info threads
```

**Visual Studio (Windows).** Not yet verified. Debug `build\dev\bin\Debug\praenora.exe` with the
command arguments `run <config> --module-path build\dev\modules\Debug --ticks 1` and set
breakpoints in the module's sources; Visual Studio loads a module's symbols when its `.pdb` file
is found beside its DLL or on the symbol path. The worker threads appear under their names in the
Threads window.

### Keeping module libraries loaded

`PRAENORA_DEBUG_KEEP_LIBRARIES=1` keeps every module library loaded until the process ends: the
runtime never calls `dlclose` or `FreeLibrary` on them. A debugger can then resolve module symbols
after a run, and a leak checker reports module frames instead of unknown addresses. The macOS
AddressSanitizer test runs set it for every test that loads modules
([testing.md](testing.md#sanitizer-builds)). The
contract checks skip their `unload` check under it:

```bash
PRAENORA_DEBUG_KEEP_LIBRARIES=1 ./build/dev/bin/praenora modules check \
    build/dev/modules/example-estimator/example-estimator.pmm.json
```

The last lines say `skip  unload  PRAENORA_DEBUG_KEEP_LIBRARIES=1 keeps module libraries loaded`.

## Message tracing

A trace records every envelope that goes into or comes out of a step, in order, with its tick, step,
direction and port, and the runtime services that each call into a step's module looked up. It needs
an output directory; `--trace` turns it on for one run (`outputs.trace_messages: true` in the
configuration for every run). The example's modules use no service, so its trace holds envelopes
only; the script below prints the message records (those with a `direction`):

```bash
RUN="${TMPDIR:-/tmp}/praenora-trace"
./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --output-dir "$RUN" --overwrite --trace --ticks 2
python3 -c 'import json, sys; [print(r["tick"], r["step"], r["direction"], r["port"]) for r in map(json.loads, open(sys.argv[1])) if "direction" in r]' "$RUN/trace/messages.jsonl"
```

```text
0 estimate in observations
0 estimate out state
0 predict in state
0 predict out predictions
1 estimate in observations
1 estimate in prior
1 estimate out state
1 predict in state
1 predict out predictions
```

Tick 1 shows the delayed feedback: `estimate` receives `prior`, the predictions of tick 0. A
record also holds the result's `diagnostics` object when the module returned one. A module that
uses a runtime service adds lookup records, with the call (`phase`), the service, its major
version, whether it was provided and how many times it was looked up in that call; a simulation
module, for example, shows its lookup of `praenora.simulation@0` in `create`, before tick 0. The
format is in [specs/observability.md](specs/observability.md#message-trace). For a failing run, the
trace's last message records show what the failing step received; the lookup records after them
show which services its failing call looked up, then those of the teardown's calls.

## Logging

The runtime logs through its own spdlog loggers, never spdlog's default logger, so an application
that uses spdlog keeps its own settings. The loggers are `praenora.runtime`, `praenora.engine`,
`module.<step>` (a step's module messages and the events of its instance) and `module.check`.
Per-step, per-call and per-tick events (`step.ready`, `invocation.completed`, `tick.completed`)
are at level `debug`, and the start and end of a run at `info`:

```bash
PRAENORA_LOG_LEVEL=debug ./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --ticks 1
```

```text
2026-11-11T18:49:38.378297000Z DEBUG praenora.engine [run.fingerprint=a4d251a5be737c49fe7cc73b30aa65367f3bc8e55cb2818ebb12bbafed93f46d event=invocation.completed tick=0 step=estimate module=example-estimator@0.1.0 duration_ns=257458] invocation completed
```

The level, format and file are each resolved on their own, from the run's options, the environment,
the configuration's `observability` section or `RuntimeOptions::logging`, in the order specified in
[specs/observability.md](specs/observability.md#settings). A step's `log_level` in the
configuration sets its module logger whatever the run's level. JSON lines and a log file:

```bash
./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --ticks 1 \
    --log-format json --log-file "${TMPDIR:-/tmp}/praenora-logs/run.log"
```

Records are flushed one by one, so a log survives a crash inside a module. The full description,
with the JSON fields and every event, is in [specs/observability.md](specs/observability.md#logs).

## Sanitizer builds

The `asan` presets build with AddressSanitizer and UndefinedBehaviorSanitizer
(`PRAENORA_SANITIZERS=address;undefined`), the `tsan` presets with ThreadSanitizer (`thread`). The
flags apply to every target, modules included, and to the projects the install test builds. The
user presets `dev-asan` and `dev-tsan` wrap them:

```bash
cmake --preset dev-asan
cmake --build --preset dev-asan
ctest --preset dev-asan
cmake --preset dev-tsan
cmake --build --preset dev-tsan
ctest --preset dev-tsan
```

- GCC and Clang only; MSVC builds have no sanitizer presets.
- The thread sanitizer cannot be combined with the address or leak sanitizers.
- On macOS, AddressSanitizer test runs keep module libraries loaded
  (`PRAENORA_DEBUG_KEEP_LIBRARIES=1`): an unloaded instrumented library otherwise leaves its
  globals registered with the sanitizer, which then reports errors against a library loaded later
  at the same address.
- `python.unittest` is registered but disabled in sanitizer builds, so a run of the label alone,
  `ctest --preset dev-asan -L python`, runs no test and exits with 8 ([python.md](python.md#tests)).

## Adding a module locally

Every module is found the same way: through its manifest on a module search path
([user-guide.md](user-guide.md#finding-modules)). Where its sources live decides how it is built.

**A fixture module for the tests.** Add a directory `tests/fixtures/modules/fx-<name>/` with the
sources and a manifest template `fx-<name>.pmm.json.in`, and one line in
[`tests/fixtures/modules/CMakeLists.txt`](../tests/fixtures/modules/CMakeLists.txt):
`praenora_add_fixture_module(fx-<name> SOURCES fx-<name>/<source>)`. It builds into
`build/<preset>/test-modules/fx-<name>/`. Fixture modules implement the test capability
`test.echo@0.1.0` (or others of the test catalog, `tests/fixtures/catalog/`), so the commands that
check them need that catalog:

```bash
./build/dev/bin/praenora modules list --module-path build/dev/test-modules
./build/dev/bin/praenora modules check build/dev/test-modules/fx-echo/fx-echo.pmm.json \
    --catalog-path tests/fixtures/catalog
```

**An example module.** Example modules live in `examples/modules/<name>/` and are built with
`praenora_add_module(... OUTPUT_DIRECTORY "${PRAENORA_EXAMPLE_MODULES_DIR}/<name>")` followed by
`praenora_example_module(<target>)`, which adds the project's warnings and `-ffp-contract=off`, and
an `add_subdirectory()` in [`examples/CMakeLists.txt`](../examples/CMakeLists.txt). See
[`examples/modules/estimator/CMakeLists.txt`](../examples/modules/estimator/CMakeLists.txt).

**A module from another repository.** Build it against an installation of the SDK, as
[module-developer-guide.md](module-developer-guide.md#a-first-module) shows, and point the runtime
at its build directory with `--module-path`, `PRAENORA_MODULE_PATH` or the configuration's
`modules.search_paths`. Then `praenora modules list` shows it,
`praenora modules inspect <name>@<version> --load` loads it and
`praenora modules check <manifest>` runs its contract checks.
To use it in a workflow, bind the step whose capability it implements to it in the configuration.

## Coding conventions

The conventions are listed in [AGENTS.md](../AGENTS.md#conventions): C++17 without extensions and
C99 for the ABI headers; snake_case functions, variables and files and PascalCase types; internals
in `praenora::detail` under `src/runtime/<component>/`; `praenora::Error` with an error-taxonomy
code for failures, and a validation report that collects every problem; public headers free of
third-party types; only C types across the module ABI; no unordered iteration, wall-clock time or
unseeded randomness in anything a run records; UTF-8 and LF everywhere.

Formatting follows [`.clang-format`](../.clang-format), and every C and C++ file of the tree is
formatted with it, by Apple clang-format 17, which Xcode runs as `xcrun clang-format` (elsewhere,
the `clang-format` of an LLVM installation; another version may format a few constructs
differently). These check one file and then the whole tree; with `-i` instead of
`--dry-run --Werror`, clang-format formats the files in place:

```sh
xcrun clang-format --dry-run --Werror src/runtime/engine/engine.cpp
git ls-files '*.c' '*.h' '*.cpp' '*.hpp' | xargs xcrun clang-format --dry-run --Werror
```

The function-pointer declarations of `include/praenora/sdk/abi.h` (and of the test service in
`tests/support/test_gate.h`) are kept out of the formatting with `/* clang-format off */` and
`/* clang-format on */`, so they keep the spelling `ret (PRAENORA_CALL *name)(...)`.

The static checks are in [`.clang-tidy`](../.clang-tidy). Not yet verified (clang-tidy was not
installed where this guide was checked): `clang-tidy` reads `build/<preset>/compile_commands.json`,
which the Ninja presets write.

```sh
clang-tidy -p build/dev src/runtime/engine/engine.cpp
```

## Decision records

Architecture decisions are recorded in [docs/adr/](adr/README.md), one file per decision
(`ADR-NNNN-<title>.md`, never renumbered), each with a status: Proposed, Accepted, Superseded or
Deprecated.

- **A new decision.** Copy [`docs/adr/template.md`](adr/template.md), take the next free number,
  fill every section (Context, Decision, Alternatives considered, Consequences, References) and add
  a row to the index in [docs/adr/README.md](adr/README.md). The documentation test checks that
  every ADR is in the index.
- **Changing a decision.** Write a new ADR that supersedes the old one, and update both headers
  (`Supersedes:`, `Superseded by:`); the old text stays.
- **Builder notes.** A call made while building that does not change what gets built goes into the
  relevant ADR as a bullet under "Builder notes (Proposed)" in its Decision section.
- **Open questions.** A question that stays open has a row in
  [open-questions.md](open-questions.md). Resolving one updates its row and adds or supersedes an
  ADR.

Changes that come with obligations:

- A schema, catalog or documentation change travels with the others it affects.
- For now no version is bumped: a change goes into the current versions in place, schemas, catalog
  entries and this repository's modules included ([AGENTS.md](../AGENTS.md#change-rules)). An ABI,
  protocol or document-format change comes with an ADR. Once versions are bumped again, an ABI
  change also bumps the ABI version in `include/praenora/sdk/abi.h`, and a change of the SDK, the
  protocol or a document format its version in
  [`cmake/PraenoraVersions.cmake`](../cmake/PraenoraVersions.cmake); the rules for which part of a
  version to bump are in
  [specs/compatibility-policy.md](specs/compatibility-policy.md#changing-a-version).
- Golden files and the AsyncAPI documents in `spec/asyncapi/` change only through
  `PRAENORA_UPDATE_GOLDEN=1` ([testing.md](testing.md#golden-files)); regenerate the AsyncAPI
  documents whenever a workflow definition, a message type or the envelope schema changes.
