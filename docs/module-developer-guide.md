# Module developer guide

This guide is for teams that write Praenora modules: libraries, built in their own repositories,
that implement one or more capabilities of the protocol catalog and that the runtime loads at run
time. It covers the module SDK, the rules of the ABI, the C++ adapter, the manifest,
`praenora_add_module()`, the in-process test kit, test vectors, the contract checks, the
determinism and portability rules, and versioning. The normative rules are in
[specs/module-abi.md](specs/module-abi.md), [specs/manifest.md](specs/manifest.md) and
[specs/threading-and-ownership.md](specs/threading-and-ownership.md).

The commands were run on macOS arm64 against an installation of this repository's `dev` build.
Linux and Windows are not yet verified ([platforms.md](platforms.md)).

## What a module is

- A library of type `MODULE` (`<name>.so` on Linux and macOS, `<name>.dll` on Windows) that exports
  one C function, `praenora_module_get_api`, which returns a static table of C function pointers:
  ABI v0. Nothing else crosses between the runtime and the module: no C++ types, no exceptions, no
  allocations that the other side frees.
- A manifest, `<name>.pmm.json`, beside the library: the module's name and version, the ABI,
  protocol and SDK versions it was built for, its platform, the capabilities it implements and the
  JSON Schema of its parameters. The runtime reads manifests to find and check modules without
  running any of their code.
- Optionally, test vectors: inputs and expected outputs that the contract checks run.

A module implements capabilities; it does not choose its inputs or its place in a workflow. A
workflow definition gives each step a capability, and a workflow configuration binds the step to a
module that implements it, so modules of the same capability are interchangeable.

## The SDK

The SDK is the CMake package component `ModuleSDK` of an installation of `praenora-runtime`. It
needs only a C or C++ compiler: no runtime library and no third-party package.

| Part | Content |
|---|---|
| `praenora/sdk/abi.h` | The C ABI v0: the entry point, the function table, the host API, sinks (C99). |
| `praenora/sdk/status.h` | The status codes `PRAENORA_STATUS_*` and `praenora_status_name()` (C99). |
| `praenora/sdk/version.h` | The SDK and protocol versions the SDK was built with. |
| `praenora/sdk/module.hpp` | The optional C++ adapter (header-only, C++17 standard library only). |
| `praenora/sdk/testing.hpp` | The in-process test kit (header-only). |
| `PraenoraModule.cmake` | `praenora_add_module()`, loaded by `find_package`. |

```cmake
find_package(Praenora 0.1 REQUIRED COMPONENTS ModuleSDK)
```

gives the target `Praenora::ModuleSDK` and `praenora_add_module()`. The package accepts every
0.1.z installation (`SameMinorVersion` while the version is 0.x).

## A first module

This example implements the capability `example.estimate@0.1.0`, which the example workflow's
`estimate` step needs, in a simpler way than the example module: each tick, every series and
variable is estimated by its latest usable observation. It uses nlohmann/json to read and write
JSON, as the example modules do; the SDK itself parses no JSON, so any library will do.

First install `praenora-runtime` somewhere, from its build tree, with the example modules (the
commands of this section run from a checkout of this repository):

```bash
PREFIX="${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "$PREFIX"
cmake --install build/dev --prefix "$PREFIX" --component Examples
```

Then create the module's directory, for example `${TMPDIR:-/tmp}/last-value-estimator`, with these
files.

`CMakeLists.txt`: the library, its manifest and its test vectors, and an in-process test:

