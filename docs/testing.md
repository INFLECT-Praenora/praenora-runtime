# Testing

This guide explains how to run the tests, what each label covers, how golden files are refreshed,
how a failure leads to the component and the party that has to fix it, the acceptance gate for
module versions, and how to add tests. The commands run from the repository root against the `dev`
build ([developer-guide.md](developer-guide.md#configure-build-and-test)) and were run on macOS
arm64.

## Running the tests

```bash
ctest --preset dev
```

runs every test of the build. The test presets print the output of failing tests and fail when no
test matches the selection. Subsets:

```bash
ctest --preset dev -N
ctest --preset dev -L lifecycle
ctest --preset dev -R '^Loader\.'
ctest --preset dev -R '^Example\.RunsTwelveTicks$'
ctest --preset dev -L docs -V
ctest --preset dev -L unit --output-junit "${TMPDIR:-/tmp}/praenora-junit.xml"
```

- `-N` lists the tests without running them.
- `-L <label>` selects a label and `-LE <label>` leaves one out; labels are regular expressions,
  so `-L 'unit|protocol'` selects two.
- `-R <regex>` selects tests by name. GoogleTest tests are named `Suite.Case`, and the suite names
  the component, so `-R '^Loader\.'` runs the loader's tests.
- `-V` prints the output of every test, passing ones included.
- `--output-junit <file>` writes JUnit XML, for tools that read it; a relative file name is relative
  to the build directory.

A GoogleTest executable can also run on its own, with GoogleTest's options:

```bash
./build/dev/bin/praenora_unit_tests --gtest_filter='Csv.*'
```

The tests write files only under the system's temporary directory or the build tree, use no
network, and need nothing from the environment: tests that read `PRAENORA_*` variables set them
themselves.

## Labels

| Label | What it covers | Tests |
|---|---|---|
| `unit` | Runtime internals, white-box: documents and YAML, canonical JSON, SHA-256, versions, durations, seeds, CSV and other input sources, configuration, logging, metrics, health, status files, run records, the error table against `status.h`, and the SDK's headers, adapter and test kit. | `praenora_unit_tests` |
| `protocol` | Every schema and catalog entry, valid and invalid documents per kind, graph validation and schedules, graph export, and the committed AsyncAPI documents. | `praenora_protocol_tests` |
| `contract` | Manifests and discovery, the loader, module instances, the engine and the contract checks, through the real loader and the fixture modules; service providers and runs that use their services, with the fixture providers; the service checks of the simulation service; provider libraries as files: their exported symbols, and the same results in two processes. | `praenora_contract_tests`, `providers.<provider>.exports`, `providers.<provider>.processes` |
| `negative` | What must be refused: bad manifests, configurations and graphs, incompatible modules, failing validations, services no provider serves, providers that do not load in a run, each with its code and location. | `praenora_negative_tests` |
| `lifecycle` | Call order and cleanup, timeouts, unresponsive modules, cancellation and stop requests, calls into one library from instances of several loads of it (they take turns, and are refused behind an abandoned call), and the runtime services an instance is served. | `praenora_lifecycle_tests` |
| `integration` | The public API through the shared library only, and the example workflow end to end: golden outputs, determinism, the fingerprint, files on disk, status queries during a run. | `praenora_integration_tests` |
| `cli` | The `praenora` tool: every command, exit code, `--json` shape and diagnostics format (`cli.<command>.<case>`, driven by `tests/cli/run_cli.cmake`), how that driver shows a command that fails (`cli.driver.shows_failing_command`), and the unit tests of its helpers (`CliTable.*`) and of what it shares with the Python extension (`Frontend.*`, `src/frontend`). | `praenora_cli_tests`, the CLI driver and `tests/cli/check_driver.cmake` |
| `python` | The Python package: CLI parity, errors, the context manager, calls from a second thread. One CTest test, `python.unittest`, runs the `unittest` suite of `tests/python`. | `python.unittest` |
| `install` | An installation into a scratch prefix: its layout (service providers included), a module and an application built against it, the installed tool, a moved installation, no build or source paths in installed files. | `install.consumer` |
| `perf` | The performance probe `praenora_perf`: 2000 ticks of a four-step chain of no-op modules, with and without payload validation. It prints a JSON report and fails only when the probe fails, never on timings. | `perf.validation_strict`, `perf.validation_off` |
| `docs` | The documentation against the source tree: every error code in the error taxonomy, every schema file in the protocol specification, every ADR in its index, every relative link resolving, the open-questions register complete. | `docs.consistency` |

The suites of the GoogleTest labels name the components they test:

| Label | Suites |
|---|---|
| `unit` | `Adapter`, `Config`, `Csv`, `Duration`, `Error`, `ErrorTaxonomy`, `Fingerprint`, `Health`, `Json`, `JsonDocument`, `JsonNear`, `Locale`, `Logging`, `Metrics`, `MetricsJson`, `ObservationCsv`, `OutputDir`, `OutputRecorder`, `PathAnchors`, `Paths`, `Platform`, `ProviderAbi`, `Resources`, `RunRecord`, `RuntimeContext`, `Schema`, `SchemaRegistry`, `SdkAbi`, `Seed`, `SemVer`, `Sha256`, `SimulationAbi`, `StatusFile`, `StatusJson`, `StatusNames`, `TestKit`, `Time`, `TraceWriter`, `ValidationReport`, `Version`, `Yaml` |
| `protocol` | `Catalog`, `Graph`, `GraphExport`, `Schemas` |
| `contract` | `Discovery`, `Engine`, `Envelope`, `Instance`, `Loader`, `Manifest`, `ModuleCheck`, `ProviderDiscovery`, `ProviderIndex`, `ProviderLoader`, `ProviderManifest`, `ServiceRun`, `Services`, the parameterized `SimulationService` (the service checks: `<Provider>/SimulationService.<Check>/<ParameterSet>`), and, with the roadrunner provider, `RoadrunnerProvider` |
| `negative` | `Compat`, `Config`, `ConfigBadFixtures`, `Graph`, `Manifest`, `ProviderLoadFailure`, `ProviderManifest`, `ServiceValidation`, `Validation` |
| `lifecycle` | `Cancel`, `Lifecycle`, `ServiceLookups`, `ServiceVisibility`, `Timeout` |
| `integration` | `Api`, `Example` |
| `cli` | `CliTable`, `Frontend` (beside the `cli.<command>.<case>` driver tests, which are not GoogleTest) |

Builds without the examples, the CLI, the Python bindings or the install rules have no tests that
need them: `PRAENORA_BUILD_EXAMPLES=OFF` drops the `Example.*` tests, the CLI tests of the example
workflow and the contract checks of the example modules (and the Python tests that need the example
modules are skipped), `PRAENORA_BUILD_CLI=OFF` drops the `cli` label, `PRAENORA_BUILD_PYTHON=OFF`
drops `python.unittest`, and `PRAENORA_INSTALL=OFF` drops `install.consumer`.

### The roadrunner provider

A build with `PRAENORA_WITH_ROADRUNNER`
([developer-guide.md](developer-guide.md#the-roadrunner-provider)) adds tests of the roadrunner
provider ([specs/simulation-service.md](specs/simulation-service.md#the-roadrunner-provider)),
most in the `contract` label:

- the service checks with two SBML models whose solutions are known exactly
  (`Roadrunner/SimulationService.<Check>/Decay` and `.../TwoCompartments`, from
  `tests/contract/simulation_service_roadrunner_test.cpp`), which require agreement to a relative
  1e-9;
- `RoadrunnerProvider.*` (`tests/contract/roadrunner_provider_test.cpp`): the engine identity, the
  model texts it refuses (paths, URIs, texts libRoadRunner would read as a path or a URI, document
  type declarations, required packages, `comp`, a flux-balance model it cannot simulate), SBML
  Level 1, Level 2 and Level 3 Version 1 (the other models it simulates are Level 3 Version 2), the
  ids of a model with every kind of rule and of models whose stoichiometry an initial assignment or
  a `stoichiometryMath` sets, events, delayed events across runs and at the start, simultaneous
  events, solver settings, the step limit, solver steps (also at the row limit) and cancellation
  within a run;
- `providers.roadrunner.exports` and `providers.roadrunner.processes`
  ([Adding tests](#adding-tests), provider files);
- `Api.TheBuildTreeRuntimeFindsTheRoadrunnerProvider` (label `integration`); and `install.consumer`
  checks that the installation holds the provider and that the installed tool lists it, usable.

In such a build the build tree's runtime finds the provider without a provider path, so a test that
expects no provider of `praenora.simulation@0` there (`cli.providers.list_empty`, part of
`Api.RunSimulatesThroughTheSimulationService`) is left out or adapted. The SBML models are in
`tests/fixtures/models/sbml/`. `ctest --preset dev-roadrunner-tsan -L contract` runs the concurrency
checks under ThreadSanitizer, which sees only the provider's own code: libRoadRunner, LLVM and the
code they generate are not instrumented.

## Sanitizer builds

The `dev-asan` and `dev-tsan` presets ([developer-guide.md](developer-guide.md#sanitizer-builds))
run the same tests under AddressSanitizer with UndefinedBehaviorSanitizer, or ThreadSanitizer:

```bash
ctest --preset dev-asan
ctest --preset dev-tsan
```

- AddressSanitizer and ThreadSanitizer reports make the process exit with an error, which fails
  the test. So does the first UndefinedBehaviorSanitizer report (`runtime error: ...`): a build
  whose `PRAENORA_SANITIZERS` holds `undefined`, such as `dev-asan`, compiles with
  `-fno-sanitize-recover=undefined`, so the process ends with an error there instead of going on.
  The CLI, install, perf and provider-file drivers also fail a command whose output holds any
  sanitizer report, whatever its exit code.
- On macOS, AddressSanitizer test runs set `PRAENORA_DEBUG_KEEP_LIBRARIES=1`, so module libraries
  stay loaded: the sanitizer keeps an unloaded library's globals registered, which would trip false
  reports against a library loaded later at the same address. Unloading is tested by every other
  build, ThreadSanitizer included.
- `python.unittest` is registered but disabled: an instrumented extension cannot be loaded into an
  interpreter that is not instrumented. A run of the label alone therefore matches no test that
  runs, and the presets' "no tests" rule makes it fail with exit code 8:

```bash
ctest --preset dev-asan -L python
```

A full run of a sanitizer tree is unaffected and lists `python.unittest` as not run.

## Golden files

Two kinds of expected output are committed and compared byte for byte or within a tolerance:

| Files | Compared by | Refreshed by |
|---|---|---|
| `tests/fixtures/golden/example.estimate-predict/` (`predictions.jsonl`, `state.jsonl`) | `Example.OutputsMatchGolden`, within a tolerance | `PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^Example\.OutputsMatchGolden$'` |
| `spec/asyncapi/*.asyncapi.json` | `GraphExport.CommittedAsyncApiMatchesGenerator` and `cli.graph.asyncapi_matches_committed`, byte for byte | `PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^GraphExport\.CommittedAsyncApiMatchesGenerator$'` |

Refreshing never happens by itself: only `PRAENORA_UPDATE_GOLDEN=1` makes those tests write the
files instead of comparing them. Refresh after a deliberate change to the example modules, their
parameters or data, a workflow definition, a message type or the envelope schema, then review the
difference before keeping it:

```bash
PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^Example\.OutputsMatchGolden$'
PRAENORA_UPDATE_GOLDEN=1 ctest --preset dev -R '^GraphExport\.CommittedAsyncApiMatchesGenerator$'
git diff --stat -- tests/fixtures/golden spec/asyncapi
```

The golden envelopes hold sixteen zeros where an envelope id holds the first 16 hex digits of the
run fingerprint, because the fingerprint covers the module libraries' digests and so differs from
build to build. Before refreshing the example's outputs, check what the example promises: at tick 0
every estimate rests on observations, from tick 1 on estimates blend, and every value is finite and
positive.

## Triage

A failing test names its component, and the failure message names an error code, a JSON pointer or
`file:line`, and the step and tick where they matter. The CLI, install, perf and provider-file
drivers show a command that failed on one line, with each argument that is empty or holds
whitespace or a quote character shown in double quotes
([`tests/support/run_command.cmake`](../tests/support/run_command.cmake) gives the rule). Start
from the label:

| Failing label | Component | Likely owner |
|---|---|---|
| `unit` | The suite's component (`Csv` is `src/runtime/io/csv`, `Yaml` is `src/runtime/document`, `SdkAbi`, `Adapter` and `TestKit` are the SDK). | runtime |
| `protocol` | `spec/schemas/`, `spec/catalog/` or the graph code (`src/runtime/graph`, `src/runtime/api/graph_export`). A difference from the committed AsyncAPI documents needs a refresh or is a regression. | protocol |
| `contract` | The loader, discovery, instance or engine code, or a fixture module that changed. | runtime |
| `negative` | Validation: a bad input was accepted, or refused with another code or location. | runtime |
| `lifecycle` | `src/runtime/instance` and `src/runtime/engine`: call order, deadlines, cancellation, teardown. | runtime |
| `integration` | The public API (`src/runtime/api`), or the example modules when their outputs changed (golden files). | runtime, or the example modules |
| `cli` | `src/cli`, or the API behind the command; the test's name says which command. For `cli.driver.*`, the drivers' shared `tests/support/run_command.cmake`. | runtime |
| `python` | `src/python` and `python/praenora/runtime`; or the interpreter, when the import fails. | runtime, or environment |
| `install` | The install rules and the CMake package (`cmake/PraenoraInstall.cmake`, `cmake/PraenoraConfig.cmake.in`, `cmake/PraenoraModule.cmake`); the driver prints each step it checked. | runtime, or environment |
| `perf` | Only when the probe crashes or prints no report. | runtime |
| `docs` | The document or link named in the message: a new error code, schema or ADR without its documentation, or a moved file. | whoever changed it |

When a run or a validation fails, its error code points further. By code range:

| Codes | Component | Likely owner |
|---|---|---|
| 100-103 (`PARSE_ERROR`, `SCHEMA_VIOLATION`, ...) | `document`: the file at the location given | user, or the module for a manifest |
| 200-204 (`CONFIG_INVALID`, `BINDING_INVALID`, `PARAMETERS_INVALID`, ...) | `config` and validation: the workflow configuration | user |
| 300-304 (`MANIFEST_INVALID`, `MODULE_NOT_FOUND`, `LIBRARY_DIGEST_MISMATCH`, ...) | `discovery` and `loader`: the manifest, the module search paths | module, or environment |
| 400-407 (`ABI_INCOMPATIBLE`, `PLATFORM_MISMATCH`, `CATALOG_ENTRY_NOT_FOUND`, ...) | `compat` and `catalog`; `graph` for `MESSAGE_TYPE_INCOMPATIBLE`; validation and the contract checks for `SERVICE_UNAVAILABLE` | module, environment or protocol |
| 500-502 (`LIBRARY_LOAD_FAILED`, `ENTRY_POINT_MISSING`, `API_TABLE_INVALID`) | `loader`: the module's library | module, or environment |
| 600-604 (`GRAPH_INVALID`, `GRAPH_CYCLE`, `PORT_*`) | `graph`: the workflow definition | protocol |
| 700-705 (`STEP_FAILED`, `OUTPUT_INVALID`, `STEP_TIMEOUT`, `MODULE_UNRESPONSIVE`, ...) | `engine` and `instance`: the step named in the details | module, or the configuration's timeouts; `INPUT_MISSING` is the runtime's |
| 800-801 (`TARGET_DATA_INVALID`, `TARGET_DATA_UNAVAILABLE`) | `io` and validation: the data file and line, or the input's `path` | user data, or environment |
| 1-12 (`INVALID_ARGUMENT`, `IO_ERROR`, `INTERNAL`, ...) | Wherever the message says | caller, environment, or the runtime for `INTERNAL` and `UNKNOWN` |

The [error taxonomy](specs/error-taxonomy.md) gives each code's owner, exit code and retryable
flag, and its [triage table](specs/error-taxonomy.md#triage) says where to look for each one. A
module's own status is never the error's code: it is the `cause` of a `STEP_FAILED`, with the
module's message.

## The module acceptance gate

A module version is adopted only when:

1. the module's own test suite passes, in its own repository (the SDK's in-process test kit,
   `praenora/sdk/testing.hpp`, drives a module without loading it);
2. `praenora modules check <manifest>` passes against a build of the module: no check fails;
3. the module ships test vectors (`tests.vectors` in its manifest), so that the checks
   `invoke.result_schema`, `invoke.expected_outputs` and `invoke.determinism` run instead of being
   skipped.

The checks, the test vectors and the test kit are described in
[module-developer-guide.md](module-developer-guide.md#contract-checks). The example modules pass the
gate in every build that has them: the contract tests run the checks on them.

## Adding tests

- **GoogleTest.** Add the source to an executable in
  [`tests/CMakeLists.txt`](../tests/CMakeLists.txt), or a new executable with
  `praenora_add_gtest(<name> LABEL <label> [WHITEBOX] SOURCES ...)`. With `WHITEBOX` it links the
  runtime's object library and may include internal headers (`praenora::detail`); without it, only
  the shared library and the public API. Name tests `Suite.Case` in PascalCase, the suite naming the
  component (`Loader.LoadsModuleFromUnicodePath`); the case may end in `_WhenCondition`. Assertions
  should print the error code's name, the JSON pointer or `file:line`, and the step and tick where
  they matter.
- **Helpers.** `tests/support/` holds what tests share: scratch directories, JSON comparison with a
  tolerance, log capture, fixture paths. Tests get the locations of the source tree, the fixtures,
  the test catalog and the fixture and example modules as compile definitions
  (`PRAENORA_TEST_SOURCE_DIR`, `PRAENORA_TEST_FIXTURES_DIR`, `PRAENORA_TEST_CATALOG_DIR`,
  `PRAENORA_TEST_MODULES_DIR`, `PRAENORA_EXAMPLE_MODULES_DIR`).
- **Fixture modules.** Add one with `praenora_add_fixture_module()` in
  [`tests/fixtures/modules/CMakeLists.txt`](../tests/fixtures/modules/CMakeLists.txt)
  ([developer-guide.md](developer-guide.md#adding-a-module-locally)); test capabilities and message
  types go into `tests/fixtures/catalog/`. Its manifest comes from `<name>/<name>.pmm.json.in` when
  that file exists, otherwise `praenora_add_module()` generates it; `REQUIRES_SERVICES` and
  `OPTIONAL_SERVICES` declare runtime services. The modules built from `fx-service/` use the test
  service `test.echo@0` of the fixture provider `test-echo` and report what `get_service()`
  returned.
- **Fixture providers.** Add one with `praenora_add_fixture_provider()` in
  [`tests/fixtures/providers/CMakeLists.txt`](../tests/fixtures/providers/CMakeLists.txt). A
  provider is loaded once per process and never unloaded, so a test that needs a provider's first
  load, or a copy of one under its name, uses a provider no other test loads.
  `make_provider_context()` (`tests/support/validation_fixtures.hpp`) builds a runtime context over
  fixture providers; `copy_fixture_provider()` and `copy_fixture_module()`
  (`tests/support/provider_fixtures.hpp`) copy one, manifest edited, into a scratch directory.
  `test-simulation` offers the simulation service for the toy model format `test-decay`
  ([specs/simulation-service.md](specs/simulation-service.md#the-test-provider-test-simulation)),
  and the fixture module `simulating-module` uses it in workflows and in its own test vectors.
- **Provider files.** `tests/providers/` tests a provider library as a file, for `test-simulation`
  and, when built, `roadrunner`: `providers.<provider>.exports` lists its exported symbols with `nm`
  (`check_exports.cmake`; not on Windows) and requires the entry point to be the only one, and
  `providers.<provider>.processes` runs `praenora_simulation_print` twice, each time in a new
  process (`run_twice.cmake`): it simulates a model of `tests/fixtures/models/`, runs a kernel and a
  plan's batch, prints every double exactly, and the two outputs must be the same bytes.
- **Service checks.** The simulation service's checks
  ([`tests/contract/simulation_service_checks.hpp`](../tests/contract/simulation_service_checks.hpp))
  are a parameterized suite: a provider of the service is checked by a source file that instantiates
  `SimulationService` with its parameter sets (provider manifest, model format and text, ids, exact
  solution, tolerances), as `tests/contract/simulation_service_test.cpp` does for `test-simulation`.
  Add the file to `praenora_contract_tests` in `tests/CMakeLists.txt`.
- **CLI tests.** Add one with `praenora_add_cli_test(cli.<command>.<case> CMD ... EXPECT_EXIT ...)`
  in [`tests/cli/CMakeLists.txt`](../tests/cli/CMakeLists.txt); the driver checks the exit code,
  regular expressions on both outputs, values in the JSON output and files left behind. The tests
  run in the caller's environment: one that must find no service provider there sets
  `ENVIRONMENT "PRAENORA_PROVIDER_PATH="`, an empty value, which counts as unset.
- **Python tests.** Add a `test_*.py` module of stdlib `unittest` tests to `tests/python/`
  (classes `<Topic>Test`, methods `test_<what>`); `tests/python/support.py` holds the environment
  and fixture paths they share. `python.unittest` discovers them.
- **Documents.** A new error code, schema file or ADR needs its documentation, or `docs.consistency`
  fails, naming what is missing.
- **Data.** Test data and golden files use LF line endings (`.gitattributes`), except the fixtures
  under `bom-crlf/` directories, which carry a byte order mark and CRLF on purpose.
