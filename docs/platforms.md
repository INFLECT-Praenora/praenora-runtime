# Platforms

`praenora-runtime` is written for Linux x86-64, Windows x86-64 and macOS arm64, with presets,
portable code and commands for each. This build has been verified on macOS arm64 only; the Linux
and Windows commands below are given as they should work and are marked "not yet verified".

## Verification status

| Platform | Compiler | Status |
|---|---|---|
| macOS arm64 (macOS 15) | AppleClang 17 | Verified: configure, build without warnings, and every test in the `dev` (Debug), `dev-asan` and `dev-tsan` builds; an installation, moved; the Python bindings with CPython 3.12. |
| Linux x86-64 | GCC 9+, Clang 10+ | Presets and portable code; not yet verified. |
| Windows x86-64 | MSVC 19.29+ (Visual Studio 2022) | Presets and portable code; not yet verified. |

Not yet verified on any platform: downloading dependencies in fetch mode (every dependency was
installed where this was checked, so nothing was downloaded), and with it the fetched projects'
option names that keep their libraries static and their install rules off
([ADR-0017](adr/ADR-0017-dependency-management.md)); building a Python wheel; Python 3.10 and 3.11;
and multi-configuration generators (Visual Studio, Xcode, Ninja Multi-Config). macOS x86-64 is not
a target.

## Common to every platform

- **Compilers.** C++17 without extensions and C99. Configuration stops below GCC 9, Clang 10,
  AppleClang 13 or MSVC 19.29 (Visual Studio 2019 16.10). Warnings are `-Wall -Wextra -Wpedantic`
  for GCC and Clang and `/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc` for MSVC;
  `PRAENORA_WARNINGS_AS_ERRORS=ON` adds `-Werror` or `/WX`.
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

## macOS

**Dependencies.** Not yet verified: this command downloads packages, which the environment where
this guide was checked does not allow. Homebrew provides everything except json-schema-validator,
which is built from source ([json-schema-validator](#json-schema-validator)).

```sh
brew install cmake ninja swig nlohmann-json yaml-cpp spdlog googletest
```

**Presets.** `macos-arm64-debug`, `macos-arm64-release`, `macos-arm64-asan` and `macos-arm64-tsan`
(Ninja, `CMAKE_OSX_ARCHITECTURES=arm64`). The local `dev*` presets inherit them; in `dev-local`,
`CMAKE_PREFIX_PATH` lists Homebrew's prefix and the json-schema-validator prefix
([developer-guide.md](developer-guide.md#local-presets)).

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
`@rpath/libpraenora_runtime.0.1.dylib`; the runtime itself has no run path and finds its
third-party libraries by their own install names (Homebrew's are absolute). Modules are `MODULE`
libraries with the suffix `.so`; a module's own dependencies belong beside it, found through an
`@loader_path` run path of the module. On an installation:

```bash
PREFIX="${TMPDIR:-/tmp}/praenora-install"
cmake --install build/dev --prefix "$PREFIX"
otool -l "$PREFIX/bin/praenora" | grep -A2 LC_RPATH
otool -D "$PREFIX/lib/libpraenora_runtime.dylib"
```

shows `path @loader_path/../lib` and `@rpath/libpraenora_runtime.0.1.dylib`. (Inside some
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

**Other notes.**

- Worker threads are named `praenora:<step>` by the threads themselves, because macOS names only
  the calling thread.
- AddressSanitizer keeps an unloaded instrumented library's globals registered, so the tests of an
  AddressSanitizer build keep module libraries loaded ([testing.md](testing.md#sanitizer-builds)).

## Linux

Not yet verified: nothing in this section has been built or run.

**Dependencies.** Distribution packages provide everything except json-schema-validator, which is
built from source ([json-schema-validator](#json-schema-validator)):

```sh
sudo apt install cmake ninja-build g++ nlohmann-json3-dev libyaml-cpp-dev libspdlog-dev libgtest-dev swig python3-dev
sudo dnf install cmake ninja-build gcc-c++ json-devel yaml-cpp-devel spdlog-devel gtest-devel swig python3-devel
```

The first line is for Debian and Ubuntu (24.04 or newer), the second for Fedora.

**Presets.** `linux-x64-debug`, `linux-x64-release`, `linux-x64-asan` and `linux-x64-tsan` (Ninja).
In `CMakeUserPresets.json`, make the `dev*` configure, build and test presets inherit the matching
`linux-x64-*` presets instead of `macos-arm64-*`, then:

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

## Windows

Not yet verified: nothing in this section has been built or run.

**Toolchain and dependencies.** Visual Studio 2022 with the C++ workload, CMake 3.24 or newer, and
the libraries from vcpkg or conda-forge (json-schema-validator built from source where they do not
provide it, as in [json-schema-validator](#json-schema-validator)). Keep the shared MSVC runtime
library (`/MD`, CMake's default) and the dynamic vcpkg triplets (such as `x64-windows`): the public
C++ API passes standard-library types between the runtime DLL and its users, which only works when
both use the same C runtime. Builds use `_WIN32_WINNT=0x0A00` (Windows 10).

**Presets.** `windows-x64-debug` and `windows-x64-release` use the Visual Studio 17 2022 generator
for x64, which is multi-configuration: the build and test presets choose `Debug` or `Release`. Make
the `dev*` presets inherit them in `CMakeUserPresets.json`. Executables and DLLs go to
`build\<preset>\bin\<config>`, the example modules to `build\<preset>\modules\<config>`:

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

## References

- [ADR-0019](adr/ADR-0019-platform-loading-rpath-dll-search.md): loading, run paths and the DLL
  search policy.
- [ADR-0017](adr/ADR-0017-dependency-management.md): dependencies and their minimum versions.
- [open-questions.md](open-questions.md): OQ-12 (minimum versions on Linux and Windows) and OQ-13
  (distribution, signing).
