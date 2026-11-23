# Platforms

`praenora-runtime` is written for Linux x86-64, Windows x86-64 and macOS arm64, with presets,
portable code and commands for each. It is verified on macOS arm64 locally and by CI, and by CI
on Linux x86-64 and Windows x86-64 with the CI presets
([releasing.md](releasing.md#what-the-runs-verified)); the other Linux and Windows commands below
are given as they should work and are marked "not yet verified".

## Verification status

| Platform | Compiler | Status |
|---|---|---|
| macOS arm64 (macOS 15; macOS 26 in CI) | AppleClang 17; AppleClang 21 in CI | Verified: configure, build without warnings, and every test in the `dev` (Debug), `dev-release`, `dev-asan` and `dev-tsan` builds; an installation, moved; the Python bindings with CPython 3.12; a Python wheel built offline and installed into a fresh virtual environment ([python.md](python.md#building-a-wheel)); the roadrunner provider with libRoadRunner 2.11.0 and LLVM 13.0.1, every test in a Debug build and the labels `unit`, `contract`, `negative`, `lifecycle`, `integration` and `install` in a ThreadSanitizer build ([The roadrunner provider](#the-roadrunner-provider)); the antimony provider with libAntimony 3.3.0 built from source, every test in a Debug build and in a Debug build with both providers, and the labels `unit`, `contract` and `integration` in a ThreadSanitizer build ([The antimony provider](#the-antimony-provider)); process isolation in module hosts, with the shared compiled models and the restricted sandbox profile, whose tests run outside any other sandbox ([specs/process-isolation.md](specs/process-isolation.md)); the `ci-macos-arm64-release` preset against a dependency bundle built locally, with installed yaml-cpp, spdlog and GoogleTest instead of fetched ones ([The CI presets](#the-ci-presets)); and by CI, the `ci-macos-arm64-release` preset with AppleClang 21 on `macos-26`, against the dependency bundle, with fetched yaml-cpp, spdlog and GoogleTest: every test, those of the restricted sandbox profile included, and the archive ([releasing.md](releasing.md#what-the-runs-verified)). The control API and the OTLP export, with cpp-httplib 0.67.0 installed header-only, locally only: the `dev`, `dev-release`, `dev-asan` and `dev-tsan` builds without warnings and their tests, the listener tests ([testing.md](testing.md#listeners)) from a shell outside any sandbox in the `dev` and `dev-tsan` builds; the `ci-macos-arm64-release` preset with installed packages, its tests outside any sandbox; and a wheel built offline with cpp-httplib (built and imported; it holds no server, and its export test skips where listeners are refused). Not yet verified: these with CI, and cpp-httplib downloaded in fetch mode. |
| Linux x86-64 | GCC 9+, Clang 10+ | Presets, a CI preset and portable code. Verified by CI ([releasing.md](releasing.md#what-the-runs-verified)): the `ci-linux-x64-release` preset with GCC 13.3 on `ubuntu-24.04`, against the dependency bundle: configure, build without warnings, and every test, with both providers, process isolation in module hosts, the Python bindings with CPython 3.12, the install tests and the archive (15 tests skip: the 12 of the restricted sandbox profile, which exists only on macOS, and 3 that need what the runner lacks). Not yet verified: the `linux-x64-debug`, `linux-x64-asan` and `linux-x64-tsan` presets, Clang, older GCC, and the distributions' packages ([Linux](#linux)); and the control API and the OTLP export, whose code CI has not built yet. |
| Windows x86-64 | MSVC 19.29+ (Visual Studio 2022) | Presets, CI presets and portable code. Verified by CI ([releasing.md](releasing.md#what-the-runs-verified)): the `ci-windows-x64-release` and `ci-windows-x64-debug` presets with MSVC 19.44 (Visual Studio 2022) on `windows-2022`, against the dependency bundles: configure, build without warnings, and every test, with both providers, the Python bindings with CPython 3.12, the install tests and the archives (5 tests skip, which Windows cannot run). Process isolation is not available on Windows in this version. Not yet verified: older MSVC, and libraries from vcpkg or conda-forge ([Windows](#windows)); and the control API (TCP only on Windows) and the OTLP export, whose code CI has not built yet. |

Fetch mode is verified by CI for yaml-cpp, spdlog and GoogleTest, which the CI presets download
and build ([The CI presets](#the-ci-presets)), and the fetched yaml-cpp and spdlog end up static
libraries: the archive check finds no library of theirs. The CI presets download cpp-httplib too;
that download is not yet verified ([cpp-httplib](#cpp-httplib)). Which of the settings of
[ADR-0017](adr/ADR-0017-dependency-management.md) brings that about, and that the fetched
projects' install rules are off, no test shows. Not yet verified on any platform: downloading
nlohmann/json and json-schema-validator in fetch mode (CI takes them from the bundle); building a
Python wheel that downloads its build requirements (an offline wheel build is verified on macOS
arm64); Python 3.10 and 3.11; and the multi-configuration generators Xcode and Ninja Multi-Config
(Visual Studio's is verified by CI on Windows, one configuration per tree). macOS x86-64 is not a
target.

## Common to every platform

- **Compilers.** C++17 without extensions and C99. Configuration stops below GCC 9, Clang 10,
  AppleClang 13 or MSVC 19.29 (Visual Studio 2019 16.10). Warnings are `-Wall -Wextra -Wpedantic`
  for GCC and Clang and `/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc` for MSVC;
  `PRAENORA_WARNINGS_AS_ERRORS=ON` adds `-Werror` or `/WX`. GCC 13 and newer also get
  `-Wno-dangling-reference` for C++: its reports here are all false alarms, references to objects
  that outlive the call (such as a loaded provider); the targets that compile libRoadRunner's
  headers get MSVC's `/wd4702`, for unreachable code in its `rr/Setting.h`; and the wrapper that
  SWIG generates for the Python extension is compiled with the compiler's default warnings, never
  as errors, while the facade it wraps has the project's.
- **One architecture per build.** A module manifest names one `platform.arch`, so a build targets
  one architecture: several `CMAKE_OSX_ARCHITECTURES` values are refused.
- **Platform names.** Manifests and `praenora version` name the platform as `linux`, `macos` or
  `windows` and `x86_64` or `arm64`. A module's manifest must name the platform the runtime was
  built for (`PLATFORM_MISMATCH`). One directory may hold manifests for several platforms: those of
  other platforms are listed but never hide a manifest for this one.
- **Loading.** The runtime opens a module library by its absolute path, from the manifest's
  directory, and calls only `praenora_module_get_api`. One load per module is shared by its
  instances and released after the last instance is destroyed
  ([ADR-0019](adr/ADR-0019-platform-loading-rpath-dll-search.md)).
- **Text and paths.** Documents, data and paths are UTF-8. Configurations, manifests and data files
  edited on Windows, with a byte order mark and CRLF line endings, read like LF files, and paths
  with spaces and non-ASCII characters work.

## json-schema-validator

Every platform needs json-schema-validator (pboettch) 2.3 or newer, which Homebrew, apt and dnf do
not package. Build it into a local prefix of your choice, then list that prefix in
`CMAKE_PREFIX_PATH` when you configure this project
([developer-guide.md](developer-guide.md#local-presets)). Not yet verified: the commands download
the sources, which the environment where this guide was checked does not allow. They clone and
build in a scratch directory outside this repository, so they can be run from any directory, the
repository root included. `-DCMAKE_PREFIX_PATH=/opt/homebrew` lets the build find Homebrew's
nlohmann/json on macOS; leave it out on Linux.

```sh
JSV="${TMPDIR:-/tmp}/json-schema-validator"
git clone --branch 2.4.1 https://github.com/pboettch/json-schema-validator.git "$JSV"
cmake -S "$JSV" -B "$JSV/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DJSON_VALIDATOR_BUILD_TESTS=OFF \
    -DJSON_VALIDATOR_BUILD_EXAMPLES=OFF -DJSON_VALIDATOR_INSTALL=ON \
    -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_INSTALL_PREFIX="$HOME/.local/opt/praenora-deps"
cmake --build "$JSV/build" --target install
```

**Sanitizer builds.** The library these commands build has no sanitizer flags, and a sanitizer
sees only the code compiled with them. In a sanitizer build that links it, errors inside the
validator go unnoticed, and AddressSanitizer can report a container overflow that is not there:
the instrumented copies of the container code mark a container's unused capacity, and the
library's own copies fill it without clearing the mark. A sanitizer build can instead build the
validator itself, with its own flags, from the commit that
[`cmake/PraenoraDependencies.cmake`](../cmake/PraenoraDependencies.cmake) pins (2.4.1): configure
it with `-DPRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON`. CMake's FetchContent then downloads the
sources, unless the cache variable `FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON_SCHEMA_VALIDATOR` names
a local copy of them: a clone of the pinned commit, as above, but in a directory that lasts. macOS
clears `$TMPDIR` after some days, and a configuration whose local copy is gone stops with an error.

```sh
JSV_SRC="$HOME/.local/src/json-schema-validator"
git clone --branch 2.4.1 https://github.com/pboettch/json-schema-validator.git "$JSV_SRC"
cmake --preset dev-asan -DPRAENORA_FETCH_JSON_SCHEMA_VALIDATOR=ON \
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON_SCHEMA_VALIDATOR="$JSV_SRC"
cmake --build --preset dev-asan
```

Both are cache variables, so they stay set for later configurations of that tree; they can also go
into the `dev-asan` and `dev-tsan` presets of `CMakeUserPresets.json`. The installed validator is
then not used, though its prefix stays in `CMAKE_PREFIX_PATH`, and the other dependencies are found
as before. The build writes nothing into the local copy and does not check it against the pinned
commit; the configuration's status line reads `dependency nlohmann_json_schema_validator 2.4.1
(local sources at <directory>)`. yaml-cpp, spdlog and GoogleTest are still linked as installed,
without sanitizer flags; no false report from them has been seen. Verified on macOS arm64 with a
clone at the pinned commit, in the `dev-asan` and `dev-tsan` builds; the download is not yet
verified.

## cpp-httplib

Every platform needs [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.67, header-only: the
runtime's OTLP export ([specs/observability.md](specs/observability.md#otlp-export)) and the control
API use it, without TLS. 0.67 is the only version built and tested, and its CMake package accepts
only 0.67.x. Install it header-only into a local prefix of your choice, then name that prefix in
`CMAKE_PREFIX_PATH` (or its `lib/cmake/httplib` in `httplib_DIR`) when you configure this project
([developer-guide.md](developer-guide.md#local-presets)). Not yet verified: the commands download
the sources. They clone and build in a scratch directory outside this repository, and every option
that would find TLS, compression or another library, compile the header into a library or build
its tests is off; the install copies `httplib.h`, the CMake package and the license, and builds
nothing.

```sh
HTTPLIB="${TMPDIR:-/tmp}/cpp-httplib"
git clone --branch v0.67.0 https://github.com/yhirose/cpp-httplib.git "$HTTPLIB"
cmake -S "$HTTPLIB" -B "$HTTPLIB/build" -DCMAKE_BUILD_TYPE=Release \
    -DHTTPLIB_COMPILE=OFF -DHTTPLIB_TEST=OFF -DHTTPLIB_INSTALL=ON \
    -DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=OFF -DHTTPLIB_USE_WOLFSSL_IF_AVAILABLE=OFF \
    -DHTTPLIB_USE_MBEDTLS_IF_AVAILABLE=OFF -DHTTPLIB_USE_ZLIB_IF_AVAILABLE=OFF \
    -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF -DHTTPLIB_USE_ZSTD_IF_AVAILABLE=OFF \
    -DHTTPLIB_USE_NON_BLOCKING_GETADDRINFO=OFF \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local/opt/praenora-deps"
cmake --install "$HTTPLIB/build"
```

The configuration prints `dependency cpp-httplib 0.67.0 (found at <prefix>/lib/cmake/httplib;
header-only)`. The build takes only the package's include directory, as a system directory, and
none of its compile definitions or link libraries (but `ws2_32` on Windows), so no TLS or
compression support is compiled in whatever the package was built with. It sets one definition
itself, on POSIX systems other than macOS: `CPPHTTPLIB_SEND_FLAGS=MSG_NOSIGNAL`, so that no write
raises `SIGPIPE` (on macOS the clients set `SO_NOSIGPIPE` on their sockets instead). A package
whose header is compiled into a library (`HTTPLIB_COMPILE`, as some distributions build it) stops
the configuration with a message that names this section. The distributions' packages (Homebrew's
`cpp-httplib`, apt's `libcpp-httplib-dev`, vcpkg's and conda-forge's) are not yet verified.

**Fetch mode.** With `-DPRAENORA_FETCH_DEPENDENCIES=ON` and no package found, CMake's FetchContent
takes cpp-httplib from its pinned commit (`7800c3dc5b33285cbb18baa5998de4ff7ef63515`, the tag
`v0.67.0`; upstream's tag `latest` moves and is never pinned), with the options above off, and the
installation then holds its `LICENSE` in `share/praenora/licenses/httplib/`
([developer-guide.md](developer-guide.md#license-files)); the CI presets always do
([The CI presets](#the-ci-presets)). Checked on macOS arm64 without a download, with
`FETCHCONTENT_SOURCE_DIR_HTTPLIB` naming a clone at the pinned commit: the build, the install
tests (the license file listed, no header or CMake file of cpp-httplib installed) and the archive.
The download is not yet verified.

## The roadrunner provider

The service provider `roadrunner`, which simulates SBML models with libRoadRunner
([specs/simulation-service.md](specs/simulation-service.md#the-roadrunner-provider)), is built only
with `-DPRAENORA_WITH_ROADRUNNER=ON`. It needs an installed libRoadRunner 2.11 C++ library (its
static library `roadrunner-static` and the packages it links) and the LLVM it was built with; the
build never downloads them. It was built and tested on macOS arm64 with the upstream release
binaries of libRoadRunner 2.11.0 (`roadrunner-2.11.0-macos-14-arm64-Release.zip` of the release
v2.11.0 of sys-bio/roadrunner) and of LLVM 13.0.1 (`llvm-13.x-macos-14-arm64-Release.zip` of the
release v13.1 of sys-bio/llvm-13.x), each unpacked into a prefix of its own
(`chmod a+x bin/llvm-config` in LLVM's).

libRoadRunner's installation keeps its CMake packages directly in `<prefix>/lib/cmake`, and
POCO's in `<prefix>/lib/cmake/Poco`, so those directories go into `CMAKE_PREFIX_PATH`, and
`LLVM_INSTALL_PREFIX` names LLVM's prefix, as for libRoadRunner's own build. A local preset in
`CMakeUserPresets.json` ([developer-guide.md](developer-guide.md#the-roadrunner-provider)):

```json
{
  "name": "dev-roadrunner-local",
  "hidden": true,
  "cacheVariables": {
    "CMAKE_PREFIX_PATH": "/opt/homebrew;<json-schema-validator prefix>;<RR>;<RR>/lib/cmake;<RR>/lib/cmake/Poco;<LLVM>",
    "LLVM_INSTALL_PREFIX": "<LLVM>",
    "PRAENORA_WITH_ROADRUNNER": "ON"
  }
}
```

with `<RR>` and `<LLVM>` the two prefixes. Configuring prints a line for libRoadRunner, SUNDIALS,
libSBML and LLVM, and stops with a message that names what is missing. It accepts libRoadRunner
2.11.x only, with a status line when the version is not 2.11.0, the one checked, and requires LLVM
13, which libRoadRunner 2.11 is built against; another version of either stops the configuration.
`llvm-config` is searched for again at each configuration, so changing `LLVM_INSTALL_PREFIX` in a
tree takes effect. The upstream package of
`roadrunner-static` names LLVM's libraries by the paths of the machine that built it; the build
takes the libraries of the same names from `LLVM_INSTALL_PREFIX` instead
([`cmake/PraenoraRoadrunner.cmake`](../cmake/PraenoraRoadrunner.cmake)).

- **macOS.** The upstream binaries are built for macOS 14, so a build with the provider needs
  `CMAKE_OSX_DEPLOYMENT_TARGET` 14.0 or later (15.0 where this was checked); with the project
  default of 11.0 the linker warns about every one of their libraries. The provider library is a
  `.so` bundle, about 48 MB in a Debug build and about 37 MB stripped, that needs only libSystem
  and libc++, and exports only `praenora_provider_get_api`. libRoadRunner compiles models into
  machine code at run time, so a host signed with the hardened runtime needs an entitlement for
  code generated at run time: `com.apple.security.cs.allow-jit` (for memory mapped with `MAP_JIT`)
  or, if LLVM's MCJit memory manager does not use `MAP_JIT`,
  `com.apple.security.cs.allow-unsigned-executable-memory`. Neither is verified with such a host
  yet.
- **Linux.** Built and tested by CI against the dependency bundle, with GCC 13.3 on Ubuntu 24.04
  ([releasing.md](releasing.md#what-the-runs-verified)). The export list is a version script,
  which `providers.roadrunner.exports` checks. The upstream `manylinux_2_28` and `ubuntu-24`
  archives are not yet verified.
- **Windows.** Built and tested by CI against the dependency bundles, Release and Debug, which are
  built with Visual Studio 2022 ([The CI presets](#the-ci-presets)). The upstream `windows-msvc18`
  archives may need newer build tools than Visual Studio 2022 (a static library must be linked by
  tools at least as new as those that built it); not yet verified. A bundle keeps some of
  libRoadRunner's packages in `<RR>/cmake`, which goes into `CMAKE_PREFIX_PATH` too. On Windows
  the finder loads libSBML's package under the name `libsbml-static` (not `sbml-static`), sets `EXPAT_LIBRARY` and
  `EXPAT_INCLUDE_DIR` from expat's package before it (libSBML's package looks for expat with a
  `FindEXPAT` module, which need not find the bundle's expat, `libexpatMD.lib` or
  `libexpatdMD.lib`, by itself), and gives libSBML's target the definitions `LIBSBML_STATIC` and
  `LIBLAX_STATIC`, without which libSBML's headers declare its functions for import from a DLL.
  In a multi-configuration build every configuration writes its provider to the same
  directory, `build\<preset>\bin\lib\praenora\providers` (the `..\lib\praenora\providers` of each
  configuration's `bin\<config>`, where its runtime looks), so build one configuration per tree.

The provider goes where the build tree's runtime looks for the providers of its installation
(`build/<preset>/lib/praenora/providers/roadrunner/`), so `praenora providers list` lists it without
a provider path, and it is installed to `<prefix>/lib/praenora/providers/roadrunner/`. Distributing
it raises license questions (libSBML is LGPL-2.1 and is linked statically; the NLEQ sources state no
license), which are open, with the other questions of building and distributing the engine
([open-questions.md](open-questions.md), OQ-27). An installation holds the license files of
libRoadRunner, LLVM and libroadrunner-deps' libraries only from a directory that
`PRAENORA_THIRD_PARTY_LICENSE_DIRS` names, such as a dependency bundle's `licenses/`
([developer-guide.md](developer-guide.md#license-files)).

## The antimony provider

The service provider `antimony`, which converts Antimony to SBML and back with libAntimony
([specs/model-conversion-service.md](specs/model-conversion-service.md#the-antimony-provider)), is
built only with `-DPRAENORA_WITH_ANTIMONY=ON`. It needs libAntimony 3.3 as a static library
(`antimony-static`), and the libSBML (with the comp package), SBMLNetwork and expat that it was
built against; the build never downloads them. No archive of libAntimony's C library for C or C++
consumers was found among its release's assets, so it is built from source, against libRoadRunner's
dependencies: the binary release of libRoadRunner 2.11.0
([The roadrunner provider](#the-roadrunner-provider)) holds libSBML 5.21.2
(`libsbml-static.a` and its package `sbml-static`), SBMLNetwork 0.5.12 (`libsbmlnetwork-static.a`,
without a package), expat 2.9.0, zlib and bzip2, with their packages in `<prefix>/lib/cmake`; a
build of libroadrunner-deps holds the same. It was built and tested on macOS arm64 with libAntimony
3.3.0 (the tag `v3.3.0` of sys-bio/antimony, commit `f13a50bce91ff2d16ec4b6c5179a8d43edff9f9b`)
built against the release binaries of libRoadRunner 2.11.0.

Not yet verified as written: the commands download the sources (the build that was checked used a
copy of them). With `<RR>` the prefix of libRoadRunner's release binaries and `<ANT>` the prefix to
install libAntimony into, they clone and build in a scratch directory outside this repository:

```sh
ANT_SRC="${TMPDIR:-/tmp}/antimony-3.3.0-src"
git clone --branch v3.3.0 --depth 1 https://github.com/sys-bio/antimony.git "$ANT_SRC"
cmake -S "$ANT_SRC" -B "$ANT_SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DANT_DEPENDENCIES_INSTALL_PREFIX=<RR> \
    -DWITH_QTANTIMONY=OFF -DWITH_PYTHON=OFF -DWITH_CELLML=OFF -DWITH_GTEST=OFF -DWITH_JULIA=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_INSTALL_PREFIX=<ANT>
cmake --build "$ANT_SRC/build"
cmake --install "$ANT_SRC/build"
```

`ANT_DEPENDENCIES_INSTALL_PREFIX` is where libAntimony's build finds libSBML, SBMLNetwork and
expat; position-independent code lets the static library be linked into the provider library;
`CMAKE_OSX_DEPLOYMENT_TARGET` matches the dependencies' (macOS only). The installation holds
`include/antimony_api.h` and the headers it includes, and `lib/libantimony-static.a` (and a shared
library, which the provider does not use).

The provider's build finds libAntimony and SBMLNetwork by their headers and libraries on
`CMAKE_PREFIX_PATH`, and libSBML by its package, so `<ANT>`, `<RR>` and `<RR>/lib/cmake` go into
`CMAKE_PREFIX_PATH`. A local preset in `CMakeUserPresets.json`
([developer-guide.md](developer-guide.md#the-antimony-provider)):

```json
{
  "name": "dev-antimony-local",
  "hidden": true,
  "cacheVariables": {
    "CMAKE_PREFIX_PATH": "/opt/homebrew;<json-schema-validator prefix>;<RR>;<RR>/lib/cmake;<ANT>",
    "CMAKE_OSX_DEPLOYMENT_TARGET": "14.0",
    "PRAENORA_WITH_ANTIMONY": "ON"
  }
}
```

Configuring builds and runs a probe that links libAntimony, SBMLNetwork and libSBML together and
prints the versions they report, which become the provider's engine object; then it prints a line
for libAntimony, libSBML, SBMLNetwork and expat, and stops with a message that names what is missing
or does not link. It accepts libAntimony 3.3.x only, with a status line when the version is not
3.3.0, the one checked, and a libSBML with the comp package
([`cmake/PraenoraAntimony.cmake`](../cmake/PraenoraAntimony.cmake)). With
`PRAENORA_WITH_ROADRUNNER` too, both providers are built from the one libSBML package, and each
links a copy of libSBML of its own; a process that loads both holds two.

- **macOS.** libRoadRunner's release binaries, and so libAntimony built against them, are built for
  macOS 14: a build with the provider needs `CMAKE_OSX_DEPLOYMENT_TARGET` 14.0 or later (15.0 where
  this was checked). The provider library is a `.so` bundle, about 12 MB in a Debug build and about
  8 MB stripped, that needs only libSystem and libc++, and exports only `praenora_provider_get_api`.
- **Linux.** Built and tested by CI against the dependency bundle, with GCC 13.3 on Ubuntu 24.04
  ([releasing.md](releasing.md#what-the-runs-verified)). The export list is a version script,
  which `providers.antimony.exports` checks; SBMLNetwork needs `libdl`, which the build adds.
- **Windows.** Built and tested by CI against the dependency bundles, Release and Debug
  ([releasing.md](releasing.md#what-the-runs-verified)). The static libraries have other names
  there: `libantimony-static.lib`, and `libsbmlnetwork-staticmd.lib`
  (`libsbmlnetwork-staticmdd.lib` for Debug) in a dependency bundle, which the finder searches for too. libSBML's package is
  `libsbml-static`, loaded after expat's location is set, and its target gets `LIBSBML_STATIC` and
  `LIBLAX_STATIC`, as for the roadrunner provider. libAntimony's header (`libutil.h`) declares its
  functions for import from a DLL unless `LIB_EXPORTS` is defined, so the provider's sources and
  the probe are compiled with `LIB_EXPORTS`; calls then refer to the names its static library
  defines. With a multi-configuration generator, the probe is built in the configuration that
  `CMAKE_TRY_COMPILE_CONFIGURATION` names (Debug when it is not set), whose MSVC runtime library
  must be the one the libraries were built with: `Release` for a Release bundle, as the
  `ci-windows-x64-release` preset sets it.

The provider goes where the build tree's runtime looks for the providers of its installation
(`build/<preset>/lib/praenora/providers/antimony/`), so `praenora providers list` lists it without a
provider path, and it is installed to `<prefix>/lib/praenora/providers/antimony/`. Distributing it
raises license questions (libSBML is LGPL-2.1 and is linked statically; the licenses of SBMLNetwork
and of expat's copy are unchecked), which are open, with the other questions of building and
distributing libAntimony ([open-questions.md](open-questions.md), OQ-29). An installation holds the
license files of libAntimony, libSBML, SBMLNetwork and expat only from a directory that
`PRAENORA_THIRD_PARTY_LICENSE_DIRS` names, such as a dependency bundle's `licenses/`
([developer-guide.md](developer-guide.md#license-files)).

## The CI presets

The `ci-*` presets, for continuous-integration jobs, do not use the installations of the sections
above. They take json-schema-validator (with the nlohmann/json it is compiled against),
libRoadRunner with libroadrunner-deps' libraries, LLVM and libAntimony from a dependency bundle of
[praenora-runtime-deps](https://github.com/INFLECT-Praenora/praenora-runtime-deps), one per
platform key, at the release that
[`cmake/PraenoraRuntimeDepsPin.cmake`](../cmake/PraenoraRuntimeDepsPin.cmake) pins
([developer-guide.md](developer-guide.md#using-a-dependency-bundle)), and build yaml-cpp, spdlog,
GoogleTest and cpp-httplib (header-only) from their pinned commits with FetchContent. Each key has
a configure, build and test preset in `CMakePresets.json`, which builds both providers, the Python
bindings and the tests, with warnings as errors
([developer-guide.md](developer-guide.md#ci-presets) lists what they set):

| Preset | Bundle | Inherits | Status |
|---|---|---|---|
| `ci-linux-x64-release` | `linux-x64-release` | `linux-x64-release` | Verified by CI: configure, build without warnings and every test, with GCC 13.3 and fetched yaml-cpp, spdlog and GoogleTest. With cpp-httplib, the control API and the OTLP export, not yet verified. |
| `ci-macos-arm64-release` | `macos-arm64-release` | `macos-arm64-release`, with `CMAKE_OSX_DEPLOYMENT_TARGET` 14.0, the bundle's | Verified by CI: configure, build without warnings and every test, with AppleClang 21 and fetched yaml-cpp, spdlog and GoogleTest. Locally also against a bundle built with AppleClang 17, with Homebrew's yaml-cpp, spdlog and GoogleTest instead of fetched ones, and so with their deployment target, 15.0, and since cpp-httplib is a dependency with it installed header-only: configure, build and every test, from a shell outside any sandbox ([developer-guide.md](developer-guide.md#ci-presets)). With fetched cpp-httplib, not yet verified. |
| `ci-windows-x64-release` | `windows-x64-release` | `windows-x64-release`, with `CMAKE_TRY_COMPILE_CONFIGURATION` Release | Verified by CI: configure, build without warnings and every test, with MSVC 19.44 and fetched yaml-cpp, spdlog and GoogleTest. With cpp-httplib, the control API and the OTLP export, not yet verified. |
| `ci-windows-x64-debug` | `windows-x64-debug` | `windows-x64-debug`, with `CMAKE_TRY_COMPILE_CONFIGURATION` Debug | Verified by CI, as for Release, and not yet with cpp-httplib. |

The jobs of the CI workflow, `.github/workflows/build.yml`, run these presets, each on the runner
image of its bundle's job in praenora-runtime-deps ([releasing.md](releasing.md#the-workflow)); what
their runs of 2026-11-22 verified, with the tests that skip on each platform, is in
[releasing.md](releasing.md#what-the-runs-verified).

The environment variable `PRAENORA_RUNTIME_DEPS` names the bundle's directory; a configuration with
one of these presets stops when it does not name a bundle. On Windows, the bundle's
`roadrunner/cmake` is on `CMAKE_PREFIX_PATH` too, and the finders handle the library names and
definitions of [The roadrunner provider](#the-roadrunner-provider) and
[The antimony provider](#the-antimony-provider). A bundle is built with one compiler; build with
that compiler or a newer one of the same family.

## macOS

**Dependencies.** Not yet verified: this command downloads packages, which the environment where
this guide was checked does not allow. Homebrew provides everything except json-schema-validator,
which is built from source ([json-schema-validator](#json-schema-validator)), and cpp-httplib,
installed header-only ([cpp-httplib](#cpp-httplib)).

```sh
brew install cmake ninja swig nlohmann-json yaml-cpp spdlog googletest
```

**Presets.** `macos-arm64-debug`, `macos-arm64-release`, `macos-arm64-asan` and `macos-arm64-tsan`
(Ninja, `CMAKE_OSX_ARCHITECTURES=arm64`). The local `dev*` presets inherit them; in `dev-local`,
`CMAKE_PREFIX_PATH` lists Homebrew's prefix and the json-schema-validator prefix
([developer-guide.md](developer-guide.md#local-presets)). `ci-macos-arm64-release` builds against a
dependency bundle ([The CI presets](#the-ci-presets)).

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

**Deployment target.** The project builds for macOS 11.0 and later (`CMAKE_OSX_DEPLOYMENT_TARGET`,
a cache default set before `project()`). Homebrew builds its bottles for the macOS release they were
made on, and linking those libraries into a build for an older target makes the linker warn about
every one of them. When that happens, set `CMAKE_OSX_DEPLOYMENT_TARGET` to that release, for
example `"15.0"`, in the `dev-local` preset of your `CMakeUserPresets.json`. The setting is local:
the project default stays 11.0.

**Run paths.** In the build tree, CMake's default run paths let the tool and the tests find the
runtime library. An installation is relocatable: the installed `praenora` finds the runtime through
the run path `@loader_path/../lib`, and the runtime's install name is
`@rpath/libpraenora_runtime.0.2.dylib`; the runtime itself has no run path and finds its
third-party libraries by their own install names (Homebrew's are absolute). Modules are `MODULE`
libraries with the suffix `.so`; a module's own dependencies belong beside it, found through an
`@loader_path` run path of the module. On an installation:

```bash
PREFIX="${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "$PREFIX"
otool -l "$PREFIX/bin/praenora" | grep -A2 LC_RPATH
otool -D "$PREFIX/lib/libpraenora_runtime.dylib"
```

shows `path @loader_path/../lib` and `@rpath/libpraenora_runtime.0.2.dylib`. (Inside some
sandboxes, Apple's developer tools also print a harmless `xcrun_db` cache warning.)

**Code signing and the hardened runtime.** The linker gives every binary of the build an ad-hoc
signature, without the hardened runtime:

```bash
codesign --display --verbose=2 build/dev/modules/example-estimator/example-estimator.so
```

prints `Signature=adhoc` and `flags=0x20002(adhoc,linker-signed)`. A host application signed with
the hardened runtime enforces library validation: it loads only libraries signed by Apple or by its
own team, so to load third-party modules it needs library validation disabled (the entitlement
`com.apple.security.cs.disable-library-validation`; not yet verified with such a host). Signing and
notarizing distributed binaries are open questions
([open-questions.md](open-questions.md), OQ-13).

**Module hosts and the restricted sandbox profile.** The module host, `praenora-module-host`, is in
`libexec/praenora/` of the build tree or the installation, beside the runtime's library directory
([specs/process-isolation.md](specs/process-isolation.md#the-module-host)). The restricted sandbox
profile of a module host (`sandbox: restricted`) exists only on macOS in this version: the module
host applies it with `sandbox_init()`, which macOS has deprecated without a successor and still
provides. macOS applies no second sandbox, so a runtime that runs in a sandbox (another program's)
cannot run a step under the profile; its load fails with `MODULE_HOST_UNAVAILABLE`, reason
`sandbox` ([specs/process-isolation.md](specs/process-isolation.md#the-restricted-profile)). The
build's module host is signed ad hoc, without the hardened runtime, and needs no entitlement. A
module host signed with the hardened runtime would need, not yet verified:
`com.apple.security.cs.allow-jit` (or `com.apple.security.cs.allow-unsigned-executable-memory`), so
that an engine such as libRoadRunner can generate code, and
`com.apple.security.cs.disable-library-validation`, to load modules and providers signed otherwise.

**Other notes.**

- Worker threads are named `praenora:<step>` by the threads themselves, because macOS names only
  the calling thread.
- AddressSanitizer keeps an unloaded instrumented library's globals registered, so the tests of an
  AddressSanitizer build keep module libraries loaded ([testing.md](testing.md#sanitizer-builds)).
- The control API's Unix socket paths hold at most 103 bytes (`sun_path`). The socket file exists
  for the moment between `bind()` and `chmod()` (normally microseconds) with the mode the umask
  leaves, while the socket refuses every connection, and a socket is taken for stale only after
  three refused connections, since a full backlog refuses with `ECONNREFUSED` here
  ([specs/control-api.md](specs/control-api.md#listeners)).

## Linux

CI builds and tests the `ci-linux-x64-release` preset, which inherits `linux-x64-release`, and its
install tests run the installed tool through its run paths, before and after the installation is
moved ([The CI presets](#the-ci-presets)). The rest of this section is not yet verified: the
distributions' packages, the other presets and the commands below.

**Dependencies.** Distribution packages provide everything except json-schema-validator, which is
built from source ([json-schema-validator](#json-schema-validator)), and cpp-httplib, installed
header-only ([cpp-httplib](#cpp-httplib)):

```sh
sudo apt install cmake ninja-build g++ nlohmann-json3-dev libyaml-cpp-dev libspdlog-dev libgtest-dev swig python3-dev
sudo dnf install cmake ninja-build gcc-c++ json-devel yaml-cpp-devel spdlog-devel gtest-devel swig python3-devel
```

The first line is for Debian and Ubuntu (24.04 or newer), the second for Fedora.

**Presets.** `linux-x64-debug`, `linux-x64-release`, `linux-x64-asan` and `linux-x64-tsan` (Ninja),
and `ci-linux-x64-release`, which builds against a dependency bundle
([The CI presets](#the-ci-presets)). In `CMakeUserPresets.json`, make the `dev*` configure, build
and test presets inherit the matching `linux-x64-*` presets instead of `macos-arm64-*`, then:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/praenora version
```

**Run paths and loading.** The installed `praenora` finds the runtime through the run path
`$ORIGIN/../lib` (with the installation's library directory name, `lib64` on some distributions).
Modules are opened with `dlopen(path, RTLD_NOW | RTLD_LOCAL)`, so their symbols never interpose on
each other and a missing symbol fails at load time; a module's own dependencies belong beside it,
found through a `$ORIGIN` run path of the module. The runtime's third-party libraries are found in
the loader's default directories; libraries installed elsewhere need `LD_LIBRARY_PATH`, or the
consumer's `-rpath-link` when it links the installed runtime. The shared library exports only the
`praenora` namespace through a version script and `--exclude-libs,ALL`. To look at an
installation:

```sh
readelf -d "${TMPDIR:-/tmp}/praenora-install/bin/praenora" | grep -E 'RPATH|RUNPATH'
ldd "${TMPDIR:-/tmp}/praenora-install/bin/praenora"
```

Thread names are cut to 15 bytes, so long step names are shortened in a debugger.

Process isolation in module hosts is verified on Linux x86-64 by CI; the restricted sandbox
profile of a module host is not available on Linux in this version (validation refuses
`sandbox: restricted` with `UNSUPPORTED`).

The control API's Unix socket paths hold at most 107 bytes (`sun_path`), and a socket is given mode
0600 with `fchmod()` before `bind()`, so its file is made with that mode (less the umask). The
control API and the OTLP export are not yet verified on Linux: CI has not built them yet
([specs/control-api.md](specs/control-api.md#platforms)).

## Windows

CI builds and tests the `ci-windows-x64-release` and `ci-windows-x64-debug` presets, which inherit
`windows-x64-release` and `windows-x64-debug`, with Visual Studio 2022, and their install tests run
the installed tool, before and after the installation is moved
([The CI presets](#the-ci-presets)). The rest of this section is not yet verified: libraries from
vcpkg or conda-forge, `dev*` presets that inherit the Windows presets, and the commands below.

**Toolchain and dependencies.** Visual Studio 2022 with the C++ workload, CMake 3.24 or newer, and
the libraries from vcpkg or conda-forge (json-schema-validator built from source where they do not
provide it, as in [json-schema-validator](#json-schema-validator), and cpp-httplib installed
header-only, as in [cpp-httplib](#cpp-httplib)). Keep the shared MSVC runtime
library (`/MD`, CMake's default) and the dynamic vcpkg triplets (such as `x64-windows`): the public
C++ API passes standard-library types between the runtime DLL and its users, which only works when
both use the same C runtime. Builds use `_WIN32_WINNT=0x0A00` (Windows 10).

**Presets.** `windows-x64-debug` and `windows-x64-release` use the Visual Studio 17 2022 generator
for x64, which is multi-configuration: the build and test presets choose `Debug` or `Release`. Make
the `dev*` presets inherit them in `CMakeUserPresets.json`. `ci-windows-x64-release` and
`ci-windows-x64-debug` inherit them and build against the dependency bundle of the same
configuration, whose MSVC runtime library (`/MD` or `/MDd`) matches it
([The CI presets](#the-ci-presets)). Executables and DLLs go to `build\<preset>\bin\<config>`, the
example modules to `build\<preset>\modules\<config>`:

```bat
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
build\dev\bin\Debug\praenora.exe version
```

**DLL search.** Modules are loaded with `LoadLibraryExW` and the flags
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`. The DLLs a module needs are
searched for in the module's own directory, the application's directory, System32 and the
directories its manifest lists in `library.dependency_paths` (added with `AddDllDirectory` for the
load only), never in the current directory or `PATH`, which rules out DLL planting. In the build
tree and in an installation, the runtime's DLL and the DLLs of its third-party libraries are copied
beside the tool and the test executables, so nothing depends on `PATH`. To look at what an
executable needs:

```bat
dumpbin /dependents build\dev\bin\Debug\praenora.exe
```

**Installation.** `bin\` holds `praenora.exe`, `praenora_runtime.dll` and the third-party DLLs;
`lib\` the import library `praenora_runtime.lib`. The installation's module directory is computed as
`<directory of praenora_runtime.dll>\..\lib\praenora\modules`, which matches only the default `bin`
and `lib` directory names.

**Other notes.**

- The tool reads its arguments as UTF-16 and converts them to UTF-8, sets the console's output code
  page to UTF-8, and writes standard output and error in binary mode, so lines end in `\n`.
- Ctrl+C, Ctrl+Break and closing the console stop a run; for a closing console the tool waits up to
  4 s for the run to wind down.
- Sanitizer presets exist for GCC and Clang only.
- The Python extension is a `.pyd` file with the runtime's DLLs beside it ([python.md](python.md)).
- Process isolation, and with it the restricted sandbox profile, is not available on Windows in this
  version: validation refuses a step in mode `process` (`UNSUPPORTED`), and the module host is not
  built.
- The control API listens on TCP only: `--listen unix:<path>` is refused by the address parser
  with `UNSUPPORTED`, which the tool reports as a usage error (`INVALID_ARGUMENT`, exit 2), and so
  is `praenora status --server unix:<path>`. A TCP listener
  takes its port exclusively (`SO_EXCLUSIVEADDRUSE`), and its socket is not inherited by the
  processes the tool starts. Ctrl+C, Ctrl+Break and closing the console shut `praenora serve`
  down, as they stop a run. Token files' modes are not checked. The listener tests that start the
  tool are POSIX only (they send it signals); the listener and the client have tests of their own
  ([specs/control-api.md](specs/control-api.md#platforms), [testing.md](testing.md#listeners)).
  Not yet verified.

## References

- [ADR-0019](adr/ADR-0019-platform-loading-rpath-dll-search.md): loading, run paths and the DLL
  search policy.
- [ADR-0017](adr/ADR-0017-dependency-management.md): dependencies and their minimum versions.
- [open-questions.md](open-questions.md): OQ-12 (minimum versions on Linux and Windows) and OQ-13
  (distribution, signing).
