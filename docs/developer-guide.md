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
| cpp-httplib, header-only | 0.67 (0.67.x only) | the runtime's OTLP export, the control API, and the tests that open local listeners ([platforms.md](platforms.md#cpp-httplib)) |
| GoogleTest | 1.11 | the tests |
| SWIG | 4.0 | the Python bindings |
| Python, with its headers | 3.10 | the Python bindings |
| libRoadRunner (C++, `roadrunner-static`) and the LLVM it was built with | 2.11 | the roadrunner provider, with `PRAENORA_WITH_ROADRUNNER` only ([platforms.md](platforms.md#the-roadrunner-provider)) |
| libAntimony (`antimony-static`, built from source) and the libSBML, SBMLNetwork and expat it was built against | 3.3 | the antimony provider, with `PRAENORA_WITH_ANTIMONY` only ([platforms.md](platforms.md#the-antimony-provider)) |

The third-party libraries link privately into the runtime, so neither the installed headers nor the
installed CMake package mention them. The commands that install them on each platform are in
[platforms.md](platforms.md); json-schema-validator, which Homebrew, apt and dnf do not package, is
built from source as [platforms.md](platforms.md#json-schema-validator) shows, and cpp-httplib is
installed header-only into a local prefix as [platforms.md](platforms.md#cpp-httplib) shows (a
package whose header is compiled into a library is refused).

**Offline by default.** A default configuration uses installed packages only and never touches the
network. With `PRAENORA_FETCH_DEPENDENCIES=ON`, CMake's FetchContent still tries `find_package`
first and downloads only what is missing, at the commits pinned in
[`cmake/PraenoraDependencies.cmake`](../cmake/PraenoraDependencies.cmake)
([ADR-0017](adr/ADR-0017-dependency-management.md)):

```bash
cmake --preset dev -DPRAENORA_FETCH_DEPENDENCIES=ON
```

With every package installed, as where this was run, that configuration finds them all and downloads
nothing. An actual download is verified by CI for yaml-cpp, spdlog and GoogleTest, which the CI
presets download ([CI presets](#ci-presets)), and not yet for nlohmann/json, json-schema-validator
and cpp-httplib (whose fetched sources were checked from a local copy of the pinned commit,
[platforms.md](platforms.md#cpp-httplib)). `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON` is the
one option that builds a dependency even when it is installed: json-schema-validator, from its
pinned sources, downloaded unless a local copy is named, so that sanitizer builds instrument it
([Sanitizer builds](#sanitizer-builds)). Configuring prints one line per dependency, saying where it
came from:

```text
-- Praenora: dependency nlohmann_json 3.12.0 (found at /opt/homebrew/share/cmake/nlohmann_json)
-- Praenora: dependency yaml-cpp 0.9.0 (found at /opt/homebrew/lib/cmake/yaml-cpp)
-- Praenora: dependency cpp-httplib 0.67.0 (found at <prefix>/lib/cmake/httplib; header-only)
```

## Local presets

[`CMakePresets.json`](../CMakePresets.json) holds the project presets: `linux-x64-debug`,
`linux-x64-release`, `linux-x64-asan`, `linux-x64-tsan`, `macos-arm64-debug`, `macos-arm64-release`,
`macos-arm64-asan`, `macos-arm64-tsan`, `windows-x64-debug` and `windows-x64-release`, each with a
build and a test preset, and the presets of the continuous-integration jobs, which build against a
dependency bundle ([CI presets](#ci-presets)). Each preset is available only on its host system.
Build trees go to `build/<preset>`, installations to `install/<preset>`.

Where your dependencies live is local knowledge, so it goes into `CMakeUserPresets.json`, which git
ignores. Create it from the example:

```bash
cp CMakeUserPresets.json.example CMakeUserPresets.json
```

Then edit its hidden `dev-local` preset:

| Cache variable | Set it to |
|---|---|
| `CMAKE_PREFIX_PATH` | Your dependency prefixes, separated by `;` (for example Homebrew's prefix and the prefixes of json-schema-validator and cpp-httplib). |
| `httplib_DIR` | Only instead of cpp-httplib's prefix in `CMAKE_PREFIX_PATH`: its `<prefix>/lib/cmake/httplib`, so that the prefix's other packages are not found there. |
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

## Using a dependency bundle

The repository [praenora-runtime-deps](https://github.com/INFLECT-Praenora/praenora-runtime-deps)
builds the dependencies that no package manager offers ready to use into a bundle, one per
platform: json-schema-validator with the nlohmann/json it is compiled against, libRoadRunner with
its dependencies, the LLVM it was built with, and libAntimony, with their license files and a
manifest, `praenora-runtime-deps.json`. Its releases publish each bundle as an archive.
[`cmake/PraenoraRuntimeDepsPin.cmake`](../cmake/PraenoraRuntimeDepsPin.cmake) pins the release
that this repository builds against and, for each platform key of
`PRAENORA_RUNTIME_DEPS_PLATFORMS` (such as `macos-arm64-release`), the archive's name and SHA-256.
The archive's name gives the bundle's version, platform, compiler and build type; for example, for
0.1.1 and `macos-arm64-release`:
`praenora-runtime-deps-0.1.1-macos-arm64-appleclang21-release.tar.xz`.

The script [`cmake/praenora_fetch_runtime_deps.cmake`](../cmake/praenora_fetch_runtime_deps.cmake),
which needs only CMake, downloads one platform's archive from the release into a directory of your
choice, checks it against the pinned SHA-256, unpacks it there and checks that the bundle's
manifest names the pinned version. A transfer that fails, or stalls for 120 seconds, is tried
again, up to three attempts in all; an archive with another SHA-256 is not:

```bash
cmake -DPLATFORM=macos-arm64-release -DDESTINATION="$HOME/praenora-runtime-deps" \
    -P cmake/praenora_fetch_runtime_deps.cmake
```

It prints one line to standard output, which names the bundle's directory, an absolute path
(here, for 0.1.1):

```text
PRAENORA_RUNTIME_DEPS=<DESTINATION>/praenora-runtime-deps-0.1.1-macos-arm64-appleclang21-release
```

Its other messages go to standard error, so a Linux or macOS shell sets the variable with
`export "$(cmake <the same arguments>)"` (on Windows the line may end in a carriage return; not yet
verified). When the environment variable `GITHUB_ENV` names a file, as in a step of a GitHub
Actions job, the script also appends the line to that file, so that the job's later steps have the
variable. A relative `DESTINATION` is relative to the current directory. The archive stays in
`DESTINATION`, and a later run there uses it again when it still has the pinned SHA-256 (otherwise
it downloads it again), and the bundle when the archive matched and the manifest names the pinned
version (otherwise it unpacks it again). What a stopped run leaves (a partial download, a staging
directory `.u-<id>/`) is removed by the next run. `-DBASE_URL=<url>` downloads `<url>/<archive>`
instead of the archive in the release, for a mirror, and `-DPIN_FILE=<file>` reads another pin
file. The test `ci.fetch_runtime_deps` ([testing.md](testing.md#labels)) runs the script on local
archives; a download from the release is verified by CI, whose jobs download their bundles with it
([releasing.md](releasing.md#what-the-runs-verified)).

**The bundle's compiler.** A bundle is built with one compiler, which its name gives (such as
`appleclang21`; the manifest's `compiler` member gives its full version). Build the runtime with
that compiler or a newer one of the same family. The macOS bundle of 0.1.1 is built with
AppleClang 21, so with an older Xcode, build the bundle locally from a clone of
praenora-runtime-deps instead (its `docs/building.md` says how) and use the directory it installs
(`install/<preset>` of that clone) as the bundle below. On macOS, `CMAKE_OSX_DEPLOYMENT_TARGET`
must also be at least the bundle's (the manifest's `platform.os_min_version`, 14.0 for 0.1.1).

**Local presets.** With `<bundle>` the bundle's directory, the hidden presets of
`CMakeUserPresets.json` ([Local presets](#local-presets)) list the bundle's directories as
praenora-runtime-deps' `docs/outputs.md` gives them ("The Praenora runtime's local presets"). The
runtime must compile against the nlohmann/json that json-schema-validator was compiled against, so
`<bundle>/json-schema-validator` comes first, before any prefix that holds an nlohmann/json of its
own, such as Homebrew's `/opt/homebrew`, which stands here for the prefixes of the other
dependencies:

```json
{
  "name": "dev-local",
  "hidden": true,
  "cacheVariables": {
    "CMAKE_PREFIX_PATH": "<bundle>/json-schema-validator;/opt/homebrew",
    "PRAENORA_THIRD_PARTY_LICENSE_DIRS": "<bundle>/licenses"
  }
},
{
  "name": "dev-engines-local",
  "hidden": true,
  "cacheVariables": {
    "CMAKE_PREFIX_PATH": "<bundle>/json-schema-validator;/opt/homebrew;<bundle>/roadrunner;<bundle>/roadrunner/lib/cmake;<bundle>/roadrunner/lib/cmake/Poco;<bundle>/llvm;<bundle>/antimony",
    "LLVM_INSTALL_PREFIX": "<bundle>/llvm",
    "PRAENORA_WITH_ROADRUNNER": "ON",
    "PRAENORA_WITH_ANTIMONY": "ON"
  }
}
```

Their other cache variables stay as [Local presets](#local-presets) gives them.
`PRAENORA_THIRD_PARTY_LICENSE_DIRS` brings the bundle's license files into installations
([License files](#license-files)). The presets for one provider take part of the list after
`<bundle>/json-schema-validator;/opt/homebrew`: `dev-roadrunner-local` the `roadrunner`
directories and `<bundle>/llvm`, with `LLVM_INSTALL_PREFIX`
([The roadrunner provider](#the-roadrunner-provider)), and `dev-antimony-local`
`<bundle>/roadrunner`, `<bundle>/roadrunner/lib/cmake` and `<bundle>/antimony`
([The antimony provider](#the-antimony-provider)). On Windows the list also holds
`<bundle>/roadrunner/cmake` (not yet verified in a `dev-local` preset; the Windows CI presets'
list holds it too); the manifest's `consumer.cmake_prefix_path` lists
each platform's directories in their order. On macOS these are the directories of the presets
with which this guide was checked, against a bundle built locally from praenora-runtime-deps.

## CI presets

[`CMakePresets.json`](../CMakePresets.json) holds a preset for each platform key of the dependency
bundles, for continuous-integration jobs: `ci-linux-x64-release`,
`ci-macos-arm64-release`, `ci-windows-x64-release` and `ci-windows-x64-debug`, each with a build
and a test preset of the same name. The jobs of the workflow `.github/workflows/build.yml` run
them, one per key, and archive the installation ([releasing.md](releasing.md#the-workflow)). Each
inherits the project preset of its key (`linux-x64-release`, and so on) and the hidden preset `ci`
(`ci-windows` on Windows, which inherits `ci`), and builds into `build/ci-<key>`. They take the
dependency bundle's directory from the environment variable `PRAENORA_RUNTIME_DEPS`, which the
fetch script prints ([Using a dependency bundle](#using-a-dependency-bundle)), and the Python
environment bundle's from `PRAENORA_RUNTIME_PYENV` (below), and set these cache variables:

| Cache variable | Value |
|---|---|
| `PRAENORA_RUNTIME_DEPS` | `$env{PRAENORA_RUNTIME_DEPS}`, which the configuration checks (below). |
| `PRAENORA_RUNTIME_PYENV` | `$env{PRAENORA_RUNTIME_PYENV}`, which the configuration checks too. |
| `PraenoraRuntimePyenv_DIR` | `$env{PRAENORA_RUNTIME_PYENV}/lib/cmake/PraenoraRuntimePyenv`, the Python environment's CMake package. |
| `CMAKE_PREFIX_PATH` | The bundle's `json-schema-validator`, `roadrunner`, `roadrunner/lib/cmake`, `roadrunner/lib/cmake/Poco`, `llvm` and `antimony`, in the order of the manifest's `consumer.cmake_prefix_path`; on Windows also `roadrunner/cmake`, after `roadrunner/lib/cmake/Poco`. |
| `LLVM_INSTALL_PREFIX` | The bundle's `llvm`. |
| `PRAENORA_THIRD_PARTY_LICENSE_DIRS` | The bundle's `licenses` ([License files](#license-files)). |
| `PRAENORA_WITH_ROADRUNNER`, `PRAENORA_WITH_ANTIMONY`, `PRAENORA_WITH_PYTHON_ENVIRONMENT`, `PRAENORA_BUILD_PYTHON`, `PRAENORA_WARNINGS_AS_ERRORS`, `PRAENORA_FETCH_DEPENDENCIES` | `ON` |
| `CMAKE_DISABLE_FIND_PACKAGE_yaml-cpp`, `CMAKE_DISABLE_FIND_PACKAGE_spdlog`, `CMAKE_DISABLE_FIND_PACKAGE_googletest`, `CMAKE_DISABLE_FIND_PACKAGE_httplib` | `ON` |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | `ci-macos-arm64-release` only: `14.0`, the macOS bundles'. |
| `CMAKE_TRY_COMPILE_CONFIGURATION` | `ci-windows-x64-*` only: `Release` or `Debug`, the configuration of the bundle. The antimony provider's probe links the bundle's libraries when the build is configured, and the Visual Studio generator builds such a program in Debug unless this says otherwise, with another MSVC runtime library than a Release bundle's. |

A tree of these presets builds both providers, the provider `python` with the Python modules of the
tests and the example, the Python bindings and the tests, with warnings as errors. nlohmann/json
and json-schema-validator come from the dependency bundle, which is first on `CMAKE_PREFIX_PATH`;
the Python environment, from its bundle's CMake package, which `PraenoraRuntimePyenv_DIR` names
(the Python bindings are still built for `Python3_EXECUTABLE`, not for the environment's
interpreter). yaml-cpp, spdlog, GoogleTest and cpp-httplib (header-only) are built from the
commits that [`cmake/PraenoraDependencies.cmake`](../cmake/PraenoraDependencies.cmake) pins,
whatever is installed, so that what a job builds does not depend on what its runner has installed:
FetchContent first looks for an installed package with `find_package()` under the dependency's
content name (`googletest` for GoogleTest; `CMAKE_DISABLE_FIND_PACKAGE_GTest` would have no
effect), and the four `CMAKE_DISABLE_FIND_PACKAGE_<name>` variables turn those calls off. Their
status lines then read `fetched into` the tree's `_deps/<name>-src`, as the CI jobs' logs show. The
presets do not set `Python3_EXECUTABLE`: pass `-DPython3_EXECUTABLE=<python>` on the configure
line, or FindPython takes the first suitable interpreter it finds.

**The bundle check.** When the cache variable `PRAENORA_RUNTIME_DEPS` is defined, as these presets
define it, the configuration stops before any dependency is looked up unless it is the absolute path
of a directory that holds `praenora-runtime-deps.json`. With the environment variable unset:

```text
PRAENORA_RUNTIME_DEPS ('') is not a dependency bundle: set the environment variable
PRAENORA_RUNTIME_DEPS to a bundle directory, for example with cmake -P
cmake/praenora_fetch_runtime_deps.cmake (docs/developer-guide.md).
```

A tree configured with other presets does not define the variable and is not checked, whatever the
environment holds. `cmake --preset` reads the environment variable each time it runs, and passes
every value of the preset again.

The Python environment bundle is checked in the same place, before any dependency is looked up,
so that a job without it stops before anything is downloaded: with
`PRAENORA_WITH_PYTHON_ENVIRONMENT`, when the cache variable `PRAENORA_RUNTIME_PYENV` is defined, it
must be the absolute path of a directory that holds `praenora-runtime-pyenv.json`. With the
environment variable unset:

```text
PRAENORA_RUNTIME_PYENV ('') is not a Python environment bundle: set the
environment variable PRAENORA_RUNTIME_PYENV to a bundle directory, for
example with cmake -P cmake/praenora_fetch_runtime_pyenv.cmake
(docs/developer-guide.md).
```

A configuration of these presets that turns `PRAENORA_WITH_PYTHON_ENVIRONMENT` off needs no Python
environment bundle and does not check the variable.

**The Python environment bundle.** The repository
[praenora-runtime-pyenv](https://github.com/INFLECT-Praenora/praenora-runtime-pyenv) builds the
Python environment, one bundle per platform key, each with a manifest,
`praenora-runtime-pyenv.json`, and a CMake package, `lib/cmake/PraenoraRuntimePyenv`; its releases
publish each bundle as an archive.
[`cmake/PraenoraRuntimePyenvPin.cmake`](../cmake/PraenoraRuntimePyenvPin.cmake) pins the release
that CI builds against and, for each platform key, the archive's name and SHA-256; the environment
is built in Release only, so `windows-x64-debug` names the Windows Release archive
([releasing.md](releasing.md#the-python-environment-bundles)). The script
[`cmake/praenora_fetch_runtime_pyenv.cmake`](../cmake/praenora_fetch_runtime_pyenv.cmake) fetches
one, as the dependency bundle's script does, with the same arguments, behavior and errors (the two
share [`cmake/praenora_fetch_bundle.cmake`](../cmake/praenora_fetch_bundle.cmake)):

```bash
cmake -DPLATFORM=macos-arm64-release -DDESTINATION="$HOME/praenora-runtime-pyenv" \
    -P cmake/praenora_fetch_runtime_pyenv.cmake
```

It prints one line to standard output, which names the bundle's directory, an absolute path (here,
for 0.1.0), and appends it to the file that `GITHUB_ENV` names, when it names one:

```text
PRAENORA_RUNTIME_PYENV=<DESTINATION>/praenora-runtime-pyenv-0.1.0-macos-arm64-cp312-appleclang21-release
```

The test `ci.fetch_runtime_pyenv` ([testing.md](testing.md#labels)) runs the script on local
archives; a download from the release through the pin is not yet verified by CI. The environment's
CMake package refuses a build for another platform, a macOS deployment target older than the
bundle's (14.0 for 0.1.0) or an older MSVC toolset.

**Running one locally.** With a dependency bundle for your platform and compiler
([The bundle's compiler](#using-a-dependency-bundle)) and a Python environment bundle, on macOS:

```bash
export PRAENORA_RUNTIME_DEPS=<bundle>
export PRAENORA_RUNTIME_PYENV=<Python environment bundle>
cmake --preset ci-macos-arm64-release -DPython3_EXECUTABLE=<python>
cmake --build --preset ci-macos-arm64-release
ctest --preset ci-macos-arm64-release
```

As in a job, that configuration downloads yaml-cpp, spdlog, GoogleTest and cpp-httplib. To use
installed packages instead and download nothing, turn the four variables off on the configure line;
`-DFETCHCONTENT_FULLY_DISCONNECTED=ON` makes a package that is not found stop the configuration
instead of being downloaded. The presets' `CMAKE_PREFIX_PATH` names only the bundle, so the packages
are then searched in CMake's default places, such as the prefix of each directory on `PATH`
(`/opt/homebrew` for `/opt/homebrew/bin`), in the order of `PATH`; the environment variable
`CMAKE_PREFIX_PATH` is searched before those (after the bundle, so the bundle's
json-schema-validator still wins). Name there cpp-httplib's prefix too, such as
`$HOME/.local/opt/praenora-deps`, where [platforms.md](platforms.md#cpp-httplib) installs it.
Homebrew's libraries target the macOS release they were built on, so with them also raise the
deployment target to that release, or the linker warns about each of them
([Local presets](#local-presets)):

```bash
CMAKE_PREFIX_PATH=/opt/homebrew:$HOME/.local/opt/praenora-deps \
    cmake --preset ci-macos-arm64-release \
    -DPython3_EXECUTABLE=<python> -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=OFF -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_googletest=OFF -DCMAKE_DISABLE_FIND_PACKAGE_httplib=OFF \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
```

Give these values again whenever you run `cmake --preset` on that tree. On macOS arm64 the preset
was checked this way, against a bundle built locally from praenora-runtime-deps with AppleClang 17
and Homebrew's yaml-cpp, spdlog and GoogleTest: configure, build without warnings, and every test,
those of the restricted sandbox profile from a shell outside any sandbox
([testing.md](testing.md#module-hosts)); and again since cpp-httplib is a dependency, with
cpp-httplib 0.67.0 from a local header-only prefix: configure and build without warnings, and
every test from a shell outside any sandbox, which its test preset needs, since it requires
listeners (below). CI verified the four CI presets on their runners, with fetched yaml-cpp, spdlog,
GoogleTest and cpp-httplib ([releasing.md](releasing.md#what-the-runs-verified)). Since the
presets turn on `PRAENORA_WITH_PYTHON_ENVIRONMENT`, building the provider `python` against a
released Python environment bundle, `ci-macos-arm64-release` was configured with that variable
unset, where it stops with the message above, and configured, built and tested with installed
packages as above, but with the preset's deployment target, 14.0, from a clean tree, offline,
against the released macOS bundle 0.1.0 that `cmake/praenora_fetch_runtime_pyenv.cmake` downloaded
through the pin, named by `PRAENORA_RUNTIME_PYENV`: all its 1484 tests passed, none skipped, from a
shell outside any sandbox (with the test preset's `PRAENORA_TESTS_REQUIRE_LISTENERS=1`), with
Homebrew's yaml-cpp, spdlog and GoogleTest, a locally installed cpp-httplib and a locally built and
installed dependency bundle in place of fetched ones; with that deployment target, the build printed
98 linker notes that Homebrew's libraries were built for a newer macOS
([platforms.md](platforms.md#the-ci-presets)). CI has not yet run them with the environment.

**Their test presets.** The CI test presets set the environment variable
`PRAENORA_TESTS_REQUIRE_LISTENERS=1`: a test that opens a local listener then fails where the
listener is refused, instead of skipping ([testing.md](testing.md#listeners)), so a job's run never
passes without them. Every test preset of `CMakePresets.json` sets `OTEL_SDK_DISABLED=true`, so no
test exports telemetry to a collector that the caller's environment names; the tests of the OTLP
export set the variables they read themselves.

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
| `PRAENORA_BUILD_CLI` | ON | The `praenora` tool, and with it the control API's server (`praenora_server`, `src/server/`) and its tests. |
| `PRAENORA_BUILD_PYTHON` | OFF | The Python bindings ([python.md](python.md)). |
| `PRAENORA_FETCH_DEPENDENCIES` | OFF | Download missing dependencies at pinned commits. |
| `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR` | OFF | Build json-schema-validator from its pinned sources even when one is installed, so that a sanitizer build instruments it ([platforms.md](platforms.md#json-schema-validator)). |
| `PRAENORA_INSTALL` | ON at top level | The install rules (none in a Python wheel build). |
| `PRAENORA_INSTALL_EXAMPLES` | OFF | Install the example modules with every installation. |
| `PRAENORA_THIRD_PARTY_LICENSE_DIRS` | empty | Absolute paths of directories (a `;`-list) whose contents every installation holds in `share/praenora/licenses/`, such as the `licenses/` directory of a dependency bundle ([below](#license-files)). |
| `PRAENORA_WARNINGS_AS_ERRORS` | OFF | Turn compiler warnings into errors. |
| `PRAENORA_SANITIZERS` | empty | `address;undefined` or `thread`, for GCC and Clang. |
| `PRAENORA_WITH_ROADRUNNER` | OFF | The roadrunner service provider ([below](#the-roadrunner-provider)). |
| `LLVM_INSTALL_PREFIX` | empty | With `PRAENORA_WITH_ROADRUNNER`: the LLVM that libRoadRunner was built with. |
| `PRAENORA_WITH_ANTIMONY` | OFF | The antimony service provider ([below](#the-antimony-provider)). |
| `PRAENORA_ASYNCAPI_SCHEMA` | empty | With the tests: the absolute path of a local copy of the official AsyncAPI 3.0.0 JSON Schema, against which the `protocol` tests then check the committed AsyncAPI documents ([testing.md](testing.md#the-official-asyncapi-schema)). |
| `PRAENORA_ASYNCAPI_PARSER` | empty | With the tests: the absolute path of a directory into which the official AsyncAPI parser (`@asyncapi/parser` 3.6.6, from npm) is installed, with which the `protocol` tests then check the committed AsyncAPI documents, run by Node.js ([testing.md](testing.md#the-official-asyncapi-parser)). |
| `PRAENORA_NODE_EXECUTABLE` | searched with `find_program()` | With `PRAENORA_ASYNCAPI_PARSER`: the Node.js that runs the parser ([testing.md](testing.md#the-official-asyncapi-parser)). |
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
| `build/<preset>/libexec/praenora/` | `praenora-module-host`, the module host (POSIX), where the build tree's runtime finds it ([specs/process-isolation.md](specs/process-isolation.md#the-module-host)). |
| `build/<preset>/modules/<name>/` | The example modules: library, manifest and test vectors. |
| `build/<preset>/test-modules/<name>/` | The fixture modules of the tests. |
| `build/<preset>/test-providers/<name>/` | The fixture service providers of the tests. |
| `build/<preset>/lib/praenora/providers/roadrunner/` | The roadrunner provider, with `PRAENORA_WITH_ROADRUNNER`: where the build tree's runtime looks for the providers of its installation. |
| `build/<preset>/lib/praenora/providers/antimony/` | The antimony provider, with `PRAENORA_WITH_ANTIMONY`, beside it. |
| `build/<preset>/lib/praenora/providers/python/` | The provider `python`, with `PRAENORA_WITH_PYTHON_ENVIRONMENT`, beside them: its library and manifest, the Python core, the environment file `python-environment.json`, which names the environment the build uses by its absolute path, and the core's copy of `praenora.sdk` ([specs/python-service.md](specs/python-service.md#the-provider-python)). |
| `build/<preset>/test-python-modules/<name>/` | The Python fixture modules of the tests, and `build/<preset>/modules/example-predictor-python/` the example Python module, with the environment. |
| `build/<preset>/python/` | The importable Python package, with `PRAENORA_BUILD_PYTHON`. |
| `build/<preset>/include/` | Generated headers: `praenora/runtime/export.h`, `praenora/sdk/version.h`. |

An installation goes elsewhere with `--prefix`; the example modules come with the component
`Examples`:

```bash
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "${TMPDIR:-/tmp}/praenora-install" --component Examples
```

### The installation

An installation holds, below its prefix
([`cmake/PraenoraInstall.cmake`](../cmake/PraenoraInstall.cmake); `bin`, `lib`, `include` and
`share` stand for the GNUInstallDirs directories, `lib` being `lib64` or a multiarch directory on
some Linux systems):

| Path | Content |
|---|---|
| `bin/` | `praenora` (on Windows also `praenora_runtime.dll` and the DLLs it loads). |
| `lib/` | The runtime library (on Windows its import library), and `lib/cmake/Praenora/`, the CMake package ([module-developer-guide.md](module-developer-guide.md)). |
| `libexec/praenora/` | `praenora-module-host` (POSIX), beside `lib/`: `<lib>/praenora/../../libexec/praenora`. |
| `include/praenora/` | The public headers of the runtime and the SDK. |
| `share/praenora/schemas/`, `share/praenora/catalog/` | The schemas and the protocol catalog, as the runtime embeds them. |
| `share/praenora/licenses/` | The license files ([License files](#license-files)). |
| `lib/praenora/providers/<name>/` | Each service provider built in the tree, its library beside its manifest: `roadrunner`, `antimony`, and, with `PRAENORA_WITH_PYTHON_ENVIRONMENT`, `python`, whose directory holds the Python core, the environment file `python-environment.json` (`{"environment": "../../python-environment", "core": "<core's file name>"}`) and `python/praenora/sdk/` too. |
| `lib/praenora/python-environment/` | With `PRAENORA_WITH_PYTHON_ENVIRONMENT`, the Python environment the build used, unchanged: every file with its permissions, symbolic links as links. |
| `lib/praenora/modules/<name>/` | The example modules, with the component `Examples` (or `PRAENORA_INSTALL_EXAMPLES`): library, manifest and test vectors, and for `example-predictor-python` its sources in `example-predictor-python.python/`. |

Nothing installed names the build tree, the source tree or the Python environment the build used:
the CMake package, the manifests and the environment files name what they need relative to
themselves, the programs and the Python core find their libraries through RPATHs relative to
themselves (`@loader_path`, `$ORIGIN`; the install test accepts, in a sanitizer build on macOS, the
compiler's own sanitizer runtime directory too), and `praenora` and `praenora-module-host` find the
provider `python`'s environment file at a path relative to their own directory, compiled in from
the GNUInstallDirs directories, which names the installation's library directory (they look below
`lib/`, as a build tree has it, when nothing is there). So an installation can be moved; the test
`install.consumer` moves one to a directory whose name holds a space and a character that is not
ASCII, and runs Python steps from there ([testing.md](testing.md#labels)).

### License files

This repository's files are under the MIT license ([`LICENSE`](../LICENSE),
[ADR-0040](adr/ADR-0040-mit-license.md)). Third-party code that the build links statically keeps
its own license, so installations hold license files in `share/praenora/licenses/`
([`cmake/PraenoraInstall.cmake`](../cmake/PraenoraInstall.cmake)):

| Path below `share/praenora/licenses/` | Content |
|---|---|
| `praenora-runtime/LICENSE` | This repository's license. |
| `<name>/` | For each dependency built in the tree with FetchContent (`nlohmann_json`, `nlohmann_json_schema_validator`, `yaml-cpp`, `spdlog`, `httplib`, which is header-only and compiled into the runtime; never GoogleTest, which only the tests link), the files at the top of its sources whose names begin with `LICENSE`, `COPYING` or `NOTICE`, in any case, and, for spdlog, the license of the fmt library it bundles (a file whose name holds `license` in `include/spdlog/fmt/bundled/`), each under its path in the sources. A dependency built in the tree without a `LICENSE` or `COPYING` file stops the configuration. Checked with the sources of json-schema-validator 2.4.1, nlohmann/json 3.12.0 and cpp-httplib 0.67.0 (`httplib/LICENSE`); not yet verified with fetched yaml-cpp and spdlog sources, nor the name of spdlog 1.17.0's fmt license file (`fmt.license.rst` is expected: a file named otherwise is not installed, and nothing says so). |
| the entries of each directory of `PRAENORA_THIRD_PARTY_LICENSE_DIRS` | Its contents, as they are. A dependency bundle's `licenses/` directory holds `<component>/` with the license files of each library in the bundle (libRoadRunner, LLVM, libroadrunner-deps' libraries, libAntimony, nlohmann/json, json-schema-validator) and `praenora-runtime-deps/LICENSE`. An entry that is not the absolute path of a directory stops the configuration. |
| `python-environment/` | With `PRAENORA_WITH_PYTHON_ENVIRONMENT`, a copy of the Python environment's `licenses/` directory: the licenses of CPython, of each package of the environment and of the environment's build, as the environment ships them (they are in `lib/praenora/python-environment/licenses/` too). |

The build cannot tell where an installed package keeps its license files, so a static or
header-only library found with `find_package()` brings them into the installation only through
`PRAENORA_THIRD_PARTY_LICENSE_DIRS`. The providers' engines (libRoadRunner, LLVM, libSBML,
libAntimony), the json-schema-validator of a dependency bundle and an installed nlohmann/json or
cpp-httplib (header-only, as in the `dev` build) are such libraries, and so is an installed spdlog
used through its header-only target: when the prefixes come from a bundle, name its `licenses/`
directory, for example in the hidden `dev-local` preset of `CMakeUserPresets.json`:

```json
"PRAENORA_THIRD_PARTY_LICENSE_DIRS": "/path/to/bundle/licenses"
```

A Python wheel build has no install rules: the wheel holds this repository's `LICENSE` only, which
scikit-build-core copies into its `.dist-info`, into `.dist-info/licenses/` with scikit-build-core
1.1.2 (0.10 not yet verified) ([python.md](python.md#building-a-wheel)).

### Archiving an installation

The target `package` ([`cmake/PraenoraPackage.cmake`](../cmake/PraenoraPackage.cmake), CPack)
builds the tree, then archives an installation of it into `build/<preset>/package/`:

```bash
cmake --build --preset dev --target package
```

| File | Content |
|---|---|
| `<name>.tar.xz` (`.zip` on Windows) | The installation, below one top-level directory `<name>/`: the default install component, which is what `cmake --install` installs, license files included ([License files](#license-files)), and never the example modules (component `Examples`), even with `PRAENORA_INSTALL_EXAMPLES`. It holds no Python package `praenora.runtime` ([python.md](python.md#building-a-wheel)); with `PRAENORA_WITH_PYTHON_ENVIRONMENT` it holds the Python environment and the provider `python` ([The installation](#the-installation)), symbolic links as links: the archive of `dev-python-release` on macOS arm64 is about 190 MB. |
| `<name>.tar.xz.sha256` (`.zip.sha256`) | The archive's SHA-256 in the format of `sha256sum`: `<hash>`, two spaces, the archive's file name and a newline, so `sha256sum -c` (or `shasum -a 256 -c`) checks it in that directory. |

CPack keeps its staging directory, `_CPack_Packages/`, beside them. The name is
`praenora-runtime-<version>-<os>-<arch>-<compiler>-<config>`, as the dependency bundles are named
([Using a dependency bundle](#using-a-dependency-bundle)):

| Label | Value |
|---|---|
| `<version>` | The runtime package's version, `project(VERSION)` in `CMakeLists.txt`. |
| `<os>`, `<arch>` | `linux`, `macos` or `windows`, and `x86_64` or `arm64`: the platform the build is for. |
| `<compiler>` | `appleclang<major>`, `clang<major>`, `gcc<major>` or `msvc<toolset>` (`msvc143` for Visual Studio 2022). Another compiler builds and installs as usual, but the configuration says `Praenora: package unavailable`, naming it, and the target `package` stops, saying so again. |
| `<config>` | `release` or `debug`: the configuration packaged. With a single-configuration generator (Ninja, Makefiles) it is `CMAKE_BUILD_TYPE`; any other build type stops the target `package`. With a multi-configuration generator (Visual Studio) it is the configuration that `cpack -C` names. |

The configuration prints the name, for the `dev` preset on macOS:

```text
-- Praenora: package praenora-runtime-0.2.0-macos-arm64-appleclang17-debug
```

With a multi-configuration generator the name in that line ends in `-<config>`. Visual Studio names
the target `PACKAGE`; it passes the build's configuration to cpack (`-C`), so the build preset's
configuration decides the name:

```bash
cmake --build --preset ci-windows-x64-debug --target PACKAGE   # ...-msvc143-debug.zip
```

So a job of the [CI presets](#ci-presets) makes its archive, after its build and tests, with
`cmake --build --preset ci-<key> --target package`, and with `--target PACKAGE` for the keys
`windows-x64-release` and `windows-x64-debug`; each archive is then in `build/ci-<key>/package/`.

After a build, `cpack` makes the same archive with any generator, without building first. `-C`
names the configuration, which a multi-configuration tree needs; a single-configuration tree
packages its build type, and a `-C` that names another configuration stops cpack:

```bash
cpack --config build/dev/CPackConfig.cmake -C Debug
```

There is no source package: the build tree has no target `package_source`, and cpack stops for the
source configuration that CPack writes all the same, `build/<preset>/cmake/CPackSourceConfig.cmake`.
A source archive is made from the repository, for example with `git archive`.

The archive of a `ci-*` tree holds the three providers, `roadrunner`, `antimony` and `python`, the
Python environment in `lib/praenora/python-environment/`, and the license files of the dependency
bundle and of the environment (`share/praenora/licenses/python-environment/`).
An archive never holds the shared libraries that the build found installed: its programs load them
from where the build found them. A `ci-*` job builds yaml-cpp and spdlog in its tree as static
libraries, from downloaded sources, so its archive needs only the system's libraries, which the job
checks after making it ([releasing.md](releasing.md#the-workflow); the check passed on every key);
a tree configured with Homebrew's yaml-cpp and spdlog, such as a local
`ci-macos-arm64-release` run with the overrides of [CI presets](#ci-presets), gives an archive
whose programs need them in `/opt/homebrew`, and the job's check fails on it. Checked on macOS
arm64 with Ninja (`ci-macos-arm64-release` with Homebrew's libraries, and `dev`) and with Ninja
Multi-Config; CI makes the archives of Linux and Windows, the latter with the target `PACKAGE` of
Visual Studio, and `install.package` checks them there
([releasing.md](releasing.md#what-the-runs-verified)).

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

### The Python environment

What runs Python ([specs/python-service.md](specs/python-service.md)), the Python core, the
provider `python`, the example Python module and the tests that embed Python, is built with
`PRAENORA_WITH_PYTHON_ENVIRONMENT=ON`, against a Python environment bundle of
[praenora-runtime-pyenv](https://github.com/INFLECT-Praenora/praenora-runtime-pyenv) for your
platform key: CPython 3.12 with the packages its lock pins, and the CMake package
`PraenoraRuntimePyenv`, which the build finds through `CMAKE_PREFIX_PATH` or
`PraenoraRuntimePyenv_DIR` and never downloads. Fetch the pinned release with
`cmake/praenora_fetch_runtime_pyenv.cmake` ([CI presets](#ci-presets)), or build one in that
repository. Without the option, what needs no Python is built all the same: `python.h`,
`python_module.hpp`, `praenora_add_python_module()`, the digest function, the Python fixture
modules (their libraries need no Python) and the tests that need no Python
([testing.md](testing.md#the-python-environment)).

Keep it in presets of its own, as for the providers: in `CMakeUserPresets.json`, a hidden
`dev-python-local` preset and presets that inherit it before `dev-local`
(`CMakeUserPresets.json.example` holds them):

```json
{
  "name": "dev-python-local",
  "hidden": true,
  "cacheVariables": {
    "CMAKE_PREFIX_PATH": "/path/to/dependency/prefix;/path/to/praenora-runtime-pyenv/bundle",
    "CMAKE_OSX_DEPLOYMENT_TARGET": "14.0",
    "PRAENORA_WITH_PYTHON_ENVIRONMENT": "ON"
  }
},
{
  "name": "dev-python",
  "displayName": "Local development with the Python environment, Debug",
  "inherits": ["macos-arm64-debug", "dev-python-local", "dev-local"]
}
```

with `dev-python-release`, `dev-python-asan` and `dev-python-tsan` likewise, from
`macos-arm64-release`, `-asan` and `-tsan`, and build and test presets of the same names. The
`CMAKE_PREFIX_PATH` of `dev-python-local` replaces the one of `dev-local`, so it lists the other
dependency prefixes too, and the macOS deployment target must be at least the bundle's (14.0 for
0.1.0): the bundle's CMake package refuses an older one, another platform and an older MSVC
toolset. For the Python environment with both providers in one build, where
`PythonRoadrunner.CoexistInOneProcess` runs, add a hidden preset whose `CMAKE_PREFIX_PATH` joins
the three lists, with `LLVM_INSTALL_PREFIX` and the three options, and a preset that inherits it
before `dev-local`, such as `dev-python-engines`. Then:

```bash
cmake --preset dev-python
cmake --build --preset dev-python
ctest --preset dev-python
ctest --preset dev-python-tsan -R '^Python(Core|Digest)\.'
ctest --preset dev-python-tsan -L 'contract|negative|integration'
./build/dev-python/bin/praenora providers list
```

The configuration prints one line that names the environment's package and directory. The build
tree's tool lists `python` without a provider path
([user-guide.md](user-guide.md#finding-service-providers)) and runs the example with its Python
predictor ([user-guide.md](user-guide.md#running-the-example-workflow)). The tests that embed
Python run the environment's interpreter in their process; as elsewhere, the listener tests and
those of the restricted sandbox profile skip in a sandbox
([testing.md](testing.md#the-python-environment)). Configuring a tree without the option removes
the provider `python`'s directory from it, as for the providers. An installation of the tree holds
the environment ([The installation](#the-installation)), about a gigabyte, and its archive is
about 190 MB on macOS arm64.

### Providers implemented in Python

A service provider of this repository may implement a service's typed table in Python
([specs/python-service.md](specs/python-service.md#providers-implemented-in-python)). It is built
only with `PRAENORA_WITH_PYTHON_ENVIRONMENT=ON` (the local preset `dev-python`), and has three
parts, which the fixture provider `test-python-echo` (`tests/fixtures/providers/test-python-echo/`,
the service `test.echo@0`) shows:

1. **Its Python sources**, a top-level package of its own: a class whose constructor takes one
   document (`None`) and whose methods each take a JSON document and return one. What it prints
   or logs, and what it writes through `praenora.sdk.Host(praenora.sdk._runtime.backend())`,
   becomes a record of the calling module when the table function was given its host API, and of
   the provider otherwise.
2. **Its C++ source**, which includes `service_glue.hpp` and no Python header. Its `start()` calls
   `praenora::detail::python::PythonProviderObject::start(host, "<package.module:Class>",
   error_json, &status)`, which starts the Python core and creates the object, and keeps the
   object it returns. Each function of the service's table turns its arguments into a JSON
   document, calls the object's method with `call(module_host, "<method>", document, result,
   error)` and turns the result back into the table's types; a function without an error sink
   logs the error document to the provider's log with `log_to()`.
3. **Its build**, with `praenora_add_python_provider()` instead of `praenora_add_provider()`:

```cmake
praenora_add_python_provider(test_python_echo
    NAME test-python-echo
    VERSION 0.1.0
    SERVICES test.echo@0.0
    ENGINE_JSON "{\"name\": \"test-python-echo-engine\", \"version\": \"1.0.0\"}"
    SOURCES test-python-echo/test_python_echo_provider.cpp
    PYTHON_SOURCES test_python_echo
    PYTHON_ROOT test-python-echo/python
    OUTPUT_DIRECTORY "${PRAENORA_TEST_PROVIDERS_DIR}/test-python-echo")
```

The build copies the sources into `python/` beside the library, compiles their digest into it, adds
the environment to the engine object and writes the environment file `python-environment.json`. An
edited source configures the build again and rebuilds the library; a library whose `python/` holds
other files than those it was built with fails to start (`DATA_INVALID`). The provider links neither
the runtime nor Python, and exports only the provider entry point. Test it as `test-python-echo` is
tested ([testing.md](testing.md#the-python-environment)): its table in the test's process, a run in
the runtime's process and in a module host, its sources edited after the build, a run beside a
Python module, and its library file (`providers.<name>.exports`, and `providers.<name>.processes`
with `praenora_python_print`'s provider mode).

## Debugging modules

Modules run inside the runtime's process, unless their step runs in a module host, so a native
debugger sees the runtime and every module together. A `Debug` build (the `dev` preset) builds the
example and fixture modules with debug information. Points to know:

- **Module libraries load late.** A module's library is opened when a run starts (or by
  `validate --load`, `modules inspect --load` and `modules check`), so a breakpoint in module code
  stays pending until then. The entry point `praenora_module_get_api` is a good first breakpoint.
- **Each instance has its own thread.** Every call into a step's instance runs on a worker thread
  named `praenora:<step>`; the thread that runs the ticks waits for it on a deadline.
- **Steps in module hosts.** A step in isolation mode `process` runs its module in a module host,
  a process of its own, which a debugger of the runtime does not see. Debug the module in the
  runtime's process instead (`--isolation in-process`; the results are the same), or attach the
  debugger to the module host, whose process id the event `module_host.started` gives
  (`--log-level debug`). A module host whose call outlives its deadline and the cancellation grace
  is killed, so disable the timeouts as below for it too
  ([specs/process-isolation.md](specs/process-isolation.md)).
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
are at level `debug`, and the start and end of a run at `info`. Every attribute key carries the
prefix `praenora.`, except `logger`:

```bash
PRAENORA_LOG_LEVEL=debug ./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules --ticks 1
```

```text
2026-11-11T18:49:38.378297000Z DEBUG praenora.engine [praenora.run.fingerprint=a4d251a5be737c49fe7cc73b30aa65367f3bc8e55cb2818ebb12bbafed93f46d praenora.event=invocation.completed praenora.tick=0 praenora.step=estimate praenora.module=example-estimator@0.1.0 praenora.duration_ns=257458] invocation completed
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

**OTLP export.** With an endpoint, the runtime also sends its log records and metrics to an
OpenTelemetry collector over OTLP/HTTP with JSON
([specs/observability.md](specs/observability.md#otlp-export)); the tool and Python take the
settings from the `OTEL_*` variables, C++ also from `RuntimeOptions::telemetry`:

```bash
OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318 OTEL_METRIC_EXPORT_INTERVAL=1000 \
    ./build/dev/bin/praenora run examples/workflows/estimate-predict/workflow.yaml \
    --module-path build/dev/modules
```

A collector that is down or refuses the export changes nothing in the run: each failure is
counted, and logged as the warning `telemetry.export_failed` at most once a minute per signal, and
the end of the process waits for what is left at most about one export timeout (10 s by default,
`OTEL_EXPORTER_OTLP_TIMEOUT`). A value the export cannot use is ignored with the warning
`telemetry.setting_ignored`, never an error. `OTEL_SDK_DISABLED=true` turns the export off, as
every test does; the tests of the export send to a fake collector of their own
([testing.md](testing.md#listeners)).

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
- The third-party libraries are normally linked as installed, without the sanitizer flags;
  json-schema-validator is built with them in a tree configured with
  `PRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON`, as is any library that `PRAENORA_FETCH_DEPENDENCIES`
  downloads ([platforms.md](platforms.md#json-schema-validator),
  [testing.md](testing.md#sanitizer-builds)).
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
  [open-questions.md](open-questions.md). Resolving one needs an Accepted decision of the
  maintainer (the linked ADR accepted, in full or in part, or an Accepted ADR that is new or
  supersedes it) and updates its row, as the register's header says.

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
  documents whenever a workflow definition, a message type or the envelope schema changes, and,
  with a local copy of the official AsyncAPI 3.0.0 schema at hand, check them against it
  ([testing.md](testing.md#the-official-asyncapi-schema)), and with the official AsyncAPI parser
  installed, through it ([testing.md](testing.md#the-official-asyncapi-parser)).
