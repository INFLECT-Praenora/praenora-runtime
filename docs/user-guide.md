# User guide

This guide is for people who run workflows: it covers getting the `praenora` tool, its commands,
running the example workflow, finding modules and service providers, the files a run writes,
watching a run's status, and what to do when something fails. How to write a workflow
configuration is in [configuration.md](configuration.md); using the runtime from Python is in
[python.md](python.md).

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

An installation holds the tool, the runtime library, the headers, the CMake package, the schemas
and catalog, and the license files in `share/praenora/licenses/`: this repository's MIT license
(`praenora-runtime/LICENSE`), those of the dependencies built in the tree, and those of the
directories that `PRAENORA_THIRD_PARTY_LICENSE_DIRS` names. A static or header-only library found
installed brings its license files only through that variable, so the installation of a `dev`
build holds `praenora-runtime/LICENSE` alone unless the variable is set
([developer-guide.md](developer-guide.md#license-files)). The service providers built in the tree
are installed too, and a build with the Python environment installs the environment with the
provider `python`
([The Python environment in an installation](#the-python-environment-in-an-installation)). The
example modules are installed only on request, as the component `Examples`, into the
installation's module directory, where the tool finds them without a search path. An installation
can be moved:

```bash
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install" --component Examples
"${TMPDIR:-/tmp}/praenora-install/bin/praenora" modules list
```

A release archive holds such an installation, without the example modules, below one directory of
its own name, `praenora-runtime-<version>-<os>-<arch>-<compiler>-<config>`
([developer-guide.md](developer-guide.md#archiving-an-installation)); the Python package is built
as a wheel instead ([python.md](python.md#building-a-wheel)). It holds the three service
providers that the CI presets build, `roadrunner`, `antimony` and `python` (those of v0.2.0, made
before the provider `python`, hold the first two), the Python environment that `python` runs, in
`lib/praenora/python-environment/` (about a gigabyte unpacked;
[The Python environment in an installation](#the-python-environment-in-an-installation)), and the
license files of the third-party libraries that the providers and the runtime link statically and
of the environment, in `share/praenora/licenses/` (the environment's in `python-environment/`
there). Beside it, a `.sha256` file holds its SHA-256. Check it, unpack it anywhere and run the
tool where it is (`<name>` stands for the archive's name):

```bash
shasum -a 256 -c <name>.tar.xz.sha256          # sha256sum -c on Linux
tar -xJf <name>.tar.xz
./<name>/bin/praenora version
./<name>/bin/praenora providers list           # the three providers, usable
```

On macOS, a browser marks what it downloads as quarantined, and Gatekeeper may then refuse to run
the tool, whose programs have no Developer ID signature and are not notarized (OQ-13 in
[open-questions.md](open-questions.md)). `gh release download` and `curl` set no such mark. For an
archive downloaded with a browser, remove the mark from the directory it was unpacked into (the
command was run on macOS 15; a browser's download of an archive is not yet verified):

```bash
xattr -dr com.apple.quarantine <name>
```

On Windows the archive is a `.zip` and the tool `<name>\bin\praenora.exe`. In PowerShell, compare
the archive's SHA-256 with the first word of its `.sha256` file, then unpack it into a short
directory, since the Python environment's paths are long
([Long paths on Windows](#the-python-environment-in-an-installation)) (not yet verified):

```powershell
$expected = (-split (Get-Content <name>.zip.sha256))[0]
(Get-FileHash -Algorithm SHA256 <name>.zip).Hash -eq $expected   # True (-eq ignores case)
Expand-Archive <name>.zip -DestinationPath .
.\<name>\bin\praenora.exe version
.\<name>\bin\praenora.exe providers list
```

Each archive needs:

| Archive | Needs |
|---|---|
| `macos-arm64` | macOS 14 or newer on Apple silicon: the deployment target of the `ci-macos-arm64-release` preset. |
| `linux-x86_64` | A glibc and a libstdc++ at least as new as those of Ubuntu 24.04, on which it is built (not yet verified). |
| `windows-x86_64` | Windows 10 or newer and the Microsoft Visual C++ runtime of Visual Studio 2022 (the Visual C++ Redistributable for Visual Studio 2015–2022, x64), whose DLLs the runtime loads; the `debug` archive needs the debug runtime, which comes with Visual Studio, not with the Redistributable (not yet verified). |

Such an archive was made and run on macOS arm64 from a local build. The workflow of
[releasing.md](releasing.md) makes one per platform key and checks that it needs only the system's
libraries, and its release job runs the Linux archive's tool
([releasing.md](releasing.md#what-the-runs-verified)). The releases `v<version>` of the repository
on GitHub hold one archive per platform key with its `.sha256` file; the first, v0.2.0, was
published on 2026-11-22.

## Commands

| Command | Does |
|---|---|
| `praenora version` | Versions of the runtime, SDK, protocol (PMP), ABI and each document format; platform, compiler and build type. |
| `praenora validate <config>` | Everything a run checks before its first tick, without running: the file, the catalog, the graph, the bindings, the modules and the runtime services they require (without loading anything), the parameters and the input data. `--load` also starts the service providers of those services, and loads each module library and checks its identity. |
| `praenora validate --document <file>` | Checks any Praenora document (a manifest, a catalog entry, test vectors, a run record...) against the schema of its kind. |
| `praenora modules list` | Every module manifest on the module search paths. |
| `praenora modules inspect <name>[@<version>]` | One module as discovery resolves it (its highest version for this platform by default); `--load` adds its `describe()` document and its library's SHA-256. |
| `praenora modules check <manifest>` | The contract checks of a module ([module-developer-guide.md](module-developer-guide.md#contract-checks)), with the services its manifest declares served by their providers. `--vectors F` uses other test vectors. |
| `praenora providers list` | Every service provider manifest on the provider search paths: the services each offers and whether the runtime can use it ([Finding service providers](#finding-service-providers)). |
| `praenora graph <config>` or `--workflow <name>@<version>` | A workflow definition's schedule and connections: `--format text` (default), `json`, `dot` or `asyncapi`. |
| `praenora run <config>` | Validates and runs a workflow. Options: `--ticks N`, `--seed S`, `--output-dir D`, `--overwrite`, `--trace`, `--status-file F`, `--isolation in-process\|process`; `--listen ADDR` serves the run through the control API while it runs ([Watching and running over HTTP](#watching-and-running-over-http)). Warnings that validation found (for example data rows before the run's start) go to standard error, and with `--json` into `result.diagnostics`; they do not stop the run. |
| `praenora serve --listen ADDR --config-root D --output-root D` | A long-running server of the control API that opens runs on request ([Watching and running over HTTP](#watching-and-running-over-http)). |
| `praenora status <file>` | Prints the status file a run keeps. |
| `praenora status --server ADDR` | The runs of a server of the control API, or with `--run ID` one run's status. |
| `praenora catalog list` / `catalog show <id>@<version>` | The protocol catalog's entries (`--kind workflows`, `capabilities` or `messages`), or one entry's document. |
| `praenora schema list` / `schema show <kind>` | The embedded JSON Schemas; `--format-version V` picks a version. |
| `praenora help [<command>]` | The usage of the tool or of a command, as `--help` does. |

Every command takes these options, before or after its arguments:

| Option | Meaning |
|---|---|
| `--json` | Standard output holds exactly one JSON document: `{"ok": true, "result": ...}` or `{"ok": false, "error": ..., "diagnostics": [...]}`. Messages still go to standard error. |
| `--module-path P` | Search `P` for modules first (repeatable). |
| `--provider-path P` | Search `P` for service providers first (repeatable). |
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
| 3 | Validation failed: the printed diagnostics say where. `SERVICE_UNAVAILABLE`, when no usable service provider offers a service that a module requires, is one such failure. |
| 4 | A module library could not be loaded or checked. |
| 5 | A module call timed out or never returned. |
| 70 | An internal error, usually a defect of the runtime. |
| 74 | An environment error. `IO_ERROR`: a file or directory that is missing or unreadable, such as the configuration, a document, a manifest, a status file or a catalog directory; a file that cannot be written; or an output directory that holds an earlier run (a missing or unreadable data file is `TARGET_DATA_UNAVAILABLE`, exit 3). `RESOURCE_EXHAUSTED`: the run ran out of memory or a worker thread could not start. |
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

`--isolation process` runs each step's module in a module host, a process of its own, instead of
the tool's process ([Process isolation](#process-isolation)). The outputs and the fingerprint are
the same; the run record says where each step ran:

```bash
./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --output-dir "$RUN" --overwrite --isolation process
```

A build with the Python environment (the local preset `dev-python`) also builds
`example-predictor-python`, the predictor as a Python module
([module-developer-guide.md](module-developer-guide.md#modules-in-python)), and the configuration
`workflow-python.yaml` binds the `predict` step to it. Its outputs are those of `workflow.yaml`,
byte for byte, but for the start of each message's `id`, which names the run: the module differs,
and so does the fingerprint. Python steps are best run in module hosts:

```bash
./build/dev-python/bin/praenora run examples/workflows/estimate-predict/workflow-python.yaml \
    --module-path build/dev-python/modules --output-dir "$RUN-python" --isolation process
```

## Process isolation

A module is native code: in the runtime's process, a segmentation fault or an `abort()` in it ends
the runtime, and a call that never returns can only be abandoned, its library kept loaded. A step
in process mode runs its module instance in a module host, the executable
`praenora-module-host` that the runtime starts for that step alone and ends with the run:

- a crash of the module (a signal, an `abort()`, an `exit()`) fails its step with `MODULE_CRASHED`
  (exit 1), whose details say how the module host ended, for example
  `{"phase": "invoke", "signal": 11, "signal_name": "SIGSEGV"}`; the step's state is `crashed`, the
  other steps are finalized and destroyed as usual, and the runtime goes on to write its run record;
- a call that outlives its deadline and the cancellation grace is ended by killing its module host
  (`MODULE_UNRESPONSIVE` with `details.terminated: true`), and nothing of the library is held on
  to: the next run uses it at once;
- Ctrl+C reaches only the runtime, which stops the call in the module host: the run ends as
  `stopped`, never as a crash.

Choose it per step or for the whole run in the configuration (`isolation.mode`,
[configuration.md](configuration.md#isolation)), or for every step of one run with `--isolation`.
`praenora validate --load` loads such a step's module in a module host too. A step whose module
uses runtime services has their providers (engines such as libRoadRunner) loaded in its module host,
so a crash in the engine ends only that step too, and a long engine call that outlives its deadline
is ended with the module host. Steps in module hosts that set `share_compiled_models` compile a
model once between them: the provider of the first keeps the compiled model in a store in the
runtime's process, and the others load it from there
([configuration.md](configuration.md#shared-compiled-models)). On macOS, a step in a module host
with `sandbox: restricted` keeps its module host's own process from the network, from starting
programs and from writing files outside its own temporary directory
([configuration.md](configuration.md#the-restricted-sandbox-profile)); it is no boundary against a
module meant to do harm, and a runtime that itself runs in a sandbox cannot give a module host the
profile. Each call costs a round trip to the module host;
[specs/process-isolation.md](specs/process-isolation.md) has the details. In this version no step
can run in a module host on Windows (`UNSUPPORTED`). The tool finds the module host beside its
runtime library, in `libexec/praenora/` of the build tree or the installation.

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

## Finding service providers

Some modules use a runtime service, such as a simulation engine, that a **service provider**
offers: a library shipped with the runtime, which the runtime loads only when a run,
`validate --load` or the contract checks need one of its services, and then keeps loaded for the
rest of the process ([specs/runtime-services.md](specs/runtime-services.md)). A module's manifest
lists the services it requires. When no usable provider offers one of them, validation fails with
`SERVICE_UNAVAILABLE` (exit code 3), located in the module's manifest, and the diagnostic's details
name the service, the provider search paths and every provider that offers a service of that name
but cannot serve it, with the reason.

The runtime looks for provider manifests (`*.provider.json`) in each provider search path and its
immediate subdirectories: the paths given with `--provider-path`, in order, then the entries of the
environment variable `PRAENORA_PROVIDER_PATH` (`:`-separated, `;` on Windows), then the
installation's provider directory, `<prefix>/lib/praenora/providers`
([search order](specs/runtime-services.md#discovery)). The first manifest of a provider name for a
platform wins. A provider is native code that runs with the tool's privileges, like a module, so put
only providers you trust on these paths.

`praenora providers list` shows what was found, in search order: each provider's name, version and
platform, the services it offers (`<name>@<major>.<minor>`), its manifest, and whether this runtime
can use it. A provider built for another platform, another runtime version or another provider
interface is listed `unusable`, with the reason, and so is a manifest shadowed by an earlier one of
the same name and platform, which is never used, naming the manifest that shadows it. Listing loads
nothing. With the test providers of the `dev` build:

```bash
./build/dev/bin/praenora providers list --provider-path build/dev/test-providers/test-echo \
    --provider-path tests/fixtures/configs/cli/providers
```

```text
test-echo         0.1.0  macos-arm64   test.echo@0.0  .../build/dev/test-providers/test-echo/test-echo.provider.json               usable
test-old-runtime  0.1.0  linux-x86_64  test.echo@0.0  .../tests/fixtures/configs/cli/providers/old-runtime.provider.json           unusable: built for linux-x86_64; this host is macos-arm64
test-old-runtime  0.2.0  linux-x86_64  test.echo@0.0  .../tests/fixtures/configs/cli/providers/shadowed/old-runtime.provider.json  unusable: shadowed by '.../tests/fixtures/configs/cli/providers/old-runtime.provider.json', which comes first on the provider search path; it is never used
```

With `--json`, each provider is `{"name", "version", "manifest", "library", "platform": {"os",
"arch"}, "services": [{"name", "major", "minor"}], "engine", "usable"}`, with `"problem"` for a
provider that is not usable. A run records each service it uses, with its provider's identity and
engine, in the run record, and they change the fingerprint
([specs/reproducibility.md](specs/reproducibility.md)).

A build with `-DPRAENORA_WITH_ROADRUNNER=ON` ([platforms.md](platforms.md#the-roadrunner-provider))
holds the provider `roadrunner`, in its build tree and in its installation, where the tool finds it
without a provider path. It offers `praenora.simulation@0.0` for models in the format `sbml`, the
text of an SBML document of Level 1, 2 or 3; what it accepts and refuses is in
[specs/simulation-service.md](specs/simulation-service.md#the-roadrunner-provider).

A build with `-DPRAENORA_WITH_ANTIMONY=ON` ([platforms.md](platforms.md#the-antimony-provider))
holds the provider `antimony` in the same way. It offers `praenora.model-conversion@0.0`, which
translates a model in the format `antimony`, Antimony text, to `sbml` and back; what it accepts and
refuses is in
[specs/model-conversion-service.md](specs/model-conversion-service.md#the-antimony-provider). A
module that simulates an Antimony model converts it with `antimony`, then simulates the SBML with
`roadrunner`. In a build with both providers (the local preset `dev-engines`,
[developer-guide.md](developer-guide.md#the-antimony-provider)), the tool lists both without a
provider path:

```bash
./build/dev-engines/bin/praenora providers list
```

```text
antimony    0.2.0  macos-arm64  praenora.model-conversion@0.0  .../build/dev-engines/lib/praenora/providers/antimony/antimony.provider.json      usable
roadrunner  0.2.0  macos-arm64  praenora.simulation@0.0        .../build/dev-engines/lib/praenora/providers/roadrunner/roadrunner.provider.json  usable
```

A build with `-DPRAENORA_WITH_PYTHON_ENVIRONMENT=ON` holds the provider `python` in its build tree
and in its installation, with the Python environment it runs
([The Python environment in an installation](#the-python-environment-in-an-installation)), where
the tool finds it without a provider path. It offers
`praenora.python@0.0`, objects of the runtime's Python environment called with JSON documents
([specs/python-service.md](specs/python-service.md)). Its engine names the interpreter and the
environment (package and lock), so a run's fingerprint names the environment it used. With the
local preset `dev-python`:

```bash
./build/dev-python/bin/praenora providers list
```

```text
python  0.2.0  macos-arm64  praenora.python@0.0  .../build/dev-python/lib/praenora/providers/python/python.provider.json  usable
```

The provider loads the environment's Python interpreter into the process when it starts, and a
process holds one interpreter: in a process that runs another one, such as a Python interpreter
that imported `praenora.runtime`, its start fails (`LIBRARY_LOAD_FAILED`, with a cause of status
`UNSUPPORTED` that says so), and such a step runs in a module host instead (`isolation: process`,
[Process isolation](#process-isolation)). A start fails too, saying which file, when the provider's
environment file (`python-environment.json`, beside its library), the environment it names or its
Python core is missing or of another build
([specs/python-service.md](specs/python-service.md#loading-and-starting-the-core)).

**Python steps.** A step bound to a Python module
([module-developer-guide.md](module-developer-guide.md#modules-in-python)) runs like any other,
given a provider of `praenora.python@0`: without one, validation refuses it with
`SERVICE_UNAVAILABLE` at `/requires/services/0` of the module's manifest. What the module prints,
and what it logs with `logging`, become log records of its step, and nothing reaches the tool's
standard output; what a child process of the module prints goes to the tool's standard error. An
exception fails the step with the module's error `INTERNAL`, whose details hold the traceback
(`praenora run --json` shows it in `error.details.cause.details.traceback`). Run Python steps in
module hosts (`isolation: { mode: process }`, [Process isolation](#process-isolation)): a crash then
fails only its step, a call that never returns is ended at its deadline, and each step imports its
module's code in a process of its own, so two modules that ship a package of one name can share a
run. A Python step that simulates with the environment's `roadrunner` package and a step that the
roadrunner provider serves each bring a libRoadRunner and an LLVM of their own; they run in one
process too (verified on macOS arm64, where the run loads the provider before the Python step
imports the package; not yet verified on Linux and Windows), and module hosts keep them apart. In
the runtime's process, a call that ignores its deadline is abandoned and keeps running until the
process exits; `praenora run` exits all the same, at the deadline plus the
cancellation grace, with `MODULE_UNRESPONSIVE`. A module's Python sources changed after it was built
are refused (`STEP_FAILED`, the module's error `DATA_INVALID`): build the module again.

**The Python environment's variables.** The packages of the environment, and the Python processes
that its code starts with `subprocess` and the process's environment, without `-I` or `-E`, read
some variables of the process's environment: each directory variable the environment's manifest
lists (`environment.paths`, but `PYTHONHOME`), and `PYTHONHASHSEED` (the children of
`multiprocessing` run isolated and read none of them). The `praenora` tool and the module host set
them first thing, each only when it is unset, when their tree has the provider `python`; a value
you set stays. A program that embeds the runtime, through the C++ API or the Python package, sets
them itself before it starts threads, to the absolute paths below the environment's directory and
`PYTHONHASHSEED` to `0`; otherwise the provider's start logs a warning for each that is unset or
differs, and the run goes on
([specs/python-service.md](specs/python-service.md#the-processs-environment)). A step of such a
program that runs in a module host gets them all the same: the module host sets them.

### The Python environment in an installation

The installation of a build with `PRAENORA_WITH_PYTHON_ENVIRONMENT=ON` holds, below its prefix
(with `lib` the library directory, or `lib64` and the like where the build named another):

| Path | Content |
|---|---|
| `lib/praenora/python-environment/` | The Python environment the build used, unchanged: its manifest `praenora-runtime-pyenv.json`, the interpreter and its packages in `python/`, its license files in `licenses/` and its CMake package. About a gigabyte. |
| `lib/praenora/providers/python/` | The provider `python`: its library and manifest, the Python core (the one library that embeds the interpreter), the environment file `python-environment.json`, which names the environment and the core relative to itself (`../../python-environment`), and `python/praenora/sdk/`, the package `praenora.sdk` that Python modules import. |
| `share/praenora/licenses/python-environment/` | A copy of the environment's license files. |

Nothing there names a directory outside the installation: the core finds the environment's
interpreter library relative to itself, and the tool and the module host find the provider's
environment file below the installation's library directory, so the installation can be moved,
also to a directory whose name holds spaces or characters that are not ASCII. A spawn pool that a
module opens starts its children with the environment's interpreter, `python/bin/python3.12`, of
the installation where it now is (on Windows `python/python.exe`, not yet verified). The installed
tool lists the provider without a provider path:

```bash
cmake --install build/dev-python --prefix "${TMPDIR:-/tmp}/praenora-python"
"${TMPDIR:-/tmp}/praenora-python/bin/praenora" providers list
```

```text
python  0.2.0  macos-arm64  praenora.python@0.0  .../praenora-python/lib/praenora/providers/python/python.provider.json  usable
```

An archive of such a build, made by the target `package`
([developer-guide.md](developer-guide.md#archiving-an-installation)), holds the same. A Python
module installed elsewhere, with `praenora_add_python_module()`'s `INSTALL_DESTINATION`, keeps its
sources in `<name>.python/` beside its manifest and library, and the installed tool runs it with
the installation's provider and environment
([module-developer-guide.md](module-developer-guide.md#modules-in-python)).

**Long paths on Windows.** The environment's deepest files lie about 140 characters below its
directory, so an installation in a deep directory holds paths longer than the 260 characters that
Windows allows by default. `praenora.exe` declares in its application manifest that it handles
long paths; Windows honors that only where long paths are enabled, with the setting
`LongPathsEnabled` (`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`, a DWORD of 1) or the group
policy "Enable Win32 long paths". Where they are not, install into a short directory, such as
`C:\praenora` (not yet verified on Windows).

## What a run writes

Without an output directory (`--output-dir`, or `outputs.directory` in the configuration) a run
writes nothing to disk. With one, it writes:

| File | Content |
|---|---|
| `run-record.json` | The run record: the `reproducibility` section that the fingerprint hashes (versions, workflow and catalog digests, time base, seed, ticks, timeouts, input digests, the runtime services of providers with each provider's identity, digests and engine, and per step the module, its manifest and library digests, parameters, seed and the services it may use), and the `execution` section (start and finish times, status, ticks completed, the error of a failed run (naming the run's directories by placeholders such as `<output>` and `<home>`), overrides, platform, where each module and provider manifest was found (relative to its search path, never as an absolute path), where each step's module instance ran, whether it shared compiled models, and its sandbox profile (`isolation`), each output file's digest and message count, the final status snapshot). It is written when the run starts and rewritten when it ends. |
| `outputs/<output>.jsonl` | One envelope per line for each recorded workflow output (`outputs.record`, by default all of them). |
| `trace/messages.jsonl` | With `--trace` (or `outputs.trace_messages: true`): every envelope into and out of a step, as `{"tick", "step", "direction", "port", "envelope"}`, plus the result's `diagnostics` when it has them, and the runtime services each call into a step's module looked up, as `{"tick", "step", "phase", "service", "major", "provided", "count"}`. |
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

A status file (`--status-file`, or `observability.status_file`; `--status-file ""` keeps none, in
place of the configuration's) holds a status snapshot that the run rewrites atomically at each
state change and every `status_interval` (1 s by default), so another process can read it at any
time:

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
it starts, `unhealthy` when the run failed or an instance failed, never returned or crashed (its
module host ended), `degraded` when
a timeout, cancellation or failure was counted without failing the run, when the run is stopping or
stopped, or when the status file could not be written, and `healthy` otherwise. With `--json`,
`praenora status` prints `{"ok": true, "result": <the snapshot>}`, as every command wraps its
result; the snapshot's fields are specified in
[specs/observability.md](specs/observability.md#status-snapshot).

The status file and the run record are written for others to read, so their error records name the
directories the run knows by placeholders: `<output>`, `<configuration>`, `<modules:caller:0>` and
the other search paths, and `<home>`. `praenora status` then shows, for example, the last error
`IO_ERROR: cannot create directory '<output>/outputs': File exists`. The tool's own error output and
the logs keep the absolute paths. A program that runs the workflow itself gets the snapshot in
either form: `Workflow::status_json(PathForm::placeholders)` in C++ and
`Workflow.status(path_form="placeholders")` in Python give the status file's, `status_json()` and
`status()` the absolute paths. The placeholders and the rules are in
[specs/observability.md](specs/observability.md#known-directories-in-the-files-of-a-run).

## Stopping a run

Ctrl+C or SIGTERM asks a run to stop: the module call in progress is asked to cancel, no further
call starts, the modules are finalized and destroyed, the run record is written with status
`stopped`, and the tool exits with 130. A second signal ends the tool at once. A tool started with
SIGINT ignored, as a shell starts a background command, keeps ignoring it.

## Watching and running over HTTP

The control API lets another process watch a run and stop it, over a Unix socket or a TCP
address; `praenora serve` also opens runs on request. Its routes, rules and answers are specified
in [specs/control-api.md](specs/control-api.md).

`praenora run --listen ADDR` serves the one run of the process, as run `1`, from before it starts
until it is over; then the tool prints its summary and exits as it would without a listener:

```bash
rm -f "$RUN/address"
./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --listen "unix:$HOME/.praenora.sock" \
    --address-file "$RUN/address" &
while [ ! -s "$RUN/address" ]; do sleep 0.1; done
curl --unix-socket "$HOME/.praenora.sock" http://localhost/v0/runs/1
curl --unix-socket "$HOME/.praenora.sock" -X POST -H 'Content-Type: application/json' \
    http://localhost/v0/runs/1/stop
```

`praenora serve` runs until SIGINT or SIGTERM (Ctrl+C) and opens a run for each `POST /v0/runs`,
from a configuration under its configuration root; run `k` writes to `<output-root>/run-<k>/` and
logs to `<output-root>/run-<k>.log`:

```bash
rm -f "$RUN/address"
./build/dev/bin/praenora serve --listen 127.0.0.1:0 --address-file "$RUN/address" \
    --config-root examples/workflows/estimate-predict --output-root "$RUN/served" \
    --module-path build/dev/modules --token-file "$HOME/.praenora.token" &
while [ ! -s "$RUN/address" ]; do sleep 0.1; done
./build/dev/bin/praenora status --server "$(cat "$RUN/address")" \
    --token-file "$HOME/.praenora.token"
```

- **Addresses.** `unix:<path>` (POSIX), `<IPv4>:<port>` or `[<IPv6>]:<port>`; port 0 lets the
  system choose. Host names, `localhost` included, are refused, and so is an IPv4-mapped IPv6
  address such as `[::ffff:127.0.0.1]:8080` (give `127.0.0.1:8080`). The tool prints
  `praenora: listening on <address>` with the real port, and `--address-file F` writes the address
  to `F` before the first request is answered, so a script can wait for that file (remove it
  first: the tool leaves it in place when it ends).
- **Who may do what.** A Unix socket is created with mode 0600: whoever may connect may also
  stop and open runs. Over TCP without tokens, only loopback addresses are allowed and only the
  queries are answered; `--token-file F` (both scopes) and `--read-token-file F` (queries only)
  name files that hold a token of at least 32 characters on their first line, readable by their
  owner only (`chmod 600`), which clients send as `Authorization: Bearer <token>`. A TCP request's
  `Host` header must name the listener's address, `localhost` on a loopback listener, or a name
  given with `--allow-host`.
- **Socket files.** A path that exists is refused, unless it is a socket that nobody listens on,
  as a crashed server leaves it, which is replaced; the tool removes its socket when it stops
  listening.
- **Shutting down.** On SIGINT or SIGTERM, `serve` stops listening, asks every run to stop, waits
  for each one to end (a run in a call that does not return ends at its own deadline, or when its
  module host is killed) and exits with 0. A second signal ends it at once with 130, leaving its
  socket file behind, which the next server replaces.
- **Isolation.** In serve mode a crash in an in-process module ends every run of the server:
  `--isolation process` (POSIX) is recommended.
- **Errors.** A wrong address, token file or `--allow-host` is a usage error (exit 2); a listener
  the system refuses (an address in use, a path that is not a socket) is `IO_ERROR` (exit 74).
  `praenora status --server` reports the server's errors with their own codes and exit codes, and
  a connection that fails as `IO_ERROR` (exit 74).

No listener and no request changes a run: its outputs, run record and fingerprint are those of the
same configuration and seed run without one.

## Logging

Records go to standard error, and also to a file with `--log-file`. The level, format and file are
set by the command's options (`--log-level`, `--log-format`, `--log-file`), the environment or the
configuration's `observability` section; which source wins is specified in
[specs/observability.md](specs/observability.md#settings). `--log-format json` writes one JSON
object per record, with OpenTelemetry field names:

```text
{"timestamp":"2026-11-11T18:34:17.178992000Z","severity_text":"INFO","severity_number":9,"body":"run started","attributes":{"logger":"praenora.engine","praenora.run.fingerprint":"a4d251a5be737c49fe7cc73b30aa65367f3bc8e55cb2818ebb12bbafed93f46d","praenora.event":"run.started","praenora.workflow":"example.estimate-predict@0.1.0"}}
```

A step whose configuration sets its own `log_level` keeps that level whatever the run's level: the
example's `predict` step logs at `debug` even with `--log-level warn`. Logging never changes the
fingerprint. The loggers, events and formats are in
[specs/observability.md](specs/observability.md#logs). Every attribute key of a record carries the
prefix `praenora.`, except `logger`.

**Exporting to an OpenTelemetry collector.** With an endpoint in the environment, the tool also
sends its log records and metrics to a collector over OTLP/HTTP with JSON, whatever it logs to
standard error or a file:

```bash
OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318 OTEL_SERVICE_NAME=forecast \
    ./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules
```

Metrics go to `<endpoint>/v1/metrics` every minute (`OTEL_METRIC_EXPORT_INTERVAL`, in
milliseconds) and when the run ends, log records to `<endpoint>/v1/logs` within a second; each
carries the run's number and fingerprint (`praenora.run.number`, `praenora.run.fingerprint`). Only
`http://` endpoints and the protocol `http/json` are supported: a value the export cannot use is
ignored with a warning (`telemetry.setting_ignored`), and the run goes on. A collector that is
down or refuses the export does not change the run, its health or its exit code: the failure is
logged as a warning (`telemetry.export_failed`) at most once a minute, and the tool waits for the
export at its end at most about the export timeout (10 s by default, `OTEL_EXPORTER_OTLP_TIMEOUT`).
`OTEL_SDK_DISABLED=true` turns it off. Every variable, and what is exported, is in
[specs/observability.md](specs/observability.md#otlp-export).

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
step, module and `details` (for example the service, the search paths and the providers a
`SERVICE_UNAVAILABLE` considered). `run` and `graph` report an invalid configuration the same way.

| Symptom | Likely cause and fix |
|---|---|
| `MODULE_NOT_FOUND`, exit 3 | No search path holds the module at that version. The message lists the searched paths and the versions found; add `--module-path` or set `PRAENORA_MODULE_PATH`, and check with `praenora modules list`. |
| `PLATFORM_MISMATCH` | Only manifests for other platforms were found: build or install the module for this one. |
| `SERVICE_UNAVAILABLE`, exit 3 | No usable provider offers a service that a module requires, at the major and at least the minor it asks for. The diagnostic's details list the provider search paths and each provider that offers the service's name but cannot serve it, with the reason; add `--provider-path` or set `PRAENORA_PROVIDER_PATH`, and check with `praenora providers list`. |
| `PARAMETERS_INVALID` | A step's parameter breaks the module's parameter schema; `praenora modules inspect <name>@<version>` shows the module, and its manifest holds the schema. |
| `SCHEMA_VIOLATION` | A misspelled key or a value of the wrong type, at the JSON pointer given. |
| `TARGET_DATA_INVALID` | The input data: the message names the file and line ([configuration.md](configuration.md#observation-csv)); with a target descriptor, also a row that the descriptor contradicts or a descriptor that is itself wrong ([configuration.md](configuration.md#target-descriptor)). With severity warning it is a finding about data that is still used, for example rows that are never delivered. |
| `TARGET_DATA_UNAVAILABLE` | The input's `path` or `descriptor`: the file does not exist, is not a file or cannot be read. |
| `IO_ERROR`, exit 74 | A missing or unreadable file or directory (the configuration, a manifest, a catalog directory), a file that cannot be written, or an output directory with an earlier run (add `--overwrite`). A missing data file is `TARGET_DATA_UNAVAILABLE` instead (exit 3). |
| `LIBRARY_LOAD_FAILED`, exit 4 | The operating system could not load the module library, for example because a library it needs is missing; the message holds the system's text. For a service provider, located in its manifest, `details.reason` names the check that failed: `start` (the provider's own error in `details.cause`), `identity` (a stale provider manifest), `service`, `conflict` or `library`. |
| `STEP_FAILED`, exit 1 | A module returned an error. Its own code and message are in the error's `cause`; its log records are on the logger `module.<step>`. |
| `STEP_TIMEOUT`, `MODULE_UNRESPONSIVE`, exit 5 | A call ran past its deadline (`run.timeouts`, `steps.<step>.timeouts`); an unresponsive module ignored the cancellation and never returned. While such a call may still be running, the process refuses the calls and loads of its library that would have to wait for it (all of them for `features.concurrency: "module"`) with `MODULE_UNRESPONSIVE` too, naming the call in `details.abandoned_call`. |
| `MODULE_CRASHED`, exit 1 | The module host of a step in process mode ended while it loaded the module or during a call: `details` say how (`signal` and `signal_name`, `exit_code`, or `reason` `protocol`, or `ended` when the system cannot say) and in which `phase`. Look at the module; the operating system's crash report names the module host. |
| `MODULE_HOST_UNAVAILABLE`, exit 4 | No module host could be started for a step in process mode: `details.reason` says why (`missing`: no `libexec/praenora/praenora-module-host` beside the runtime library, as in an installation from the Python wheel; `spawn`, `handshake`, `build`; `sandbox`: the restricted profile cannot be applied, as when the runtime itself runs in a sandbox). Run the step in process mode from a build tree or a full installation, or set it to `in-process`; run a restricted step outside any other sandbox, or set its `sandbox` to `none`. |
| `UNSUPPORTED`, exit 3 | A step in process mode on Windows: set its `isolation.mode` to `in-process`. A step with `sandbox: restricted` anywhere but on macOS (located at its `isolation.sandbox`): set it to `none`. |
| `RUN_STOPPED`, exit 130 | The run was stopped on request. |
| `cannot listen on <address>`, `IO_ERROR`, exit 74 | `run --listen` or `serve` could not open its listener: the message gives the system's error (`bind()`: an address in use, or listeners refused, as some sandboxes refuse them all), or says that a Unix socket's path is a file that is not a socket, or a socket that another process listens on. |
| `UNAUTHENTICATED`, `PERMISSION_DENIED`, exit 2 | `praenora status --server` (or another client) sent no valid token (`--token-file`), a read token where control is needed, or, over TCP, a `Host` header that names neither the listener's address nor a name of `--allow-host` ([Watching and running over HTTP](#watching-and-running-over-http)). |

The [error taxonomy's triage table](specs/error-taxonomy.md#triage) says where to look for every
code. For problems with a module itself, `praenora modules check <manifest>` runs its contract
checks and names the check that fails.