```cmake
cmake_minimum_required(VERSION 3.24)
project(last_value_estimator VERSION 0.1.0 LANGUAGES CXX)

find_package(Praenora 0.1 REQUIRED COMPONENTS ModuleSDK)
find_package(nlohmann_json 3.11 REQUIRED)

praenora_add_module(last_value_estimator
    NAME last-value-estimator
    VERSION 0.1.0
    CAPABILITIES example.estimate@0.1.0
    SOURCES last_value.cpp
    MANIFEST_TEMPLATE last-value-estimator.pmm.json.in
    TEST_VECTORS vectors.json
    LINK_LIBRARIES nlohmann_json::nlohmann_json)

# In-process tests with the SDK's test kit (praenora/sdk/testing.hpp): no library is loaded.
enable_testing()
find_package(GTest 1.11 REQUIRED)
include(GoogleTest)
add_executable(last_value_test last_value_test.cpp)
target_link_libraries(last_value_test PRIVATE
    Praenora::ModuleSDK nlohmann_json::nlohmann_json GTest::gtest_main)
gtest_discover_tests(last_value_test)
```

`last_value.hpp`: the module class, in a header so that the test can include it too:

```cpp
// last-value-estimator: the capability example.estimate@0.1.0. Each tick, every series and
// variable is estimated by its latest usable observation in the tick's batch.
#ifndef LAST_VALUE_HPP
#define LAST_VALUE_HPP

#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace last_value {

namespace sdk = praenora::sdk;
using nlohmann::json;

class LastValue final : public sdk::Module {
public:
    explicit LastValue(const sdk::CreateContext& context) : host_(context.host) {}

    std::string invoke(std::string_view invocation_json) override {
        const json invocation = json::parse(invocation_json, nullptr, false);
        if (!invocation.is_object()) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "the invocation is not JSON");
        }
        try {
            const json& batch = invocation.at("inputs").at("observations").at("data");
            // Rows arrive sorted by time, so the last usable row of a key is its latest. A
            // std::map keeps the estimates in a deterministic order.
            std::map<std::pair<std::string, std::string>, json> latest;
            for (const json& row : batch.at("observations")) {
                if (row.value("missing", false) || row.value("censoring", "none") != "none" ||
                    !row.at("value").is_number()) {
                    continue;
                }
                latest[{row.at("series_id").get<std::string>(),
                        row.at("variable_id").get<std::string>()}] = row;
            }
            json estimates = json::array();
            for (const auto& [key, row] : latest) {
                estimates.push_back({{"series_id", key.first},
                                     {"variable_id", key.second},
                                     {"time", row.at("time")},
                                     {"value", row.at("value")},
                                     {"basis", "observation"}});
            }
            if (host_.log_enabled(sdk::LogLevel::debug)) {
                host_.log(sdk::LogLevel::debug, std::to_string(estimates.size()) + " estimates");
            }
            json state = {{"time_unit", batch.at("time_unit")}, {"estimates", estimates}};
            return json{{"outputs", {{"state", {{"data", std::move(state)}}}}}}.dump();
        } catch (const json::exception& error) {
            throw sdk::Error(PRAENORA_STATUS_DATA_INVALID, error.what());
        }
    }

private:
    sdk::Host host_;
};

} // namespace last_value

#endif // LAST_VALUE_HPP
```

`last_value.cpp`: the library's one source file, which defines the exported entry point:

```cpp
// The module's library: its single exported entry point, praenora_module_get_api.
#include "last_value.hpp"

PRAENORA_SDK_DEFINE_MODULE(last_value::LastValue)
```

`last-value-estimator.pmm.json.in`: the manifest template; `praenora_add_module()` fills in the
`@...@` variables:

```json
{
  "kind": "module-manifest",
  "format_version": "0.1.0",
  "module": {
    "name": "@PRAENORA_MODULE_NAME@",
    "version": "@PRAENORA_MODULE_VERSION@",
    "summary": "Estimates each series by its latest usable observation."
  },
  "abi": { "major": @PRAENORA_ABI_MAJOR@, "minor": @PRAENORA_ABI_MINOR@ },
  "pmp_version": "@PRAENORA_PMP_VERSION@",
  "sdk_version": "@PRAENORA_SDK_VERSION@",
  "library": { "path": "@PRAENORA_MODULE_LIBRARY_FILE@" },
  "platform": { "os": "@PRAENORA_MODULE_OS@", "arch": "@PRAENORA_MODULE_ARCH@" },
  "capabilities": @PRAENORA_MODULE_CAPABILITIES_JSON@,
  "parameters_schema": {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "additionalProperties": false
  },
  "features": { "concurrency": "instance" },
  "tests": { "vectors": "@PRAENORA_MODULE_TEST_VECTORS@" }
}
```

