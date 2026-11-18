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
themselves, except the tests of the official AsyncAPI schema, to which CTest gives the schema's
path ([below](#the-official-asyncapi-schema)).

## Labels

| Label | What it covers | Tests |
|---|---|---|
| `unit` | Runtime internals, white-box: documents and YAML, canonical JSON, SHA-256, versions, durations, seeds, CSV and other input sources, configuration, logging, metrics, health, status files, run records, the error table against `status.h`, the SDK's headers, adapter and test kit, and the antimony provider's rules that read a source's text alone. | `praenora_unit_tests` |
| `protocol` | Every schema and catalog entry, valid and invalid documents per kind, graph validation and schedules, graph export, and the committed AsyncAPI documents, also against the official AsyncAPI 3.0.0 schema when the build names a copy of it ([below](#the-official-asyncapi-schema)), and through the official AsyncAPI parser when the build names an installation of it ([below](#the-official-asyncapi-parser)). | `praenora_protocol_tests`, `praenora_asyncapi_official_schema_tests`, `asyncapi.parser.accepts_committed_documents`, `asyncapi.parser.refuses_broken_copies` |
| `contract` | Manifests and discovery, the loader, module instances, the engine and the contract checks, through the real loader and the fixture modules; service providers and runs that use their services, with the fixture providers; the service checks of the simulation and model conversion services; provider libraries as files: their exported symbols, and the same results in two processes, and how the driver of the latter shows two outputs that differ and a run that fails. | `praenora_contract_tests`, `providers.<provider>.exports`, `providers.<provider>.processes`, `providers.run_twice.shows_first_difference` |
| `negative` | What must be refused: bad manifests, configurations and graphs, incompatible modules, failing validations, services no provider serves, providers that do not load in a run, each with its code and location. | `praenora_negative_tests` |
| `lifecycle` | Call order and cleanup, timeouts, unresponsive modules, cancellation and stop requests, calls into one library from instances of several loads of it (they take turns, and are refused behind an abandoned call), and the runtime services an instance is served. | `praenora_lifecycle_tests` |
| `integration` | The public API through the shared library only, and the example workflow end to end: golden outputs, determinism, the fingerprint, files on disk, status queries during a run. | `praenora_integration_tests` |
| `cli` | The `praenora` tool: every command, exit code, `--json` shape and diagnostics format (`cli.<command>.<case>`, driven by `tests/cli/run_cli.cmake`), that driver passing each argument of a command to its program, an empty one included (`cli.driver.passes_empty_arguments`), and showing a command that fails (`cli.driver.shows_failing_command`), and the unit tests of its helpers (`CliTable.*`) and of what it shares with the Python extension (`Frontend.*`, `src/frontend`). | `praenora_cli_tests`, the CLI driver and `tests/cli/check_driver.cmake` |
| `python` | The Python package: CLI parity, errors, the context manager, calls from a second thread. One CTest test, `python.unittest`, runs the `unittest` suite of `tests/python`. | `python.unittest` |
| `install` | An installation into a scratch prefix: its layout (service providers included), a module and an application built against it, the installed tool, a moved installation, no build or source paths in installed files. | `install.consumer` |
| `perf` | The performance probe `praenora_perf`: 2000 ticks of a four-step chain of no-op modules, with and without payload validation. It prints a JSON report and fails only when the probe fails, never on timings. | `perf.validation_strict`, `perf.validation_off` |
| `docs` | The documentation against the source tree: every error code in the error taxonomy, every schema file in the protocol specification, every ADR in its index, every relative link resolving, the open-questions register complete. | `docs.consistency` |

The suites of the GoogleTest labels name the components they test:

| Label | Suites |
|---|---|
| `unit` | `Adapter`, `AntimonyImportCheck`, `AntimonyTextCheck`, `Config`, `Csv`, `Duration`, `Error`, `ErrorTaxonomy`, `Fingerprint`, `Health`, `Json`, `JsonDocument`, `JsonNear`, `Locale`, `Logging`, `Metrics`, `MetricsJson`, `ModelConversionAbi`, `ObservationCsv`, `OutputDir`, `OutputRecorder`, `PathAnchors`, `Paths`, `Platform`, `ProviderAbi`, `Resources`, `RunRecord`, `RuntimeContext`, `SbmlTextCheck`, `Schema`, `SchemaRegistry`, `SdkAbi`, `Seed`, `SemVer`, `Sha256`, `SimulationAbi`, `StatusFile`, `StatusJson`, `StatusNames`, `TestKit`, `Time`, `TraceWriter`, `ValidationReport`, `Version`, `Yaml` |
| `protocol` | `Catalog`, `Graph`, `GraphExport`, `Schemas` |
| `contract` | `Discovery`, `Engine`, `Envelope`, `Instance`, `Loader`, `Manifest`, `ModuleCheck`, `ProviderDiscovery`, `ProviderIndex`, `ProviderLoader`, `ProviderManifest`, `ServiceRun`, `Services`, `TestConversionProvider`, the parameterized `SimulationService` and `ModelConversionService` (the service checks: `<Provider>/SimulationService.<Check>/<ParameterSet>` and `<Provider>/ModelConversionService.<Check>/<ParameterSet>`), and `RoadrunnerProvider` with the roadrunner provider and `AntimonyProvider` with the antimony provider |
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

### The antimony provider

A build with `PRAENORA_WITH_ANTIMONY`
([developer-guide.md](developer-guide.md#the-antimony-provider)) adds tests of the antimony provider
([specs/model-conversion-service.md](specs/model-conversion-service.md#the-antimony-provider)),
most in the `contract` label:

- the service checks with Antimony and SBML (`Antimony/ModelConversionService.<Check>/Antimony`,
  from `tests/contract/model_conversion_service_antimony_test.cpp`): exact translations both ways
  (decay, function definitions, events, rules, a submodel flattened, a submodule chosen by
  `module`, SBML Level 2 Version 4 and Level 3 Versions 1 and 2), the sources each rule refuses, a
  layout request (`UNSUPPORTED`), a syntax error, and a generated chain of 700 reactions whose
  translation holds the provider's lock;
- `AntimonyProvider.*` (`tests/contract/antimony_provider_test.cpp`): an import of a missing file,
  refused in code and harmless in every form of comment, string and block; the round trip; the main
  module and the option `module`; function definitions; libSBML's errors with their line and
  column; warnings; the engine object against what the libraries report; byte order marks and
  CR LF; non-ASCII text; error text that is valid UTF-8; libAntimony's errors, which have no line;
  SBML that names other files, which are never read; distributions, which do not convert back;
  layout requests, refused the same way in different seconds of the wall clock; and SBML that holds
  a layout (`tests/fixtures/models/sbml/laid-out.xml`), which converts to the same Antimony in
  different seconds (both tests sleep until just past the next second between their calls);
- `providers.antimony.exports` and `providers.antimony.processes`
  ([Adding tests](#adding-tests), provider files);
- `Api.TheBuildTreeRuntimeFindsTheAntimonyProvider` (label `integration`) and, in a build with the
  roadrunner provider too, `Api.AntimonyModelSimulatesThroughRoadrunner`, which converts an
  Antimony model with `antimony` and simulates the SBML with `roadrunner` in one process, to a
  relative 1e-9 of the exact solution; and `install.consumer` checks that the installation holds the
  provider and that the installed tool lists it, usable.

The unit tests of the rules that read a source's text alone (`AntimonyImportCheck.*`,
`AntimonyTextCheck.*` and `SbmlTextCheck.*`, in `tests/unit/antimony_source_checks_test.cpp`) need
no libAntimony and run in every build. As with roadrunner, `cli.providers.list_empty` is left out
of such a build. The Antimony models and the expected outputs are in
`tests/fixtures/models/antimony/`. `ctest --preset dev-antimony-tsan -L 'contract|integration|unit'`
runs the concurrency checks under ThreadSanitizer, which sees only the provider's own code:
libAntimony, libSBML and SBMLNetwork are not instrumented. The local preset `dev-engines` builds
both providers and runs the tests of both in one tree.

### The official AsyncAPI schema

A build whose cache variable `PRAENORA_ASYNCAPI_SCHEMA` holds the absolute path of a local copy of
the official AsyncAPI 3.0.0 JSON Schema adds three tests to the `protocol` label
(`tests/protocol/asyncapi_official_schema_test.cpp`). They compile that schema with the validator
of Praenora's own documents:

- `GraphExport.OfficialSchemaIsTheAsyncApi300Schema`: the file is the schema of AsyncAPI 3.0.0
  (its `$id`, and the `asyncapi` version it requires);
- `GraphExport.CommittedAsyncApiIsValidAgainstTheOfficialSchema`: every
  `spec/asyncapi/*.asyncapi.json` is valid against it;
- `GraphExport.OfficialSchemaRefusesBrokenAsyncApiCopies`: copies of a committed document, each
  broken in one place (two of them in a string format, `uri` and `regex`), are refused, with every
  issue at that place or at an object that encloses it, so the test above cannot pass on a schema
  that accepts anything or on a validator that skips formats.

Neither the build nor the tests download the schema, and it is not committed. The copy that was
checked is `schemas/3.0.0.json` of release v6.11.1 of
[asyncapi/spec-json-schemas](https://github.com/asyncapi/spec-json-schemas), with SHA-256
`d4571a420e6ffb7fcc7066c95a6db1202f299a3c51daa103d0706bf30f95e626`; both committed documents are
valid against it. Download it once, outside the source tree, then configure with it:

```bash
curl -fL -o "$HOME/asyncapi-3.0.0.json" \
    https://raw.githubusercontent.com/asyncapi/spec-json-schemas/v6.11.1/schemas/3.0.0.json
shasum -a 256 "$HOME/asyncapi-3.0.0.json"
cmake --preset dev -DPRAENORA_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json"
cmake --build --preset dev
ctest --preset dev -R '^GraphExport\.'
```

Every build of the tests compiles `tests/protocol/asyncapi_official_schema_test.cpp` into an
executable of its own, `praenora_asyncapi_official_schema_tests`, so a change to what it uses
breaks the build at once; only a build with the variable registers its tests. CTest gives them the
copy's path in the environment variable `PRAENORA_TEST_ASYNCAPI_SCHEMA`. Run on its own, the
executable needs that variable, and without it each test fails and says so:

```bash
PRAENORA_TEST_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json" \
    ./build/dev/bin/praenora_asyncapi_official_schema_tests
```

CMake makes a relative value absolute against the current directory; a value that then does not
name a file, or that holds a `;`, `[` or `]`, fails the configuration. Configuring with an empty
value (`-DPRAENORA_ASYNCAPI_SCHEMA=`) removes the three tests; the executable is still built.

The tests pass in the ThreadSanitizer tree as they are. In the AddressSanitizer tree, compiling the
official schema trips AddressSanitizer's container-overflow check when json-schema-validator is a
library built without AddressSanitizer, as the commands in
[platforms.md](platforms.md#json-schema-validator) build it. The report is false: only the
instrumented copies of the container code mark a container's unused capacity, so a container that
the uninstrumented library fills looks overrun. A tree configured with
`PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON` builds the validator from its sources with the tree's own
flags ([platforms.md](platforms.md#json-schema-validator), which also shows how to use a local copy
of the sources instead of downloading them). There the check reports nothing, and the three tests
keep it and pass, with no `ASAN_OPTIONS`:

```bash
cmake --preset dev-asan -DPRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON \
    -DPRAENORA_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json"
cmake --build --preset dev-asan
ctest --preset dev-asan
```

This was verified with a local copy of the validator's sources at the pinned commit, in the
`dev-asan` and `dev-tsan` trees; the download is not yet verified.

When the validator is an installed library instead (an imported target), the build takes it to be
built without AddressSanitizer. So, in a build whose `PRAENORA_SANITIZERS` holds `address` and
whose validator is installed, CTest runs only these three tests with that check off: it appends
`:detect_container_overflow=0` to the `ASAN_OPTIONS` it runs with, and every other test keeps the
check. Run on its own there, the executable needs the same option:

```bash
ASAN_OPTIONS=detect_container_overflow=0 \
    PRAENORA_TEST_ASYNCAPI_SCHEMA="$HOME/asyncapi-3.0.0.json" \
    ./build/dev-asan/bin/praenora_asyncapi_official_schema_tests
```

### The official AsyncAPI parser

The schema checks the documents' structure only. The official AsyncAPI parser,
[`@asyncapi/parser`](https://github.com/asyncapi/parser-js), also applies the rules of the AsyncAPI
specification that a JSON Schema cannot express. A build whose cache variable
`PRAENORA_ASYNCAPI_PARSER` holds the absolute path of a directory into which that parser is
installed adds two tests to the `protocol` label. Each runs
`tests/protocol/asyncapi_parser_check.cjs` with Node.js. The script first checks that the parser
is version 3.6.6, the version whose results it records, then:

- `asyncapi.parser.accepts_committed_documents`: the parser, with its default rules, parses every
  `spec/asyncapi/*.asyncapi.json` and reports for each exactly the diagnostics that the script
  records (`RECORDED_DIAGNOSTICS`), none of which is an error. A diagnostic more, or one fewer,
  fails the test.
- `asyncapi.parser.refuses_broken_copies`: the parser refuses copies of a committed document,
  each broken in one place, each with an error at that place whose code is the broken rule's name
  (`invalid-ref` for a reference that does not resolve). Six of the seven breaks
  are rules that the AsyncAPI 3.0.0 JSON Schema does not express: an operation's message must
  belong to the operation's channel; an operation's channel must be under the root `channels`, and
  a channel's servers under the root `servers`; a channel's key holds no `?`; a channel has no
  parameter that its address does not use; and every reference resolves. The seventh breaks the
  schema, which the parser applies too. So neither a parser that accepts anything nor a check of
  the JSON Schema alone passes this test, while the first test alone would pass on a parser that
  accepts anything and reports the recorded diagnostics.

The parser is given each document's text and reads no other file and no URL: the documents refer
only to themselves. Version 3.6.6 reports one diagnostic for each document, which the script
records: `asyncapi-latest-version` at `/asyncapi`, at the `info` level, saying that the latest
AsyncAPI version is 3.1.0. The documents are AsyncAPI 3.0.0
([ADR-0020](adr/ADR-0020-message-envelope-and-asyncapi.md)). The parser's rules at the `warning`
level, its recommended rules, apply to AsyncAPI 2 documents only.

Neither the build nor the tests download the parser, and nothing of it is committed. Install it
once, outside the source tree, at the version that the script records (`--ignore-scripts` keeps
npm from running the packages' install scripts), then configure with it:

```bash
npm install --prefix "$HOME/asyncapi-parser" --ignore-scripts --no-audit --no-fund \
    @asyncapi/parser@3.6.6
cmake --preset dev -DPRAENORA_ASYNCAPI_PARSER="$HOME/asyncapi-parser"
ctest --preset dev -R '^asyncapi\.parser\.'
```

The script also runs on its own, from the source tree:

```bash
node tests/protocol/asyncapi_parser_check.cjs accepts_committed_documents \
    "$HOME/asyncapi-parser" spec/asyncapi
```

The configuration looks for Node.js (`node` or `nodejs`) with CMake's `find_program()`, which
searches the `bin` directories of `CMAKE_PREFIX_PATH` before the `PATH`; the cache variable
`PRAENORA_NODE_EXECUTABLE` holds the one found, or names another (unset it,
`-UPRAENORA_NODE_EXECUTABLE`, to search again). CTest runs the script without the caller's
`NODE_OPTIONS` and `NODE_PATH`. CMake makes a relative value given without a type
(`-DPRAENORA_ASYNCAPI_PARSER=<dir>`) absolute against the current directory; a value that is then
not absolute, that does not name a directory holding `node_modules/@asyncapi/parser`, or that
holds a `;`, fails the configuration. Configuring with an empty value
(`-DPRAENORA_ASYNCAPI_PARSER=`) removes the two tests. They run Node.js, not code that this build
compiles, so they run alike in every tree, the sanitizer trees included.

With another version of the parser, both tests fail and name the version found. To move to
another version, install it, change `PARSER_VERSION` in the script, run the tests and record in
`RECORDED_DIAGNOSTICS` what the new version reports, once no error is left: an error in a
committed document is fixed in the generator, and the documents regenerated
([Golden files](#golden-files)). If `asyncapi.parser.refuses_broken_copies` then fails, check each
break's expected code and place (`BREAKS`) against the new version's rules, without dropping a
break. npm installs the parser's dependencies at their newest versions that the parser allows, so
another installation of 3.6.6 may run with newer ones; the tests print the versions of the parser,
of `@asyncapi/specs`, the package of the AsyncAPI JSON Schemas, and of `@stoplight/spectral-core`,
which runs the rules, that they ran with. The recorded results come from `@asyncapi/parser` 3.6.6
with `@asyncapi/specs` 6.11.1, whose AsyncAPI 3.0.0 schema is the file of the previous section
(the same SHA-256), and `@stoplight/spectral-core` 1.24.1, on Node.js 26.8.2, macOS arm64.

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
- A sanitizer sees only the code compiled with its flags, and the third-party libraries are linked
  as installed, normally without them: json-schema-validator as
  [platforms.md](platforms.md#json-schema-validator) builds it, and yaml-cpp, spdlog and GoogleTest
  from the package manager. Errors inside them go unnoticed, and AddressSanitizer can report a
  false container overflow when such a library fills a container that instrumented code made. A
  tree configured with `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON` builds json-schema-validator from
  its pinned sources with the tree's own flags
  ([platforms.md](platforms.md#json-schema-validator)); the others stay as installed, and no false
  report from them has been seen.
- In an AddressSanitizer tree configured with `PRAENORA_ASYNCAPI_SCHEMA` whose json-schema-validator
  is an installed library, the tests of the official AsyncAPI schema run with AddressSanitizer's
  container-overflow check off, because such a library is taken to be built without the sanitizer,
  and such a library trips a false report there; every other test keeps the check. With
  `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON` they keep it too
  ([The official AsyncAPI schema](#the-official-asyncapi-schema)).
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

Check refreshed AsyncAPI documents against the official AsyncAPI 3.0.0 schema and with the official
AsyncAPI parser too ([The official AsyncAPI schema](#the-official-asyncapi-schema),
[The official AsyncAPI parser](#the-official-asyncapi-parser)).

The golden envelopes hold sixteen zeros where an envelope id holds the first 16 hex digits of the
run fingerprint, because the fingerprint covers the module libraries' digests and so differs from
build to build. Before refreshing the example's outputs, check what the example promises: at tick 0
every estimate rests on observations, from tick 1 on estimates blend, and every value is finite and
positive.

## Triage

A failing test names its component, and the failure message names an error code, a JSON pointer or
`file:line`, and the step and tick where they matter. The CLI, install, perf and provider-file
drivers show a command that failed on one line, with each argument that is empty or holds
whitespace or a quote character shown in double quotes, and its outputs line by line as they were
printed, as CMake captures them, without the carriage return of each carriage return and line feed
and without NUL bytes ([`tests/support/run_command.cmake`](../tests/support/run_command.cmake) gives
the rules).
Start from the label:

| Failing label | Component | Likely owner |
|---|---|---|
| `unit` | The suite's component (`Csv` is `src/runtime/io/csv`, `Yaml` is `src/runtime/document`, `SdkAbi`, `Adapter` and `TestKit` are the SDK). | runtime |
| `protocol` | `spec/schemas/`, `spec/catalog/` or the graph code (`src/runtime/graph`, `src/runtime/api/graph_export`). A difference from the committed AsyncAPI documents needs a refresh or is a regression. For `asyncapi.parser.*`, the diagnostics that the test prints: a committed document that the parser refuses, or a parser of another version ([The official AsyncAPI parser](#the-official-asyncapi-parser)). | protocol |
| `contract` | The loader, discovery, instance or engine code, or a fixture module that changed. For `providers.run_twice.*`, the driver `tests/providers/run_twice.cmake` or the drivers' shared `tests/support/run_command.cmake`, or, after an intended change of its report, the expected texts in `tests/providers/check_run_twice.cmake`. | runtime |
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
  the shared library and the public API. `NO_TESTS` in place of `LABEL` builds the executable but
  registers none of its tests, for tests that need what a build may lack. The caller then
  registers them with `gtest_discover_tests()` when they can run, as for
  [the official AsyncAPI schema](#the-official-asyncapi-schema): with the build's own test
  environment if they load module libraries, and with one value per property
  (`tests/CMakeLists.txt` says why). Name tests `Suite.Case` in PascalCase, the suite naming the
  component (`Loader.LoadsModuleFromUnicodePath`); the case may end in `_WhenCondition`. Assertions
  should print the error code's name, the JSON pointer or `file:line`, and the step and tick where
  they matter.
- **Helpers.** `tests/support/` holds what tests share: scratch directories, JSON comparison with a
  tolerance, log capture, fixture paths. A test that takes a file's or a directory's permissions
  away gives them back through a `RestorePermissions` guard (`temp_dir.hpp`), declared after its
  `ScratchDir`. Tests get the locations of the source tree, the fixtures,
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
  `test-conversion` offers the model conversion service for the toy formats `test-decay` and
  `test-decay-text`
  ([specs/model-conversion-service.md](specs/model-conversion-service.md#the-test-provider-test-conversion)),
  and the fixture module `converting-module` uses it, with `test-simulation` to simulate what it
  converted, in workflows (`Api.ConvertingModule*`) and in its own test vectors
  (`ModuleCheck.ConvertingModulePassesWithTheConversionService`).
- **Provider files.** `tests/providers/` tests a provider library as a file, for `test-simulation`,
  `test-conversion` and, when built, `roadrunner` and `antimony`: `providers.<provider>.exports`
  lists its exported symbols with `nm` (`check_exports.cmake`; not on Windows) and requires the
  entry point to be the only one, and `providers.<provider>.processes` runs a print tool twice, each
  time in a new process (`run_twice.cmake`), and the two outputs must be the same bytes. Each run's
  standard output is written to a file, `first.out` or `second.out` in
  `build/<preset>/tests/providers/processes/<provider>/`, and the two files are compared byte for
  byte: CMake's capture of an output drops the carriage return of each carriage return and line
  feed and every NUL byte, so two captured texts can be the same when the bytes differ. For the
  simulation providers the tool is `praenora_simulation_print`: it simulates a model of
  `tests/fixtures/models/`, runs a kernel and a plan's batch, and prints every double exactly. For
  the conversion providers it is `praenora_conversion_print`: it converts a source and the result
  back, `tests/fixtures/models/test-decay-text/two-states.txt` to `test-decay` for `test-conversion`
  and `tests/fixtures/models/antimony/decay.ant` to `sbml` for `antimony`, and prints both texts and
  reports verbatim. A run that exits with a nonzero code or prints a sanitizer report fails the
  test, and so does an output that shows no simulation and batch, or no conversion and conversion
  back. The report names the tool and the run, as in
  `praenora_simulation_print (second run): exit code 1, expected 0`, and shows that run's command,
  exit code and outputs; before those of the second run, it shows the first run's outputs. When
  the outputs differ, the report names the tool and the first line where they differ and shows it
  as each run printed it, as CMake captures it; it says which output is shorter when one is a
  prefix of the other, and names the longer one's next byte in hexadecimal; otherwise it names the
  byte of that line where they first differ, counted in the bytes as printed, and that byte of each
  output in hexadecimal, and says why the line looks the same in both when it does; the first run's
  outputs follow, and then the second run's command, exit code and outputs;
  `providers.run_twice.shows_first_difference` (`check_run_twice.cmake`) checks that report, also
  for two outputs that differ only by a carriage return before a line feed or by a NUL byte, the
  report on each failure of either run, and that two runs that printed the same bytes pass, with
  their count.
- **Service checks.** The simulation service's checks
  ([`tests/contract/simulation_service_checks.hpp`](../tests/contract/simulation_service_checks.hpp))
  are a parameterized suite: a provider of the service is checked by a source file that instantiates
  `SimulationService` with its parameter sets (provider manifest, model format and text, ids, exact
  solution, tolerances), as `tests/contract/simulation_service_test.cpp` does for `test-simulation`.
  The model conversion service's checks
  ([`tests/contract/model_conversion_service_checks.hpp`](../tests/contract/model_conversion_service_checks.hpp))
  work the same way: `ModelConversionService` with a parameter set (provider manifest, engine
  object, conversions, exact translations, refusals, an engine failure, optionally a source with
  several modules and one that holds the provider's lock), as
  `tests/contract/model_conversion_service_test.cpp` does for `test-conversion` and
  `tests/contract/model_conversion_service_antimony_test.cpp` for `antimony`. Add the file to
  `praenora_contract_tests` in `tests/CMakeLists.txt`.
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
