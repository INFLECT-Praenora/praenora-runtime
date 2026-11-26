# Platforms

`praenora-runtime` is written for Linux x86-64, Windows x86-64 and macOS arm64, with presets,
portable code and commands for each. It is verified on macOS arm64 locally and by CI, and by CI
on Linux x86-64 and Windows x86-64 with the CI presets
([releasing.md](releasing.md#what-the-runs-verified)); the other Linux and Windows commands below
are given as they should work and are marked "not yet verified".

## Verification status

| Platform | Compiler | Status |
|---|---|---|
| macOS arm64 (macOS 15; macOS 26 in CI) | AppleClang 17; AppleClang 21 in CI | Verified: configure, build without warnings, and every test in the `dev` (Debug), `dev-release`, `dev-asan` and `dev-tsan` builds; an installation, moved; the Python bindings with CPython 3.12; a Python wheel built offline and installed into a fresh virtual environment ([python.md](python.md#building-a-wheel)); the roadrunner provider with libRoadRunner 2.11.0 and LLVM 13.0.1, every test in a Debug build and the labels `unit`, `contract`, `negative`, `lifecycle`, `integration` and `install` in a ThreadSanitizer build ([The roadrunner provider](#the-roadrunner-provider)); the antimony provider with libAntimony 3.3.0 built from source, every test in a Debug build and in a Debug build with both providers, and the labels `unit`, `contract` and `integration` in a ThreadSanitizer build ([The antimony provider](#the-antimony-provider)); process isolation in module hosts, with the shared compiled models and the restricted sandbox profile, whose tests run outside any other sandbox ([specs/process-isolation.md](specs/process-isolation.md)); the `ci-macos-arm64-release` preset against a dependency bundle built locally, with installed yaml-cpp, spdlog and GoogleTest instead of fetched ones ([The CI presets](#the-ci-presets)); and by CI, the `ci-macos-arm64-release` preset with AppleClang 21 on `macos-26`, against the dependency bundle, with fetched yaml-cpp, spdlog and GoogleTest: every test, those of the restricted sandbox profile included, and the archive ([releasing.md](releasing.md#what-the-runs-verified)). The control API and the OTLP export, with cpp-httplib 0.67.0 installed header-only, locally: the `dev`, `dev-release`, `dev-asan` and `dev-tsan` builds without warnings and their tests, the listener tests ([testing.md](testing.md#listeners)) from a shell outside any sandbox in the `dev` and `dev-tsan` builds; the `ci-macos-arm64-release` preset with installed packages, its tests outside any sandbox; and a wheel built offline with cpp-httplib (built and imported; it holds no server, and its export test skips where listeners are refused). By CI, the workflow's run 45165537866 built and tested them on every platform key, with cpp-httplib downloaded at its pinned commit, its license installed, and the listener tests run with `PRAENORA_TESTS_REQUIRE_LISTENERS=1` ([releasing.md](releasing.md#what-the-runs-verified)); not yet verified: the distributions' and vcpkg's cpp-httplib packages ([cpp-httplib](#cpp-httplib)), and the OpenAPI document checked with official OpenAPI tooling. The Python environment (`PRAENORA_WITH_PYTHON_ENVIRONMENT`, with the released macOS bundle of praenora-runtime-pyenv 0.1.0, CPython 3.12.16, built with AppleClang 21), locally with AppleClang 17, offline, each build from a clean tree and its tests from a shell outside any sandbox with `PRAENORA_TESTS_REQUIRE_LISTENERS=1`, so that the listener tests and those of the restricted sandbox profile ran ([developer-guide.md](developer-guide.md#the-python-environment)): configure, build without warnings and every test of `dev-python` (Debug) and `dev-python-release`, 1344 tests each, none skipped; of `dev-python-asan` and `dev-python-tsan`, 1342 each, without a sanitizer report, with `python.unittest` disabled as in every sanitizer tree and, under AddressSanitizer, `PythonModuleRun.SourcesChangedInOneProcessRefused` skipped, since that tree keeps module libraries loaded ([testing.md](testing.md#sanitizer-builds)); and of `dev-python-engines`, with the roadrunner and antimony providers too, 1484, none skipped, among them a roadrunner step beside a Python step that imports the environment's `roadrunner` package, in one process, the provider loaded first, as a run loads it. Among the tests of each, the label `install`: an installation with the environment and the provider `python`, moved to a directory whose name holds a space and a non-ASCII character, its Python steps run there in the tool's process and in module hosts, and its archive, made with cpack and checked file by file against the installation ([developer-guide.md](developer-guide.md#the-installation)). The builds without the option, the same way: configure, build without warnings and every test of `dev` and `dev-release`, 1246 each, none skipped, of `dev-asan` and `dev-tsan`, 1244 each, without a sanitizer report (`python.unittest` disabled), and of `dev-engines`, with both providers, 1384, none skipped. The `ci-macos-arm64-release` preset, with the bundle downloaded through the pin: configure, build and every test, 1484, none skipped, with Homebrew's yaml-cpp, spdlog and GoogleTest and a cpp-httplib installed locally, in place of fetched ones, and a dependency bundle built locally in place of the released one; its build printed 98 linker notes that Homebrew's libraries were built for a newer macOS than the preset's deployment target, 14.0 ([The CI presets](#the-ci-presets)). A Python wheel built offline without warnings, which holds `praenora.sdk` beside `praenora.runtime`, installed into a fresh virtual environment whose Python imports both ([python.md](python.md#building-a-wheel)). And the pinned release, downloaded for `macos-arm64-release` with `cmake/praenora_fetch_runtime_pyenv.cmake` and checked against the pin, without a token, a second run reusing it. Not yet verified: the CI presets with the environment ([The CI presets](#the-ci-presets)), the workflow's check of an archive's libraries on an archive with the environment ([releasing.md](releasing.md#the-workflow)), and the other load order beside the roadrunner provider. |
| Linux x86-64 | GCC 9+, Clang 10+ | Presets, a CI preset and portable code. Verified by CI ([releasing.md](releasing.md#what-the-runs-verified)): the `ci-linux-x64-release` preset with GCC 13.3 on `ubuntu-24.04`, against the dependency bundle: configure, build without warnings, and every test, with both providers, process isolation in module hosts, the Python bindings with CPython 3.12, the install tests and the archive (15 tests skip: the 12 of the restricted sandbox profile, which exists only on macOS, and 3 that need what the runner lacks). Not yet verified: the `linux-x64-debug`, `linux-x64-asan` and `linux-x64-tsan` presets, Clang, older GCC, and the distributions' packages ([Linux](#linux)). The control API and the OTLP export are built and tested there too. Not yet verified: the Python environment, not yet compiled on Linux, its installation among them. |
| Windows x86-64 | MSVC 19.29+ (Visual Studio 2022) | Presets, CI presets and portable code. Verified by CI ([releasing.md](releasing.md#what-the-runs-verified)): the `ci-windows-x64-release` and `ci-windows-x64-debug` presets with MSVC 19.44 (Visual Studio 2022) on `windows-2022`, against the dependency bundles: configure, build without warnings, and every test, with both providers, the Python bindings with CPython 3.12, the install tests and the archives (5 tests skip, which Windows cannot run). Process isolation is not available on Windows in this version. Not yet verified: older MSVC, and libraries from vcpkg or conda-forge ([Windows](#windows)). The control API (TCP only on Windows) and the OTLP export are built and tested there too. Not yet verified: the Python environment, not yet compiled on Windows, its installation and the application manifest that allows long paths ([Windows](#windows)). |

Fetch mode is verified by CI for yaml-cpp, spdlog and GoogleTest, which the CI presets download
and build ([The CI presets](#the-ci-presets)), and the fetched yaml-cpp and spdlog end up static
libraries: the archive check finds no library of theirs. The CI presets download cpp-httplib too,
which CI verified ([cpp-httplib](#cpp-httplib)). Which of the settings of
[ADR-0017](adr/ADR-0017-dependency-management.md) brings that about, and that the fetched
projects' install rules are off, no test shows. Not yet verified on any platform: downloading
nlohmann/json in fetch mode (CI takes it from the bundle); building a
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

## jsoncons

Every platform needs jsoncons 1.10.0, exactly, the library that validates JSON Schema (draft-07 and
2020-12). It is header-only, so nothing is built and a sanitizer build instruments it with the rest
of the runtime. Homebrew and conda-forge do not package it; vcpkg's port (1.9.0 when this was
checked) and the Debian and Ubuntu packages (1.3.2 in Debian 13, 1.5.0 in Ubuntu 26.04) are not
1.10.0, the only version the runtime was tested with: the validator's messages and reporter
changed between releases, and the adapter reads message texts (`find_package` asks for exactly
1.10.0, and a test pins the texts). Two ways to get it:

- Configure with `-DPRAENORA_FETCH_DEPENDENCIES=ON`. `find_package(jsoncons CONFIG)` is tried first;
  otherwise CMake downloads the sources at the commit that
  [`cmake/PraenoraDependencies.cmake`](../cmake/PraenoraDependencies.cmake) pins (v1.10.0) and
  defines an interface target for their include directory. The project is not added to the build,
  so it installs nothing and builds no tests.
- Install it into a prefix of your own and list that prefix in `CMAKE_PREFIX_PATH`
  ([developer-guide.md](developer-guide.md#local-presets)). Not yet verified on this host's
  platforms other than Windows: the commands download the sources.

```sh
git clone --branch v1.10.0 https://github.com/danielaparker/jsoncons.git "${TMPDIR:-/tmp}/jsoncons"
cmake -S "${TMPDIR:-/tmp}/jsoncons" -B "${TMPDIR:-/tmp}/jsoncons/build" \
    -DJSONCONS_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$HOME/.local/opt/praenora-deps"
cmake --install "${TMPDIR:-/tmp}/jsoncons/build"
```

The dependency bundle of praenora-runtime-deps does not hold jsoncons yet, so the `ci-*` presets
(which set `PRAENORA_FETCH_DEPENDENCIES=ON`) fetch it.

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
CI verified the download on every platform key (run 45165537866).

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
    "CMAKE_PREFIX_PATH": "/opt/homebrew;<jsoncons prefix>;<RR>;<RR>/lib/cmake;<RR>/lib/cmake/Poco;<LLVM>",
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
    "CMAKE_PREFIX_PATH": "/opt/homebrew;<jsoncons prefix>;<RR>;<RR>/lib/cmake;<ANT>",
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
above. They take nlohmann/json (from the bundle's `json-schema-validator` directory),
libRoadRunner with libroadrunner-deps' libraries, LLVM and libAntimony from a dependency bundle of
[praenora-runtime-deps](https://github.com/INFLECT-Praenora/praenora-runtime-deps), one per
platform key, at the release that
[`cmake/PraenoraRuntimeDepsPin.cmake`](../cmake/PraenoraRuntimeDepsPin.cmake) pins
([developer-guide.md](developer-guide.md#using-a-dependency-bundle)), and build yaml-cpp, spdlog,
GoogleTest and cpp-httplib (header-only) from their pinned commits with FetchContent. Each key has
a configure, build and test preset in `CMakePresets.json`, which builds both engine providers, the
Python environment with the provider `python`, the Python bindings and the tests, with warnings as
errors
([developer-guide.md](developer-guide.md#ci-presets) lists what they set):

| Preset | Bundle | Inherits | Status |
|---|---|---|---|
| `ci-linux-x64-release` | `linux-x64-release` | `linux-x64-release` | Verified by CI: configure, build without warnings and every test, with GCC 13.3 and fetched yaml-cpp, spdlog and GoogleTest. With fetched cpp-httplib, the control API and the OTLP export too. |
| `ci-macos-arm64-release` | `macos-arm64-release` | `macos-arm64-release`, with `CMAKE_OSX_DEPLOYMENT_TARGET` 14.0, the bundle's | Verified by CI: configure, build without warnings and every test, with AppleClang 21 and fetched yaml-cpp, spdlog and GoogleTest. Locally also against a bundle built with AppleClang 17, with Homebrew's yaml-cpp, spdlog and GoogleTest instead of fetched ones, and so with their deployment target, 15.0, and since cpp-httplib is a dependency with it installed header-only: configure, build and every test, from a shell outside any sandbox ([developer-guide.md](developer-guide.md#ci-presets)). With fetched cpp-httplib, verified by CI too. |
| `ci-windows-x64-release` | `windows-x64-release` | `windows-x64-release`, with `CMAKE_TRY_COMPILE_CONFIGURATION` Release | Verified by CI: configure, build without warnings and every test, with MSVC 19.44 and fetched yaml-cpp, spdlog and GoogleTest. With fetched cpp-httplib, the control API and the OTLP export too. |
| `ci-windows-x64-debug` | `windows-x64-debug` | `windows-x64-debug`, with `CMAKE_TRY_COMPILE_CONFIGURATION` Debug | Verified by CI, as for Release; with fetched cpp-httplib, the control API and the OTLP export too (run 45165537866, [releasing.md](releasing.md#what-the-runs-verified)). |

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

The presets also turn on `PRAENORA_WITH_PYTHON_ENVIRONMENT`: they build the provider `python`,
the Python core and the Python modules of the tests and the example against a released Python
environment bundle of
[praenora-runtime-pyenv](https://github.com/INFLECT-Praenora/praenora-runtime-pyenv), at the
release that [`cmake/PraenoraRuntimePyenvPin.cmake`](../cmake/PraenoraRuntimePyenvPin.cmake) pins
(0.1.0), which they use as it is and whose directory the environment variable
`PRAENORA_RUNTIME_PYENV` names; a configuration with one of these presets stops when it does not
name a bundle ([developer-guide.md](developer-guide.md#ci-presets)). The statuses above came
before it: no CI run has yet built or tested these presets with the Python environment (not yet
verified). Locally, `ci-macos-arm64-release` was configured and built from a clean tree, offline,
with the released macOS bundle that `cmake/praenora_fetch_runtime_pyenv.cmake` downloaded through
the pin, and all its 1484 tests passed, none skipped, from a shell outside any sandbox with the test
preset's `PRAENORA_TESTS_REQUIRE_LISTENERS=1` ([Verification status](#verification-status)). It
differed from CI in its dependencies: Homebrew's yaml-cpp, spdlog and GoogleTest and a cpp-httplib
installed locally, in place of fetched ones, and a dependency bundle built and installed locally in
place of the released one. With Homebrew's libraries and the preset's deployment target, 14.0, its
build printed 98 warnings, all linker notes that Homebrew's fmt, spdlog, yaml-cpp and GoogleTest
were built for a newer macOS; CI builds those from their pinned sources.

## macOS

**Dependencies.** Not yet verified: this command downloads packages, which the environment where
this guide was checked does not allow. Homebrew provides everything except jsoncons, which
configuring with `PRAENORA_FETCH_DEPENDENCIES=ON` downloads ([jsoncons](#jsoncons)), and
cpp-httplib, installed header-only ([cpp-httplib](#cpp-httplib)).

```sh
brew install cmake ninja swig nlohmann-json yaml-cpp spdlog googletest
```

**Presets.** `macos-arm64-debug`, `macos-arm64-release`, `macos-arm64-asan` and `macos-arm64-tsan`
(Ninja, `CMAKE_OSX_ARCHITECTURES=arm64`). The local `dev*` presets inherit them; in `dev-local`,
`CMAKE_PREFIX_PATH` lists Homebrew's prefix and the jsoncons prefix
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
sandboxes, Apple's developer tools also print a harmless `xcrun_db` cache warning.) With the Python
environment, the installed Python core, `lib/praenora/providers/python/praenora-python-core.dylib`,
has the one run path `@loader_path/../../python-environment/python/lib`, where the installation's
environment keeps `libpython3.12.dylib` (install name `@rpath/libpython3.12.dylib`), and the
provider `python`'s library has none (in a sanitizer build the compiler adds `@executable_path` and
the toolchain's sanitizer runtime directory to both).

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

**Dependencies.** Distribution packages provide everything except jsoncons 1.10.0, which
configuring with `PRAENORA_FETCH_DEPENDENCIES=ON` downloads ([jsoncons](#jsoncons)), and
cpp-httplib, installed header-only ([cpp-httplib](#cpp-httplib)):

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

With the Python environment, the installed Python core has the run path
`$ORIGIN/../../python-environment/python/lib`, where the installation's environment keeps
`libpython3.12.so.1.0`, and `praenora` and `praenora-module-host` find the provider `python`'s
environment file below the installation's library directory, `lib64` included (not yet verified on
Linux).

Thread names are cut to 15 bytes, so long step names are shortened in a debugger.

Process isolation in module hosts is verified on Linux x86-64 by CI; the restricted sandbox
profile of a module host is not available on Linux in this version (validation refuses
`sandbox: restricted` with `UNSUPPORTED`).

The control API's Unix socket paths hold at most 107 bytes (`sun_path`), and a socket is given mode
0600 with `fchmod()` before `bind()`, so its file is made with that mode (less the umask). CI
builds and tests the control API and the OTLP export on Linux ([specs/control-api.md](specs/control-api.md#platforms)).

## Windows

CI builds and tests the `ci-windows-x64-release` and `ci-windows-x64-debug` presets, which inherit
`windows-x64-release` and `windows-x64-debug`, with Visual Studio 2022, and their install tests run
the installed tool, before and after the installation is moved
([The CI presets](#the-ci-presets)). The rest of this section is not yet verified: libraries from
vcpkg or conda-forge, `dev*` presets that inherit the Windows presets, and the commands below.

**Toolchain and dependencies.** Visual Studio 2022 with the C++ workload, CMake 3.24 or newer, and
the libraries from vcpkg or conda-forge (jsoncons 1.10.0 downloaded or installed as in
[jsoncons](#jsoncons), and cpp-httplib installed header-only, as in
[cpp-httplib](#cpp-httplib)). Keep the shared MSVC runtime
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
- `praenora.exe` has, in every Windows build, an application manifest
  (`src/cli/praenora.manifest`) that declares it long-path aware (`longPathAware`), and so do the
  test programs that run the Python environment in their process. It matters for the environment,
  whose files lie up to about 140 characters below its directory: Windows lets a process open
  longer paths than 260 characters only when it declares that and long paths are enabled in the
  system (the setting `LongPathsEnabled`;
  [user-guide.md](user-guide.md#the-python-environment-in-an-installation)). The manifest declares
  no code page: the tool already takes its arguments as UTF-16 and calls the system's wide
  functions. With the environment, the Python core's DLL is installed without its import library,
  and Python steps run in the tool's process only, as process isolation is not available. Not yet
  verified.
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
  ([specs/control-api.md](specs/control-api.md#platforms), [testing.md](testing.md#listeners)),
  which CI runs ([releasing.md](releasing.md#what-the-runs-verified)).
- A status file cannot be replaced while another handle holds it open without
  `FILE_SHARE_DELETE`, as the C++ library's `std::ifstream` opens it (`praenora status` reading the
  file, for example, or a virus scanner). The writer tries the replacement up to ten times, 10 ms
  apart, then fails that write, which becomes the workflow's last error and makes the health
  `degraded`, and writes again at its next change or interval
  ([specs/observability.md](specs/observability.md#status-file)). A reader that keeps opening the
  file while a run writes it can therefore degrade the run's health.

## References

- [ADR-0019](adr/ADR-0019-platform-loading-rpath-dll-search.md): loading, run paths and the DLL
  search policy.
- [ADR-0017](adr/ADR-0017-dependency-management.md): dependencies and their minimum versions.
- [open-questions.md](open-questions.md): OQ-12 (minimum versions on Linux and Windows) and OQ-13
  (distribution, signing).