`vectors.json`: one case of one tick, with the expected output:

```json
{
  "kind": "module-test-vectors",
  "format_version": "0.1.0",
  "cases": [
    {
      "name": "latest-observation-wins",
      "capability": { "id": "example.estimate", "version": "0.1.0" },
      "time": { "unit": "d", "start": 0.0, "period": 1.0 },
      "ticks": [
        {
          "inputs": {
            "observations": {
              "window": { "start": 0.0, "end": 1.0 },
              "time_unit": "d",
              "observations": [
                { "series_id": "S01", "variable_id": "population", "time": 0.25, "value": 10.0 },
                { "series_id": "S02", "variable_id": "population", "time": 0.5, "value": null,
                  "missing": true },
                { "series_id": "S01", "variable_id": "population", "time": 0.75, "value": 12.5 }
              ]
            }
          },
          "expect": {
            "outputs": {
              "state": {
                "time_unit": "d",
                "estimates": [
                  { "series_id": "S01", "variable_id": "population", "time": 0.75,
                    "value": 12.5, "basis": "observation" }
                ]
              }
            }
          }
        }
      ]
    }
  ]
}
```

`last_value_test.cpp`: the module driven in process by the test kit:

```cpp
// The module driven in process by the SDK's test kit: FakeHost stands in for the runtime's host
// API and DirectModule calls the module's function table directly.
#define PRAENORA_MODULE_NAME "last-value-estimator"
#define PRAENORA_MODULE_VERSION "0.1.0"
#define PRAENORA_MODULE_CAPABILITIES "[{\"id\":\"example.estimate\",\"version\":\"0.1.0\"}]"

#include "last_value.hpp"

#include <praenora/sdk/testing.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
PRAENORA_SDK_DEFINE_MODULE_GETTER(last_value::LastValue, last_value_get_api)
} // namespace

using praenora::sdk::testing::DirectModule;
using praenora::sdk::testing::FakeHost;

TEST(LastValue, KeepsTheLatestUsableObservation) {
    FakeHost host;
    DirectModule module(&last_value_get_api);
    ASSERT_EQ(module.create(host, R"({"parameters": {}})").status, PRAENORA_STATUS_OK);
    const auto result = module.invoke(R"({"inputs": {"observations": {"data": {
        "time_unit": "d", "observations": [
          {"series_id": "S01", "variable_id": "x", "time": 0.25, "value": 1.0},
          {"series_id": "S01", "variable_id": "x", "time": 0.75, "value": 2.0}]}}}})");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
    const auto state = nlohmann::json::parse(result.output)["outputs"]["state"]["data"];
    EXPECT_EQ(state["estimates"].size(), 1u);
    EXPECT_EQ(state["estimates"][0]["value"], 2.0);
}

TEST(LastValue, RefusesAMalformedInvocation) {
    FakeHost host;
    DirectModule module(&last_value_get_api);
    ASSERT_EQ(module.create(host, R"({"parameters": {}})").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.invoke("{").status, PRAENORA_STATUS_INVALID_ARGUMENT);
}
```

Build it against the installation, run its tests, and check it with the installed tool:

```bash
MODULE="${TMPDIR:-/tmp}/last-value-estimator"
cmake -S "$MODULE" -B "$MODULE/build" -G Ninja -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$MODULE/build"
ctest --test-dir "$MODULE/build"
"$PREFIX/bin/praenora" modules list --module-path "$MODULE/build"
"$PREFIX/bin/praenora" modules check "$MODULE/build/last-value-estimator/last-value-estimator.pmm.json"
```

