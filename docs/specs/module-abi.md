# Module ABI v0

- **Version:** ABI 0.0 (`PRAENORA_ABI_MAJOR` 0, `PRAENORA_ABI_MINOR` 0)
- **Status:** Normative, MVP. The shape (one entry point returning a versioned function table) is
  Accepted; the details are Proposed ([ADR-0003](../adr/ADR-0003-native-module-abi-v0.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

This document specifies the binary interface between the Praenora runtime (the host) and a module:
a separately built shared library that implements one or more capabilities. It is written for module
authors and for implementers of another host. The C declarations in
[`abi.h`](../../include/praenora/sdk/abi.h) and [`status.h`](../../include/praenora/sdk/status.h)
are authoritative; this document states the rules around them.

Related specifications: the documents exchanged through the ABI are in [protocol.md](protocol.md),
the manifest that describes a module before it is loaded in [manifest.md](manifest.md), memory and
thread rules in [threading-and-ownership.md](threading-and-ownership.md), deadlines and cancellation
in [time-cancellation-timeouts.md](time-cancellation-timeouts.md), and status codes in
[error-taxonomy.md](error-taxonomy.md).

## Conventions

- The headers are C99 and C++ compatible. Besides `status.h`, `abi.h` depends only on `<stddef.h>`
  and `<stdint.h>`.
- Only fixed-width integers, `size_t`, object pointers and function pointers cross the boundary.
  ABI structs contain no `enum`, `bool` or C++ type. No C++ type, exception, `longjmp` or allocator
  ownership crosses the boundary: a module MUST NOT let an exception or a `longjmp` leave any ABI
  function.
- Strings are UTF-8 and length-delimited (a pointer and a size); they are not NUL-terminated. A
  zero-length string MAY come with a NULL pointer; a NULL pointer with a nonzero size is invalid.
- Documents crossing the ABI are UTF-8 JSON. A module MUST NOT depend on the key order or the
  formatting of a document it receives (the runtime writes canonical JSON, but another host need
  not).
- `PRAENORA_CALL` is the calling convention of the entry point, of every table function and of
  every host callback. It is empty for ABI v0 on every supported target. A module MUST declare its
  entry point and its table functions with it. Whether it becomes `__cdecl` on Windows is decided
  before ABI v0 ships ([ADR-0003](../adr/ADR-0003-native-module-abi-v0.md)).
- `PRAENORA_MODULE_EXPORT` exports the entry point from a library built with hidden symbol
  visibility (`__declspec(dllexport)` on Windows, `__attribute__((visibility("default")))` with GCC
  and Clang); `PRAENORA_EXTERN_C` gives it C linkage in C++. The entry-point prototype is declared
  only when `PRAENORA_MODULE_IMPLEMENTATION` is defined, which `praenora_add_module()` does; a host
  never links against a module, it looks the symbol up at run time.

| Macro | Value | Meaning |
|---|---|---|
| `PRAENORA_ABI_MAJOR`, `PRAENORA_ABI_MINOR` | `0u`, `0u` | The ABI version the headers implement. |
| `PRAENORA_MODULE_ENTRY_POINT` | `"praenora_module_get_api"` | The name of the exported symbol. |
| `PRAENORA_MODULE_API_V0_BASE_SIZE` | `offsetof(praenora_module_api_v0, diagnostics)` | The size of the required part of the v0 table. |
| `PRAENORA_HOST_API_V0_BASE_SIZE` | `offsetof(praenora_host_api, get_service)` plus the size of `get_service`, which is `sizeof(praenora_host_api)` in v0.0 | The smallest host API of ABI v0. Frozen at the v0.0 layout: callbacks that later minor versions append do not change it, so a module built against later headers still accepts a v0.0 host. |
| `PRAENORA_LOG_TRACE` ... `PRAENORA_LOG_CRITICAL` | 0 ... 5 | Log levels: trace, debug, info, warn, error, critical. |
| `PRAENORA_STATUS_OK` ... `PRAENORA_STATUS_DATA_INVALID` | 0 ... 12 | The general status codes ([error-taxonomy.md](error-taxonomy.md)). |
| `PRAENORA_STATUS_MODULE_BASE` | 10000 | The first module-defined status code. |

## Entry point

```c
PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL
praenora_module_get_api(uint32_t abi_major);
```

- A module library MUST export `praenora_module_get_api` with C linkage. It is the only symbol a
  host looks up.
- For `abi_major` 0 the function MUST return a pointer to the module's v0 table, a
  `praenora_module_api_v0` whose first field is the `praenora_api_header`. For a generation the
  module does not implement it MUST return NULL. A module MAY serve several generations by switching
  on `abi_major`.
- The function MUST be thread-safe and free of side effects, and MUST return the same table every
  time. The table is static data of the library: it MUST stay valid and unchanged while the library
  is loaded.
- The host calls it with `PRAENORA_ABI_MAJOR` (0) once each time it loads the library.

The module fills the header as follows: `abi_major` is the generation of the table (0), `abi_minor`
the minor version the module was compiled against (`PRAENORA_ABI_MINOR`), `struct_size` the `sizeof`
of its whole table, and `reserved` 0.

## Trust

A module is native code that runs inside the host's process, with the host's privileges: nothing
contains a module that misbehaves, and a crash in a module ends the process (out-of-process
isolation is OQ-5, [ADR-0006](../adr/ADR-0006-threading-model.md)). In a run, and with
`validate --load` or `modules inspect --load`, the runtime loads only libraries named by manifests
found on its module search paths ([manifest.md](manifest.md#discovery)); the contract checks load
the library of the manifest they are given. Discovery and validation read manifests without running
module code ([ADR-0009](../adr/ADR-0009-module-manifests.md)), and a digest pinned in the manifest
is the only integrity check (signatures are OQ-19). Whoever can write to a module search path can
therefore run code in every process that loads a module found there.

## Loading and table acceptance

The host checks a library in this order and refuses it at the first failure. Each error is located
in the module's manifest (file and JSON pointer); the codes are explained in
[error-taxonomy.md](error-taxonomy.md).

| # | Check | Error |
|---:|---|---|
| 1 | The library file named by the manifest exists and can be read; when the manifest pins `library.sha256`, the file has that SHA-256. Nothing of the library runs before this check. | `LIBRARY_NOT_FOUND`, `IO_ERROR`, `LIBRARY_DIGEST_MISMATCH` |
| 2 | The library loads: `dlopen(path, RTLD_NOW \| RTLD_LOCAL)` on POSIX, `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR \| LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` on Windows ([ADR-0019](../adr/ADR-0019-platform-loading-rpath-dll-search.md)). | `LIBRARY_LOAD_FAILED`, with the system's text |
| 3 | It exports `praenora_module_get_api`. | `ENTRY_POINT_MISSING` |
| 4 | `praenora_module_get_api(0)` returns a non-NULL pointer. | `API_TABLE_INVALID` |
| 5 | `header.abi_major` is 0. | `ABI_INCOMPATIBLE` |
| 6 | `header.reserved` is 0 and `header.struct_size` is at least `PRAENORA_MODULE_API_V0_BASE_SIZE`. | `API_TABLE_INVALID` |
| 7 | No required function pointer is NULL. | `API_TABLE_INVALID`, naming the functions |
| 8 | `describe()` returns OK and writes at most 1 MiB of JSON that satisfies the `pmp-describe` schema, with versions that are usable semantic versions. | `MODULE_IDENTITY_MISMATCH` |
| 9 | The `describe()` document agrees with the manifest on `name`, `version`, `abi.major` and the set of capabilities. | `MODULE_IDENTITY_MISMATCH`, naming every differing field |

The host copies the module's table into a table of its own `praenora_module_api_v0` size, in whole
function pointers: functions beyond the module's `struct_size`, and one that `struct_size` covers
only in part, read as NULL, and fields beyond the host's size (from a newer minor version) are
ignored. `describe()` runs on the thread that loads the library. A library is
loaded once per manifest and shared by every instance of the module. The manifest's own checks,
which come before loading (platform, ABI major, protocol version, capabilities), are in
[manifest.md](manifest.md).

## Function table

```c
typedef struct praenora_module_api_v0 {
    praenora_api_header header;
    /* required */
    praenora_status (PRAENORA_CALL *describe)(praenora_sink* out_json);
    praenora_status (PRAENORA_CALL *create)(const praenora_host_api* host, praenora_bytes create_json,
                                            praenora_instance** out_instance, praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *initialize)(praenora_instance*, praenora_bytes initialize_json,
                                                praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *invoke)(praenora_instance*, praenora_bytes invocation_json,
                                            praenora_sink* result_json, praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *finalize)(praenora_instance*, praenora_sink* error_json);
    void            (PRAENORA_CALL *destroy)(praenora_instance*);
    /* optional (may be NULL) */
    praenora_status (PRAENORA_CALL *diagnostics)(praenora_instance*, praenora_sink* out_json);
    praenora_status (PRAENORA_CALL *self_test)(const praenora_host_api* host, praenora_sink* report_json);
} praenora_module_api_v0;
```

| Function | Required | When the host calls it | Input (borrowed) | Output |
|---|---|---|---|---|
| `describe` | yes | Any time after load, from any thread; never with an instance. | none | The identity document (`pmp-describe`). |
| `create` | yes | Once per instance. | The create document (`pmp-create`) and the host API. | `*out_instance`. |
| `initialize` | yes | Once, after a successful `create`. | The initialize document (`pmp-initialize`). | none |
| `invoke` | yes | Once per tick. | The invocation document (`pmp-invocation`). | The result document (`pmp-result`). |
| `finalize` | yes | Once at shutdown, and only if `initialize` succeeded. | none | none |
| `destroy` | yes | Exactly once for every instance a `create` returned. | none | none; it cannot fail. |
| `diagnostics` | no | Between calls, on the instance's thread. The runtime asks for it in failure reports when the manifest declares `features.diagnostics`. | none | A module-defined JSON object. |
| `self_test` | no | By the contract checks only, never concurrently with another call into the library. | The host API. | `{"passed": bool, "checks": [{"name", "passed", "message"}]}`. |

- `describe` MUST be thread-safe and free of side effects. Its output is static data of the library.
- On success `create` MUST return `PRAENORA_STATUS_OK` and store a non-NULL instance in
  `*out_instance`; the instance is allocated with the module's own allocator. A `create` that
  returns OK without an instance is reported by the host as `INTERNAL`. When `create` stores an
  instance, even in a call that returns after its deadline, the host calls `destroy` for it later.
- `destroy` MUST free the instance and MUST have stopped every thread the module started for it
  before it returns.
- A call that fails MUST return a status other than OK and SHOULD write an error document to its
  `error_json` sink (see [Status codes and errors](#status-codes-and-errors)).
- The documents are specified field by field in [protocol.md](protocol.md). The host writes
  `pmp_version` (the runtime's PMP version) into every document it passes in; a result MAY carry
  `pmp_version`, and the error document has none.

## Lifecycle

The host keeps one state per instance (the `praenora::StepState` values). Status snapshots show it
per step, except that a step whose call failed is shown as `failed` from then on
([observability.md](observability.md)).

| State | Meaning | Next states |
|---|---|---|
| `loaded` | The library is loaded; the instance has not been created. | `created`, `failed` |
| `created` | `create` returned an instance. | `ready`, `failed`, `destroyed` (a stop came before `initialize`) |
| `ready` | `initialize` succeeded; the instance may be invoked. | `finalized`, `failed` |
| `busy` | A call into the instance is running. | the state the call's outcome leads to |
| `finalized` | `finalize` succeeded. | `destroyed` |
| `destroyed` | `destroy` returned. | none |
| `failed` | A `create`, `initialize` or `finalize` did not succeed. | `destroyed` (when an instance exists) |
| `unresponsive` | A call did not return within its deadline plus the cancellation grace. | none (terminal) |

A host MUST give these guarantees, and a module MAY rely on them:

1. All calls for one instance (`create` through `destroy`, `diagnostics` included) are made from one
   dedicated host thread, one at a time.
2. `invoke` is called only on a `ready` instance. An invocation that fails, times out or is
   cancelled leaves the instance `ready`, so it is still finalized.
3. `finalize` is called only for an instance whose `initialize` succeeded. When `initialize` fails,
   the host calls `destroy` without `finalize`.
4. At shutdown the host finalizes every `ready` instance, last created first, then destroys every
   instance that exists, last created first, then joins the instance threads, and only then unloads
   the libraries.
5. An `unresponsive` instance is never finalized or destroyed; its thread is detached and its library
   stays loaded for the rest of the process. The other instances of its library are kept the same
   way unless the manifest declares `features.concurrency: "instance"`
   ([threading-and-ownership.md](threading-and-ownership.md)).
6. A library is unloaded after its last instance is destroyed, unless it is pinned. Setting the
   environment variable `PRAENORA_DEBUG_KEEP_LIBRARIES` to `1` pins every library, which helps
   debuggers and leak checkers.

The runtime's engine loads, creates and initializes the steps one at a time in schedule order, then
invokes them tick by tick ([protocol.md](protocol.md#execution-semantics)). The rationale is in
[ADR-0004](../adr/ADR-0004-lifecycle-functions-and-documents.md).

## Host API

```c
typedef struct praenora_host_api {
    uint32_t struct_size;
    uint32_t abi_major, abi_minor;
    void* context;
    void        (PRAENORA_CALL *log)(void* context, int32_t level, const char* msg, size_t size);
    int32_t     (PRAENORA_CALL *log_enabled)(void* context, int32_t level);
    int32_t     (PRAENORA_CALL *cancel_requested)(void* context);
    const void* (PRAENORA_CALL *get_service)(void* context, const char* name, size_t name_size,
                                             uint32_t service_abi_major);
} praenora_host_api;
```

The host passes its host API to `create` (and to `self_test`). It is valid from the `create` call
until `destroy` returns. A module MAY call its callbacks from any of its threads while the instance
exists; the callbacks are thread-safe and never throw or `longjmp`.

| Field | Rule |
|---|---|
| `struct_size` | `sizeof(praenora_host_api)` as compiled into the host. A module MUST check that `struct_size` covers a field before it uses the field; a host of a later minor version appends callbacks. |
| `abi_major`, `abi_minor` | The host's ABI version. A module MAY refuse to create an instance for another major (the C++ adapter returns `PRAENORA_STATUS_UNSUPPORTED`). |
| `context` | Opaque and per instance. A module MUST pass it unchanged to every callback. |
| `log` | Records a message at a `PRAENORA_LOG_*` level. The runtime routes it to the step's logger `module.<step>` and adds the attributes `run.fingerprint`, `tick` (during an invocation), `step` and `module` (`name@version`). Levels below 0 count as trace and above 5 as critical. |
| `log_enabled` | Nonzero when a message at that level would be recorded, so a module can skip formatting it. |
| `cancel_requested` | Nonzero while the call in flight has passed its deadline or had a stop requested; zero at the start of every call. A module SHOULD poll it in long computations and then return `PRAENORA_STATUS_CANCELLED` ([time-cancellation-timeouts.md](time-cancellation-timeouts.md)). |
| `get_service` | The function table of a runtime service, a struct that starts with `praenora_api_header`, for a name and a major version; NULL when the service is not available. |

Runtime services come from service providers, separately built libraries that the runtime loads
when a run needs them ([runtime-services.md](runtime-services.md)). `get_service` returns, in this
order:

1. a provider service that the module's manifest declares in `requires.services` (module manifest
   format 0.2; [manifest.md](manifest.md#fields)) and that resolved for the run, required or
   optional;
2. a service built into the runtime, whatever the manifest declares; this version has no
   production one, and tests register test-only services;
3. NULL for everything else, including a provider service that the manifest does not declare, even
   when the run loaded its provider for another step, and every provider service when the manifest
   has format 0.1.

The rules are those of [runtime-services.md](runtime-services.md#visibility). Each lookup is logged
at debug level (event `service.lookup`). Once an instance is unresponsive, or kept after another
instance of its library became unresponsive, `get_service` returns NULL for every service. A
service's table, and what the module does with it, follow the service's own specification (for
`praenora.simulation@0`, [simulation-service.md](simulation-service.md), with the table in the
SDK's [`simulation.h`](../../include/praenora/sdk/simulation.h)); provider tables live until the
process ends
([ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md),
[ADR-0026](../adr/ADR-0026-runtime-service-providers.md)).

## Sinks

```c
typedef struct praenora_sink {
    void* context;
    praenora_status (PRAENORA_CALL *write)(void* context, const char* data, size_t size);
} praenora_sink;
```

- A module writes its output by calling `write(context, data, size)` zero or more times; the bytes
  are appended. The host copies them before `write` returns, so the module keeps ownership of its
  buffers.
- A sink is valid only during the call that received it. A module MUST NOT keep a sink, or write to
  it, after that call returns.
- `write` returns a status other than OK when the host refuses the data. The runtime refuses a write
  that would take one sink of a call beyond 256 MiB (1 MiB for `describe`) with
  `PRAENORA_STATUS_RESOURCE_EXHAUSTED`, and then every later write to that sink. A module SHOULD stop
  and return the status `write` returned.

Ownership is summarized in [threading-and-ownership.md](threading-and-ownership.md).

## Status codes and errors

- Every function that can fail returns a `praenora_status` (`int32_t`): `PRAENORA_STATUS_OK` (0) on
  success. On failure a module returns a general code from 1 to 99 (1 to 12 are defined in this
  version, and a module SHOULD use one of them) or a code of its own from
  `PRAENORA_STATUS_MODULE_BASE` (10000) up. It MUST NOT return a code from 100 to 9999: 100 to 999
  belong to the runtime and 1000 to 9999 are unassigned. `praenora_status_name()` gives the stable
  name of codes 0 to 12 and `"UNKNOWN"` for any other value.
- `PRAENORA_STATUS_CANCELLED` answers a cancellation request. The runtime counts a call that returns
  it as cancelled only when a stop was requested during that call; otherwise the call failed.
- On failure a module SHOULD write an error document to `error_json`:
  `{"code": <int>, "message": "<text>", "details": {...}}` (`pmp-error`; `details` optional). Text
  that is not a JSON object is accepted and taken as the message.
- The runtime wraps a module's failure in an error of its own (`STEP_FAILED`, `STEP_TIMEOUT`,
  `MODULE_UNRESPONSIVE` or `OUTPUT_INVALID`) and keeps the module's error document as its `cause`. A
  call that returns after its deadline counts as timed out whatever status it returned
  ([time-cancellation-timeouts.md](time-cancellation-timeouts.md)).

## Versioning and evolution

- The ABI version is a `major.minor` pair owned by `abi.h` and independent of every package version
  ([compatibility-policy.md](compatibility-policy.md)).
- A host accepts only tables of its own major. Minor versions only append: new module functions go
  at the end of `praenora_module_api_v0` and new host callbacks at the end of `praenora_host_api`,
  and each side reads the other's `struct_size` to learn which exist.
- A new generation N adds a table type `praenora_module_api_vN` that starts with the same
  `praenora_api_header`. A host MAY ask for its newest generation first and fall back to older ones.
- Changing the ABI adds an ADR; once versions are bumped again, it also bumps the version in `abi.h`
  ([compatibility-policy.md](compatibility-policy.md#changing-a-version)). Structured logging and
  module-defined metrics are candidate minor additions ([open-questions.md](../open-questions.md),
  OQ-24).

## C++ adapter and test kit (informative)

[`module.hpp`](../../include/praenora/sdk/module.hpp) is an optional, header-only C++ adapter: a
module derives from `praenora::sdk::Module` and expands `PRAENORA_SDK_DEFINE_MODULE(ClassName)`
once, which defines the entry point and a static table. Its functions catch every exception
(`praenora::sdk::Error` becomes its code, `std::bad_alloc` `RESOURCE_EXHAUSTED`, anything else
`INTERNAL`), write outputs in 4096-byte pieces, build `describe()` from the identity macros that
`praenora_add_module()` defines, and parse no JSON. Its `Host` wraps the host API, and
`Host::api()` gives the host API itself for service functions that take it.
[`testing.hpp`](../../include/praenora/sdk/testing.hpp) drives a module's table in process with a
fake host. Details are in
[ADR-0004](../adr/ADR-0004-lifecycle-functions-and-documents.md) and
[ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md); building a module with
[`PraenoraModule.cmake`](../../cmake/PraenoraModule.cmake) is described in [manifest.md](manifest.md).

## References

- [ADR-0003](../adr/ADR-0003-native-module-abi-v0.md): single entry point and versioned table.
- [ADR-0004](../adr/ADR-0004-lifecycle-functions-and-documents.md): lifecycle functions and documents.
- [ADR-0005](../adr/ADR-0005-ownership-and-allocation-across-the-abi.md): ownership and allocation.
- [ADR-0006](../adr/ADR-0006-threading-model.md): threading.
- [ADR-0012](../adr/ADR-0012-time-timeouts-and-cancellation.md): timeouts and cancellation.
- [ADR-0019](../adr/ADR-0019-platform-loading-rpath-dll-search.md): platform loading.
- [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md): runtime services.
- [module-developer-guide.md](../module-developer-guide.md): writing a module.
