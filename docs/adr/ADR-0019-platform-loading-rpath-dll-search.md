# ADR-0019: Platform loading, RPATH and Windows DLL search policy

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Loading arbitrary native libraries is sensitive to search paths and symbol visibility. The project requires dlopen and dlsym on POSIX and the LoadLibrary family on Windows, defined RPATH behavior on Linux and macOS, and defined DLL search behavior on Windows.

## Decision

- **POSIX:** `dlopen(abs_path, RTLD_NOW | RTLD_LOCAL)`, then `dlsym("praenora_module_get_api")`. `RTLD_LOCAL` keeps modules' symbols from interposing on each other.
- **Windows:** the UTF-8 path is converted to UTF-16 and loaded with `LoadLibraryExW(abs_path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`; the entry point comes from `GetProcAddress`. Searched: the module's directory, the application directory, System32 and `dependency_paths` added with `AddDllDirectory`. The current directory and `PATH` are not searched.
- **Sharing:** one library load per resolved manifest, reference-counted and unloaded after the last instance is destroyed. `PRAENORA_DEBUG_KEEP_LIBRARIES=1` disables unloading. Every loaded library's SHA-256 is computed and recorded.
- **Installed RPATH:** `praenora` gets `$ORIGIN/../lib` on Linux and `@loader_path/../lib` on macOS; the runtime dylib install name is `@rpath/...`. POSIX modules use `$ORIGIN` or `@loader_path` RPATHs for their own dependencies.
- **Windows layout:** executables and DLLs build into `<build>/bin`; third-party DLLs are copied with `$<TARGET_RUNTIME_DLLS>`.
- **Installation default module directory:** `<prefix>/lib/praenora/modules`, found relative to the runtime library through `dladdr` or `GetModuleHandleExW`.
- macOS hardened-runtime hosts need library validation disabled to load third-party modules (OQ-13).

### Builder notes (Proposed)