`CMAKE_PREFIX_PATH` also needs the prefixes of nlohmann/json and GoogleTest when CMake does not find
them by itself (it found Homebrew's here). The build writes
`build/last-value-estimator/last-value-estimator.so`, its manifest and a copy of the vectors. The
check ends with `17 checks: 16 passed, 0 failed, 1 skipped` (the module has no `self_test`).

The module can now replace the example estimator. A copy of the example configuration binds the
`estimate` step to it; the installed tool finds the example predictor in its installation's module
directory and the new module in its build directory:

```bash
WF="${TMPDIR:-/tmp}/last-value-workflow"
mkdir -p "$WF/data"
cp examples/workflows/estimate-predict/data/observations.csv "$WF/data/"
sed -e 's/name: example-estimator/name: last-value-estimator/' \
    -e 's/parameters: { gain: 0.6 }/parameters: {}/' \
    examples/workflows/estimate-predict/workflow.yaml > "$WF/workflow.yaml"
"$PREFIX/bin/praenora" run "$WF/workflow.yaml" --module-path "$MODULE/build" --ticks 3
```

## ABI rules

The ABI is specified in [specs/module-abi.md](specs/module-abi.md); the C++ adapter follows these
rules for you, and a module written in C follows them itself.

**Entry point and table.** The library exports `praenora_module_get_api(uint32_t abi_major)`. For
major 0 it returns a pointer to a static `praenora_module_api_v0` whose header says ABI 0.0, its
`struct_size` and `reserved` 0, with the required functions `describe`, `create`, `initialize`,
`invoke`, `finalize` and `destroy` set; `diagnostics` and `self_test` are optional (NULL). For any
other major it returns NULL. Both the entry point and `describe` must be thread-safe and free of
side effects.

**Lifecycle.** For each step, the runtime calls `create` with the create document (step,
capability, module and the validated parameters, defaults filled in), then `initialize` with the
initialize document (run fingerprint, the step's seed, the time base), `invoke` once per tick with
the invocation document (tick, logical time, the inputs as envelopes; absent optional inputs left
out), `finalize` once at the end if `initialize` succeeded, and `destroy` exactly once for every
instance `create` returned. A result document is `{"outputs": {"<port>": {"data": ...}},
"diagnostics": {...}}`: the runtime builds the envelopes, so a module returns only the data of each
output port. Every required output must be present, and only declared ports may appear
([specs/protocol.md](specs/protocol.md#module-abi-documents)).

**Ownership.** Inputs (`praenora_bytes`, the host API pointer) belong to the runtime: copy what you
keep beyond the call (the host API stays valid until `destroy` returns). Outputs go through sinks:
call `sink->write` as often as needed; the runtime copies the bytes before `write` returns, so your
buffers stay yours. A sink is valid only during its call. An instance is allocated by `create` and
freed by `destroy` with your own allocator. Nothing else crosses: no `malloc`/`free` pairs across
the boundary, no `FILE*`, no C++ objects, no exceptions, no `longjmp`. A result may be at most
256 MiB; beyond that the sink refuses the write with `PRAENORA_STATUS_RESOURCE_EXHAUSTED`, which the
module should return.

**Threading.** The runtime makes every call for one instance, from `create` to `destroy`, on one
thread of its own (`praenora:<step>`), never two at once. When the manifest's
`features.concurrency` is `"module"` (the default), different instances of a library are never
called concurrently within one run or one contract-check session; `"instance"` declares instances
independent. That promise does not reach across runs. A host process must not run two workflows
that use your library at the same time, nor use the library again after one of its calls became
unresponsive
([specs/threading-and-ownership.md](specs/threading-and-ownership.md#concurrency-across-instances)),
and the `praenora` tool runs one workflow per process; but the runtime does not enforce it, so a
host that breaks the rule can call two of your instances at once. The host callbacks (`log`,
`log_enabled`, `cancel_requested`, `get_service`) are thread-safe and may be called from any thread
of the module while the instance exists. A module that starts threads must join them before
`destroy` returns.

**Deadlines and cancellation.** Each call has a deadline (`invoke` 300 s and other calls 60 s by
default, set by the configuration). When it passes, `cancel_requested()` returns nonzero for that
call: a module with long loops should poll it and return `PRAENORA_STATUS_CANCELLED` soon. A call
that still has not returned after the cancellation grace makes the instance unresponsive: the
runtime abandons it, never destroys it and keeps its library loaded, and an unresponsive `create`,
`initialize` or `invoke` fails the run.

**Errors.** A failing call returns a status: a general code from 1 to 99 (`PRAENORA_STATUS_*`, 1 to
12 defined) or a code of the module's own from `PRAENORA_STATUS_MODULE_BASE` (10000) up, never one
from 100 to 9999. It should write an error document to its error sink:
`{"code": <int>, "message": "<text>", "details": {...}}`. The runtime reports the failure as
`STEP_FAILED` (or `STEP_TIMEOUT`, `OUTPUT_INVALID`) with the module's document as its `cause`.

| Status | Use it for |
|---|---|
| `PRAENORA_STATUS_INVALID_ARGUMENT` (2) | A document the module cannot read (the contract checks send `{` and expect an error). |
| `PRAENORA_STATUS_UNSUPPORTED` (5) | A capability, parameter or host feature the module does not support. |
| `PRAENORA_STATUS_RESOURCE_EXHAUSTED` (6) | Out of memory, or a sink that refused a write. |
| `PRAENORA_STATUS_CANCELLED` (7) | A call that stopped because `cancel_requested()` said so. |
| `PRAENORA_STATUS_FAILED` (11) | A computation that failed. |
| `PRAENORA_STATUS_DATA_INVALID` (12) | Input data with the wrong form or values. |

**Host API.** `log(level, message)` writes to the step's logger (`module.<step>`), which adds the
step, module, tick and run fingerprint; `log_enabled(level)` saves formatting messages nobody reads.
`get_service(name, major)` returns a runtime service's function table, or NULL: this version
registers no production service. Read `host->struct_size` before using a callback added in a later
ABI minor version.

## The C++ adapter

`praenora/sdk/module.hpp` implements the table for a C++ class. Derive from
`praenora::sdk::Module`, give the class a constructor taking `const praenora::sdk::CreateContext&`,
and expand `PRAENORA_SDK_DEFINE_MODULE(ClassName)` once in the library, at namespace scope.

| Name | What it is |
|---|---|
| `Module` | The base class: `invoke(std::string_view invocation_json)` returns the result document (required); `initialize(const InitializeContext&)`, `finalize()` and `diagnostics()` have defaults. The constructor is `create`, the destructor `destroy`. |
| `CreateContext` | `host` (valid until the instance is destroyed) and `create_json` (valid during the constructor). |
| `InitializeContext` | `initialize_json`, which holds the run fingerprint, the step's seed and the time base. |
| `Host` | `log()`, `log_enabled()`, `cancel_requested()` and `service()`; each checks the host's `struct_size` first. |
| `LogLevel` | `trace`, `debug`, `info`, `warn`, `error`, `critical`. |
| `Error` | `Error(code, message, details_json = "{}")`: throw it to fail a call with a status of your choice. |
| `PRAENORA_SDK_DEFINE_MODULE(ClassName)` | The exported entry point and the static table. |
| `PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, getter)` | A getter for the table without an exported entry point, for in-process tests. |

- Every exception is caught at the boundary: `sdk::Error` becomes its code (or `INTERNAL` for a code
  outside 1 to 99 and below 10000), `std::bad_alloc` becomes `RESOURCE_EXHAUSTED`, and anything else
  `INTERNAL` with `what()` as the message. The adapter writes the error document.
- `describe()` reports `PRAENORA_MODULE_NAME`, `PRAENORA_MODULE_VERSION` and
  `PRAENORA_MODULE_CAPABILITIES`, which `praenora_add_module()` defines from its arguments, so the
  library and its manifest cannot disagree.
- The adapter parses no JSON: parse the create and initialize documents yourself for the parameters
  and the seed ([specs/protocol.md](specs/protocol.md#create-document)).
- Outputs are written to the sinks in pieces of 4096 bytes; a refused write ends the call with the
  sink's status.

A module may also implement the table in C. The fixture module `fx-minimal-c` is a complete C99
module: its table and entry point are in
[`fx-minimal-c/fx_minimal_c.c`](../tests/fixtures/modules/fx-minimal-c/fx_minimal_c.c) and its
functions in [`common/c_fixture.c`](../tests/fixtures/modules/common/c_fixture.c), both under
`tests/fixtures/modules/`. `PRAENORA_MODULE_IMPLEMENTATION`, which `praenora_add_module()` defines,
makes `abi.h` declare the entry point.

## The manifest

`praenora_add_module()` generates the manifest beside the library, from a template or, without
one, with the required fields and a parameter schema that accepts any object. A template is a JSON
file in which `@...@` variables stand for what the build knows, such as `@PRAENORA_MODULE_NAME@`;
the variables are listed in [specs/manifest.md](specs/manifest.md#generating-manifests).

Besides the variables, a template sets what only the module knows:

- `module.summary`, `authors`, `repository` and extension fields `x-...`.
- `parameters_schema`: a JSON Schema draft-07 for the step's `parameters`. The runtime validates the
  configuration's parameters against it (`PARAMETERS_INVALID`) and fills in its `default`s before
  `create`. Give every parameter a default where one makes sense: the contract checks use the
  defaults when no test vectors are given.
- `features`: `concurrency` (`"module"` or `"instance"`), `diagnostics` (whether a failure's details
  include the instance's `diagnostics()`), `self_test` (whether the contract checks require a
  `self_test` function) and `cancellation` (informational).
- `tests.vectors`: the test vectors' file name, relative to the manifest.
- `library.sha256`: an optional pin of the library's digest, checked before loading; and
  `library.dependency_paths`: directories, relative to the manifest, where the module's own DLLs
  live (honored on Windows only; on Linux and macOS use an RPATH of `$ORIGIN` or `@loader_path`).

The template must not contain `$<` otherwise: the result is evaluated for generator expressions.
Every field and rule is in [specs/manifest.md](specs/manifest.md), and
`praenora validate --document <manifest>` checks a manifest against its schema.

## `praenora_add_module()`

```cmake
praenora_add_module(<target>
    NAME <name> VERSION <semver> CAPABILITIES <id@version>... SOURCES <source>...
    [MANIFEST_TEMPLATE <file>] [TEST_VECTORS <file>] [OUTPUT_DIRECTORY <dir>]
    [INSTALL_DESTINATION <dir> [INSTALL_COMPONENT <component>] [INSTALL_EXCLUDE_FROM_ALL]]
    [LINK_LIBRARIES <library>...])
```

| Argument | Meaning |
|---|---|
| `NAME` | The module name: lower-case words of letters, digits and `-`, separated by dots. It is also the library's file name, without a `lib` prefix. |
| `VERSION` | The module's semantic version. |
| `CAPABILITIES` | Each implemented capability as `<id>@<version>`, as the catalog names it. |
| `SOURCES` | The sources. |
| `MANIFEST_TEMPLATE` | The manifest template, relative to the current source directory. |
| `TEST_VECTORS` | A test vectors file, copied beside the manifest at build time. |
| `OUTPUT_DIRECTORY` | Where the library and manifest go (default `<current binary dir>/<name>`); multi-configuration generators get a directory per configuration. |
| `INSTALL_DESTINATION` | Install the library, the manifest and the vectors together into this directory; `<libdir>/praenora/modules/<name>` inside an installation's prefix is where the runtime looks by itself. |
| `INSTALL_COMPONENT`, `INSTALL_EXCLUDE_FROM_ALL` | Put the installed files in a component, and leave them out of a full installation. |
| `LINK_LIBRARIES` | More libraries to link privately, such as a JSON library. |

The target is always an `add_library(MODULE)` target, whatever `BUILD_SHARED_LIBS` says, compiled
with hidden symbol visibility so that only the entry point is exported, and linked to
`Praenora::ModuleSDK`. The target properties `PRAENORA_MODULE_DIR`, `PRAENORA_MODULE_MANIFEST` and
`PRAENORA_MODULE_TEST_VECTORS_PATH` give the output directory, the manifest and the copied vectors
for rules of your own. The helper's reference is the comment at the top of
[`cmake/PraenoraModule.cmake`](../cmake/PraenoraModule.cmake).

## The test kit

`praenora/sdk/testing.hpp` drives a module in process, with no library loaded, from any test
framework:

- `FakeHost` implements the host API for tests: it records every log message (`logs()`), raises and
  clears the cancel flag on demand (`request_cancel()`, `reset_cancel()`), serves service tables
  (`register_service()`), can report a smaller `struct_size` to act as an older host, and can refuse
  sink writes (`refuse_sink_writes(true)`).
- `DirectModule` takes the module's getter (or any `praenora_module_get_api_fn`, also one found
  with `dlsym` in a built library), checks the table as the runtime does, and calls `describe()`,
  `create(host, json)`, `initialize(json)`, `invoke(json)`, `finalize()`, `diagnostics()` and
  `destroy()`. Each returns a `CallResult`: the `status` and what the module wrote to its output
  sink (`output`) and its error sink (`error`).
- `PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, getter)` defines the getter in the test; the test
  defines `PRAENORA_MODULE_NAME`, `PRAENORA_MODULE_VERSION` and `PRAENORA_MODULE_CAPABILITIES`
  itself, as `last_value_test.cpp` above does.

## Test vectors

A test vectors document (`kind: module-test-vectors`, schema
[`module-test-vectors.0.1.schema.json`](../spec/schemas/module-test-vectors.0.1.schema.json)) holds
cases. Each case names a `capability` the module implements and gives `parameters` (validated and
completed with the defaults, as in a run), a `seed`, a `time` base and a list of `ticks`. Each tick
gives the `data` of each input port, from which the checks build the envelopes, and optionally an
`expect`ed `outputs` object with a `tolerance` (`relative`, `absolute`, both 0 by default). Numbers
match when `|a - b| <= max(absolute, relative * max(|a|, |b|))`; everything else must be equal, and
the expected and actual outputs must have the same ports.

Good vectors come from real runs: the example modules' vectors are the envelopes they received and
produced in the first two ticks of the example workflow, taken from its message trace
([`examples/modules/estimator/vectors.json`](../examples/modules/estimator/vectors.json)). Ship the
vectors with the module (`TEST_VECTORS`, `tests.vectors`), so anyone can check it.

## Contract checks

`praenora modules check <manifest> [--vectors F]` loads the module and runs checks with stable ids,
each `pass`, `fail` or `skip` with a message. It exits with 0 when no check failed and with 1
otherwise. With `--json` it prints `{"ok": true, "result": <the module-check-report document>}`, or,
when a check failed, `{"ok": false, "error": ..., "diagnostics": []}`, an error `FAILED` that holds
the report in its `details`. `--catalog-path` adds the catalog that defines the module's
capabilities when they are not in the built-in catalog.

| Check | Passes when |
|---|---|
| `manifest.schema` | The manifest satisfies the `module-manifest` schema. |
| `manifest.semantics` | Its paths are relative, its capabilities unique, its parameter schema compiles, it fits this runtime (platform, ABI major, PMP version) and the catalog defines its capabilities. |
| `library.digest` | The library exists and matches `library.sha256` when the manifest pins one. |
| `library.load` | The library loads. |
| `library.entry_point` | It exports `praenora_module_get_api`. |
| `api.header` | The table's header is valid for ABI 0. |
| `api.required_functions` | Every required function is set. |
| `describe.schema` | `describe()` writes a valid `pmp-describe` document. |
| `describe.identity` | It agrees with the manifest: name, version, ABI major, capabilities. |
| `lifecycle.create_destroy` | An instance can be created and destroyed. |
| `lifecycle.full` | An instance can be created, initialized, finalized and destroyed. |
| `invoke.result_schema` | Every result of the vectors is valid: declared ports only, every required one, each datum valid for its message type. |
| `invoke.expected_outputs` | The outputs match the vectors' expectations within their tolerance. |
| `invoke.determinism` | A second run of the vectors on a new instance gives byte-identical results. |
| `invoke.malformed_input` | `invoke("{")` returns an error status, in time. |
| `self_test` | The module's `self_test` reports `passed` (skipped when it has none and the manifest does not declare it). |
| `unload` | Every instance was destroyed and the library released. |

A check runs only when the one it depends on passed; otherwise it is skipped with the reason.
Without test vectors the lifecycle checks use the first capability and the parameter schema's
defaults, and the checks that need inputs are skipped, so a module should ship vectors. The checks
and their order are specified in [ADR-0024](adr/ADR-0024-contract-checks-and-test-vectors.md).

**The acceptance gate.** Before this project adopts a module version, the module's own test suite
passes, `praenora modules check` passes on its manifest, and its test vectors are part of the module
([testing.md](testing.md#the-module-acceptance-gate)).

## Determinism and portability

The runtime makes runs reproducible: same fingerprint, same platform and same binaries give
byte-identical outputs. That holds only if modules keep these rules, which the `invoke.determinism`
check tests in part:

- **No global mutable state.** Keep all state in the instance: two instances of a module, in one
  run or in two, must not influence each other. Statics are for constant data only.
- **No file I/O in algorithm code.** Everything a module computes from comes through its documents:
  parameters, inputs and the initialize document. Data a step needs is a workflow input, which the
  runtime reads, checks and covers with the fingerprint. Report through outputs, diagnostics and the
  log callback, not files.
- **No wall-clock time.** Use the tick's `logical_time` and the time base; never the system clock,
  timers or the process id.
- **Randomness only from the seed.** Seed every generator from the step's seed in the initialize
  document, which the runtime derives from the run seed and the step id.
- **Deterministic order.** Never let the iteration order of a hash table, or of anything keyed by
  pointers, reach an output. Sort, or use ordered C++ containers such as `std::map`.
- **Threads are yours to join.** Threads a module starts must not race on shared data, must not
  call into the instance after `destroy`, and must be joined before `destroy` returns.
- **No uninitialized memory**, and no undefined behavior: the sanitizer builds of the runtime run
  module code too.
- **Floating point.** Results agree across platforms only within a tolerance. Compile with
  `-ffp-contract=off` (GCC and Clang) to stop fused multiply-adds from changing results, as the
  example modules do, and parse and format numbers without the process's locale.
- **Text and paths.** Every document is UTF-8 JSON. A module receives no paths; if it must name a
  file in its outputs, use UTF-8.

## Versioning

| Version | Rule |
|---|---|
| The module's (`VERSION`) | Semantic versioning: before 1.0, a change of the minor may break users and a change of the patch must not. Configurations pin an exact version, so a new version is adopted by editing configurations. |
| ABI (`abi`) | The module's ABI major must equal the runtime's; any minor is accepted. A module built with this SDK declares ABI 0.0. |
| PMP (`pmp_version`) | Must be in the runtime's compatibility class (0.1.x) and not newer than the runtime's. |
| SDK (`sdk_version`) | Informational: reported by `describe()` and recorded in the run record. |
| Capabilities | Exact: a module implements `example.estimate@0.1.0` or it does not. A new capability version means a new module version that declares it. |

`praenora_add_module()` writes the ABI, PMP and SDK versions of the SDK it builds with, so
rebuilding against a newer SDK updates them. The policy is in
[specs/compatibility-policy.md](specs/compatibility-policy.md).
