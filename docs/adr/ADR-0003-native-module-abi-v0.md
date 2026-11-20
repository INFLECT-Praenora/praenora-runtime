# ADR-0003: Native module ABI v0: single entry point returning a versioned function table

- Status: Accepted (scope noted in Decision)
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Modules are independent shared libraries written by different teams, possibly with different compilers and C++ standard libraries. The host must load them with `dlopen`/`LoadLibraryExW` and drive them without sharing C++ types, exceptions or allocators.

## Decision

Status split: the shape below is Accepted (required by the project); the exact spellings and details are Proposed and are specified in [docs/specs/module-abi.md](../specs/module-abi.md).

Accepted shape:
- A module exports exactly one `extern "C"` symbol, `praenora_module_get_api(uint32_t abi_major)`, which returns a pointer to a static, versioned function table, or NULL if that major is unsupported.
- Every table begins with a generation-independent header (`abi_major`, `abi_minor`, `struct_size`, `reserved`).
- No C++ standard-library types and no exceptions cross the ABI.

Proposed details:
- The host accepts a table only if the pointer is non-NULL, `header.abi_major == 0`, `header.reserved == 0`, `header.struct_size` is at least the v0 baseline, and every required function pointer is non-NULL. Otherwise the error is `API_TABLE_INVALID`, or `ABI_INCOMPATIBLE` when the major differs.
- ABI types are fixed-width integers, `size_t`, and object and function pointers. No `enum`, `bool` or C++ types appear in ABI structs. All strings are UTF-8 and length-delimited.
- Minor evolution is append-only: new module functions go at the end of `praenora_module_api_v0`, new host callbacks at the end of `praenora_host_api`, and `struct_size` tells each side which exist.
- A future generation adds a new table type with the same header; a module may serve several generations by switching on the requested major.
- The ABI version (`major.minor`, starting at 0.0) is owned by `include/praenora/sdk/abi.h` and is independent of package versions (see ADR-0014).

### Builder notes (Proposed)

- `status.h` holds the status type and codes, and `abi.h` includes it: `praenora_status` is `int32_t`; the general codes 0–12 and `PRAENORA_STATUS_MODULE_BASE` (10000) are macros; `praenora_status_name()` returns the stable name of codes 0–12 and `"UNKNOWN"` for any other value. The log levels `PRAENORA_LOG_TRACE` (0) to `PRAENORA_LOG_CRITICAL` (5) are macros in `abi.h`.
- `PRAENORA_CALL` is empty on every supported target. `PRAENORA_MODULE_EXPORT` is `__declspec(dllexport)` on Windows and `__attribute__((visibility("default")))` with GCC and Clang. `PRAENORA_EXTERN_C` is `extern "C"` in C++. The entry-point prototype is declared only when `PRAENORA_MODULE_IMPLEMENTATION` is defined, which `praenora_add_module()` does for modules; the host only looks the symbol up.
- `PRAENORA_MODULE_API_V0_BASE_SIZE` is `offsetof(praenora_module_api_v0, diagnostics)`; `PRAENORA_HOST_API_V0_BASE_SIZE` is frozen at the v0.0 layout, `offsetof(praenora_host_api, get_service) + sizeof(((praenora_host_api*)0)->get_service)`, which equals `sizeof(praenora_host_api)` in v0.0. Defined as `sizeof`, it would grow with the first callback a minor version appends, and a module built against those headers that checks `host->struct_size >= PRAENORA_HOST_API_V0_BASE_SIZE` would refuse every v0.0 host. The value and every layout are unchanged, so this is no ABI change and the ABI version stays 0.0. In the host API, `context` follows the three `uint32_t` fields directly, so 64-bit targets have 4 bytes of padding after `abi_minor`; no explicit padding field was added.
- The ABI version lines have the form `#define PRAENORA_ABI_MAJOR 0u`, which the build parses. The SDK and protocol versions are in the generated header `praenora/sdk/version.h` (`PRAENORA_SDK_VERSION_{MAJOR,MINOR,PATCH}`, `PRAENORA_SDK_VERSION_STRING`, `PRAENORA_PMP_VERSION_STRING`), produced from `cmake/PraenoraVersions.cmake`, so each version keeps a single source.
- Unit tests pin every field offset and both base sizes, from C++ and from a C99 translation unit compiled with the project's warnings, and check that the two views agree.
- The host copies a module's table in whole function pointers (`copy_module_table()` in `src/runtime/loader/loaded_module.hpp`, an interface addition): the bytes copied are those `header.struct_size` covers, up to the host's own table size, cut back to the end of the last function pointer covered completely. A `struct_size` that ends inside an optional function, which no compiler produces but a hand-built table can, therefore leaves that function NULL instead of a half-copied pointer that the host would call.
- The function-pointer declarations of `abi.h` keep the usual C spelling `ret (PRAENORA_CALL *name)(...)`. The tree is formatted with `.clang-format` (`PointerAlignment: Left`), whose clang-format would write `ret(PRAENORA_CALL* name)(...)`, which reads like a call, so `/* clang-format off */` and `/* clang-format on */` enclose those declarations: the members of `praenora_sink`, `praenora_host_api` and `praenora_module_api_v0`, and `praenora_module_get_api_fn`. The test service's table in `tests/support/test_gate.h` is enclosed the same way.
- `PRAENORA_CALL` stays empty for ABI v0 on every target. Defining it as `__cdecl` on `_WIN32` is recorded as an option, to be decided before ABI v0 ships: it would pin the calling convention of the entry point, the table's functions and the host callbacks whatever default convention a compiler option selects, and changing the macro after v0 has shipped would break the ABI. The SDK's adapter, test kit and fixture modules already declare those functions with `PRAENORA_CALL`, and the ABI specification requires every module to do so, so either choice leaves module sources unchanged.
- Later: [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) runs a module instance in a module host, a process of its own, when its step asks for it, without changing the ABI: the module host loads the library with the same checks and calls the same function table with the same documents and host callbacks, and the runtime passes each call over a private channel that is internal to one runtime build. A module cannot tell where it runs other than by its process id. The out-of-process modules below are built that way, as an option per step, not as a second transport of the ABI.

## Alternatives considered

- **C++ interface classes with virtual methods.** Ties modules to a compiler and standard library ABI.
- **COM-style interface queries.** Flexible, but heavy for a JSON-in/JSON-out contract.
- **Per-function exports (one symbol per call).** Complicates evolution and symbol checking.
- **Out-of-process modules over a pipe or socket.** Strong isolation, but deferred (ADR-0006, OQ-5).

## Consequences

- One symbol to look up, and one structure to validate before any call.
- Several ABI generations can coexist in one library.
- The table is static data, so the host can check it before creating any instance.
- Module authors write C-compatible code, or use the optional C++ adapter (ADR-0004).

## References

- [docs/specs/module-abi.md](../specs/module-abi.md)
- [docs/specs/compatibility-policy.md](../specs/compatibility-policy.md)
- [dlopen(3)](https://man7.org/linux/man-pages/man3/dlopen.3.html)
- [LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