- An output directory that already contains `run-record.json` is refused with `IO_ERROR` unless overwrite is requested (`--overwrite`). Overwrite removes only Praenora-managed files (`run-record.json`, `outputs/`, `trace/`, `status.json`) and never other contents; log files are appended across runs and are never removed. To keep that true, a run whose log file, or the runtime's, lies at or under one of the managed paths of its output directory is refused with `CONFIG_INVALID`, naming the file, before anything is created or removed. The paths are compared resolved and absolute, and, where they exist, by asking the file system whether the log file or one of its ancestors is the same object as a managed path, which also catches another spelling on a case-insensitive file system and a hard link. This keeps re-runs from mixing old and new artifacts.
- `prepare_output_dir()` (`src/runtime/io/run_record.hpp`) creates a missing output directory. Without overwrite, an existing `run-record.json` is refused with the message "output directory already contains a run record; use overwrite", located at that file, and nothing changes. With overwrite it removes `outputs/`, `trace/`, `status.json` and, last, `run-record.json`, so a directory that could not be cleaned completely still shows that it holds a run. A directory without `run-record.json` holds no earlier run and is left as it is, even with overwrite. Its first part, `check_output_dir()`, checks without changing anything (a directory or missing; no earlier run unless overwrite), so `Runtime::open()` refuses an earlier run before it writes anything and removes it only once everything else that can fail has succeeded (ADR-0023). The engine checks again, without overwrite, before it writes its first record, so a caller that skips the preparation cannot replace an earlier run either.
- When `praenora-runtime` is the top-level project, executables (and Windows DLLs) build into `<build>/bin` and libraries and archives into `<build>/lib` on every platform. Multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) add a configuration directory: `<build>/bin/<config>` and `<build>/lib/<config>`.
- On Windows, `praenora_copy_runtime_dlls()` copies `$<TARGET_RUNTIME_DLLS>` next to the CLI and every test executable after each build; when the list is empty it does nothing.
- A build targets one architecture. Several `CMAKE_OSX_ARCHITECTURES` values are rejected at configuration time, because a module manifest declares a single `platform.arch`.
- Fixture modules build into `<build>/test-modules/<name>/` and example modules into `<build>/modules/<name>/`; each directory holds the library and its manifest. Multi-config generators put the configuration first (`<build>/test-modules/<config>/<name>/`), so `PRAENORA_TEST_MODULES_DIR` and `PRAENORA_EXAMPLE_MODULES_DIR` each name one directory per configuration that holds all of its modules.
- `LoadedModule::load()` checks that the library file exists (`LIBRARY_NOT_FOUND`) before it computes the digest, and compares the digest before the library is opened, so a mismatching library never runs its initializers. Errors are located at the manifest (`/library/path`, `/library/sha256`). That first step is `check_library_file()` (in `loader/loaded_module.hpp`), which returns the digest; validation uses it to report the same two errors, with the same messages, without loading anything.
- The module's function table is copied into a full-size v0 table; fields beyond the module's `struct_size` are NULL, so callers test optional functions for NULL. The entry point and `GetProcAddress` results are converted between data and function pointers with `memcpy`, which every compiler accepts without a warning.
- A pinned library's handle is dropped without `dlclose` or `FreeLibrary`, so it stays loaded for the rest of the process. `PRAENORA_DEBUG_KEEP_LIBRARIES` pins when its value is exactly `1`.
- Windows: paths get backslashes before `LoadLibraryExW` and `AddDllDirectory` (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` needs a fully qualified path); the flags name `LOAD_LIBRARY_SEARCH_USER_DIRS`, the directories added with `AddDllDirectory`, explicitly, although `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` includes them; the `dependency_paths` directories are added for the duration of one load only (process-wide while they are in place); `SetThreadErrorMode` suppresses error dialog boxes; `FormatMessageW` text is converted to UTF-8 and reported as `error <code>: <text>`. This code is not yet built or tested.
- `installation_module_dir()` comes from the binary that holds the runtime code, found with `dladdr()` or `GetModuleHandleExW()` and `GetModuleFileNameW()` on the address of an object inside it: the shared library normally, the test executable in white-box tests. Environment variables are read as UTF-8 (`GetEnvironmentVariableW` on Windows). These system calls live in `src/runtime/platform/{posix,windows}/` behind `platform/platform.hpp`.
- AddressSanitizer on macOS keeps an unloaded instrumented library's globals registered and their redzones poisoned, so a library loaded later at an overlapping address trips false reports, and the sanitizer crashes while describing them. Test runs of AddressSanitizer builds on macOS therefore set `PRAENORA_DEBUG_KEEP_LIBRARIES=1` (a CTest environment property): libraries stay loaded there, and unloading is tested by every other build, ThreadSanitizer included.
- Installation (`cmake/PraenoraInstall.cmake`, GNUInstallDirs): `praenora` goes to `${CMAKE_INSTALL_BINDIR}` with `INSTALL_RPATH` `@loader_path/../${CMAKE_INSTALL_LIBDIR}` on macOS and `$ORIGIN/../${CMAKE_INSTALL_LIBDIR}` on other ELF platforms; the runtime library goes to `${CMAKE_INSTALL_LIBDIR}`, keeps CMake's default install name `@rpath/libpraenora_runtime.0.1.dylib` and gets no RPATH of its own: installing removes the one the build tree gives it (with Homebrew, `/opt/homebrew/lib`). Its third-party libraries are found by their own install names (Homebrew's are absolute) or, on Linux, in the loader's default directories (not yet verified). Checked on macOS with `otool -l` and `otool -D` on an installation.
- Windows (not yet verified): the runtime's DLL goes to the bin directory, with the DLLs of its third-party libraries (`$<TARGET_RUNTIME_DLLS:praenora_runtime>`; nothing when they are all static), and its import library to the lib directory. The runtime's own list is needed because an executable's `$<TARGET_RUNTIME_DLLS>` names the runtime's DLL but not the libraries the runtime links privately (ADR-0017). For the same reason, the build tree now copies the runtime's third-party DLLs beside the runtime's DLL in `<build>/bin` (`praenora_copy_runtime_dlls(praenora_runtime)`); the tool and the black-box tests found them there before only because the white-box test executables, which link those libraries directly, copy them into the same directory.
- The example modules install into `${CMAKE_INSTALL_LIBDIR}/praenora/modules/<name>/` (library, manifest and test vectors), which is the installation's module directory: the runtime finds it beside its own library. On Windows it is computed as `<directory of the runtime's DLL>/../lib/praenora/modules`, which matches the installation only with the default `bin` and `lib` directory names. They form the install component `Examples`, which a default installation includes only with `PRAENORA_INSTALL_EXAMPLES=ON`; `cmake --install <build> --component Examples` installs them otherwise. Their rules are `praenora_add_module()`'s own (`INSTALL_DESTINATION`, `INSTALL_COMPONENT`, `INSTALL_EXCLUDE_FROM_ALL`; ADR-0009).
- An installation can be moved. The install test runs the installed `praenora` (`version --json`; `modules list --json` without `--module-path` and with `PRAENORA_MODULE_PATH` unset, which must list the example modules from the installation's module directory; and `modules check` on each installed example manifest, which loads the module from the installation and runs its contract checks with its installed test vectors), moves the whole installation to another directory whose name holds a space and a non-ASCII character (`moved prëfix`), and runs all three again there: the tool still loads its runtime library, and finds and loads the modules at their new place. The test also fails when an installed CMake file or manifest names the source or build tree.
- `PRAENORA_DEBUG_KEEP_LIBRARIES` is read from the process environment each time `LoadedModule::load()` opens a library (`src/runtime/loader/loaded_module.cpp`), whatever `RuntimeOptions::use_environment` says: a `Runtime` constructed with `use_environment = false` still pins every library it loads while the variable is `1`, in runs, in `validate --load` and in the contract checks. The runtime's other variables (`PRAENORA_MODULE_PATH`, `PRAENORA_CATALOG_PATH`, `PRAENORA_LOG_LEVEL`, `PRAENORA_LOG_FORMAT`) are read only with `use_environment`.

## Alternatives considered

- **`RTLD_GLOBAL`.** Lets modules interpose on each other's symbols.
- **`RTLD_LAZY`.** Defers unresolved-symbol failures to the first call, far from the load site.
- **Default Windows search order (`LoadLibraryW`).** Includes the current directory and `PATH`, which enables DLL planting.
- **Absolute RPATHs.** Break relocation of installed packages.

## Consequences

- Load failures surface at load time, with `dlerror()` or `FormatMessage` text in the diagnostic.
- Windows module dependencies must be declared in the manifest or sit beside the module.
- Library digests are available for the run record.

## References

- [docs/platforms.md](../platforms.md)
- [dlopen(3)](https://man7.org/linux/man-pages/man3/dlopen.3.html)
- [LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
- [docs/specs/manifest.md](../specs/manifest.md)
