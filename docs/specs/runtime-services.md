# Runtime services and service providers

- **Version:** runtime 0.2.0; provider interface 0.0 (`PRAENORA_PROVIDER_ABI_MAJOR` 0,
  `PRAENORA_PROVIDER_ABI_MINOR` 0); document format `provider-manifest` 0.1.0 (schema
  [`provider-manifest.0.1.schema.json`](../../spec/schemas/provider-manifest.0.1.schema.json));
  service requirements in `module-manifest` 0.2.0 and services in the `run-record` 0.2.0
- **Status:** Normative. Proposed ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md)). The
  provider interface is internal to the runtime: it is built only in this repository, never
  installed, and may change with any runtime release.
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

A runtime service is a function table that the runtime offers to modules: a module asks for one by
name and major version through the host API's `get_service`
([module-abi.md](module-abi.md#host-api)). Some services would carry a heavy engine, for example a
simulation engine with a compiler of its own, which neither every module nor the runtime library
itself should carry. Such services come from **service providers**: separately built shared
libraries, shipped with the runtime and described by provider manifests, that the runtime loads only
when something needs one of their services, starts once and keeps loaded for the rest of the process.
A process holds at most one library, and so one engine, of each provider. Modules never link an
engine: they reach it only through `get_service`, so a module may still be built with any compiler.

This document specifies services, provider manifests, the provider interface, how the runtime finds
providers and resolves a service to one of them, which services a module sees, how runs, validation
and the contract checks load providers, how the run fingerprint records them, and how the runtime
loads a provider. Modules declare the services they require or can use in their manifests
(`requires.services`, module manifest format 0.2; [manifest.md](manifest.md#fields)); a manifest of
format 0.1 declares none.

## Services

- A service has a **name**, a dotted identifier such as `praenora.simulation`
  (`^[a-z][a-z0-9-]*(\.[a-z][a-z0-9-]*)*$`, as module names), and a version `major.minor`.
- Its table is a C struct that starts with `praenora_api_header`, whose `abi_major` is the service's
  major and whose `struct_size` covers the fields the provider fills, as for module tables. A later
  minor version only appends fields; a new major is a new table.
- A provider **offers** a service at one minor of each major it serves. A **requirement**
  `{name, major, minor}` is satisfied by an offer of the same name and major whose minor is at least
  the required one.
- A service's own specification states its documents, threading and ownership rules. The public
  services of this version: `praenora.simulation@0`, simulating models
  ([simulation-service.md](simulation-service.md)), whose table is declared in the module SDK's
  [`simulation.h`](../../include/praenora/sdk/simulation.h).

## Provider manifest

The manifest of the test provider `test-echo`, as `praenora_add_provider()` generates it on macOS
arm64:

```json
{
  "kind": "provider-manifest",
  "format_version": "0.1.0",
  "provider": { "name": "test-echo", "version": "0.1.0" },
  "runtime_version": "0.2.0",
  "abi": { "major": 0, "minor": 0 },
  "library": { "path": "test-echo.so" },
  "platform": { "os": "macos", "arch": "arm64" },
  "services": [{"name":"test.echo","major":0,"minor":0}],
  "engine": {"name": "test-echo-engine", "version": "1.0.0"}
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `kind` | `"provider-manifest"` | yes | The document kind. |
| `format_version` | semantic version | yes | The format; 0.1.x is read with the 0.1 schema. |
| `provider.name` | identifier | yes | The provider's name, with the rules of module names. |
| `provider.version` | semantic version | yes | The provider's version. |
| `runtime_version` | semantic version | yes | The version of the runtime the provider was built with. |
| `abi.major`, `abi.minor` | integers from 0 to 4294967295 | yes | The provider interface version the library was built for. |
| `library.path` | non-empty string | yes | The library file, relative to the manifest's directory. |
| `library.sha256` | 64 lowercase hex digits | no | The library's SHA-256, which the runtime then verifies before loading it. |
| `platform.os` | `linux`, `macos` or `windows` | yes | The operating system the library was built for. |
| `platform.arch` | `x86_64` or `arm64` | yes | The architecture the library was built for. |
| `services` | array of `{name, major, minor}`, at least one | yes | The services the provider offers; majors and minors are integers from 0 to 4294967295. |
| `engine` | object | yes | Describes the engine behind the services (`{}` for none). It is compared with the provider's `describe()`. |
| `x-...` | any | no | Extension fields, at the top level only. |

Rules:

- **File.** A provider manifest is a strict JSON file whose name ends in `.provider.json`, beside
  its library, encoded in UTF-8 and at most 16 MiB. It MUST be valid against the
  `provider-manifest` schema: unknown properties are refused, except top-level properties whose
  names start with `x-`.
- **Path.** `library.path` MUST be relative to the manifest's directory; a path that is absolute in
  POSIX or in Windows notation is refused on every platform, as in module manifests
  ([manifest.md](manifest.md#rules)).
- **Services.** Each `(name, major)` MUST appear at most once.

The runtime reads a provider manifest without loading the library, in the order and with the errors
of a module manifest ([manifest.md](manifest.md#reading-a-manifest)). Every error is located in the
manifest's file, at the line, column and JSON pointer of the problem where there is one:

| # | Check | Error |
|---:|---|---|
| 1 | The file is at most 16 MiB, can be read and is JSON. | `PARSE_ERROR` (too large, or not JSON), `IO_ERROR` |
| 2 | It is an object whose `kind` is `provider-manifest`. | `MANIFEST_INVALID` |
| 3 | `format_version` is a semantic version for which the runtime has a schema. | `MANIFEST_INVALID`, `FORMAT_VERSION_UNSUPPORTED` (naming the supported formats) |
| 4 | It satisfies the schema. | `MANIFEST_INVALID` at the first problem's pointer, every problem in the details |
| 5 | `provider.version` and `runtime_version` are usable semantic versions, and `abi.major` and `abi.minor` fit in 32 bits, in that order. | `MANIFEST_INVALID` |
| 6 | `library.path` is relative. | `MANIFEST_INVALID` |
| 7 | Service by service, in order: its major and minor fit in 32 bits, and its name and major are not listed before. | `MANIFEST_INVALID` |

The manifest's SHA-256 is the digest of exactly the bytes read.

## Provider interface

The header [`provider_abi.h`](../../src/runtime/provider/provider_abi.h) (C99, depending only on
[`abi.h`](../../include/praenora/sdk/abi.h)) declares the interface; it is authoritative and
internal. Its conventions are those of the module ABI ([module-abi.md](module-abi.md#conventions)):
only fixed-width integers, `size_t`, pointers, length-delimited UTF-8 and host-owned sinks cross it,
and no exception or `longjmp` leaves a function of the provider.

```c
#define PRAENORA_PROVIDER_ABI_MAJOR 0u
#define PRAENORA_PROVIDER_ABI_MINOR 0u
#define PRAENORA_PROVIDER_ENTRY_POINT "praenora_provider_get_api"

typedef struct praenora_provider_host {
    uint32_t struct_size;  /* sizeof as compiled into the runtime */
    uint32_t reserved;     /* 0 */
    void* context;
    void (PRAENORA_CALL *log)(void* context, int32_t level, const char* msg, size_t size);
    int32_t (PRAENORA_CALL *log_enabled)(void* context, int32_t level);
} praenora_provider_host;

typedef struct praenora_provider_api_v0 {
    praenora_api_header header;
    praenora_status (PRAENORA_CALL *describe)(praenora_sink* out_json);
    praenora_status (PRAENORA_CALL *start)(const praenora_provider_host* host,
                                           praenora_sink* error_json);
    const void* (PRAENORA_CALL *get_service)(const char* name, size_t name_size, uint32_t major);
} praenora_provider_api_v0;

PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_provider_api_v0* PRAENORA_CALL
praenora_provider_get_api(void);
```

- **Entry point.** The library exports exactly one symbol, `praenora_provider_get_api`, with C
  linkage. It returns a pointer to a static table, is thread-safe and has no side effects. The
  prototype is declared only when `PRAENORA_PROVIDER_IMPLEMENTATION` is defined, which
  `praenora_add_provider()` does.
- **Table.** `header.abi_major` is `PRAENORA_PROVIDER_ABI_MAJOR`, `header.reserved` is 0 and
  `header.struct_size` covers the whole v0 table; every function is required.
- **describe.** Writes `{"name", "version", "services": [{"name", "major", "minor"}], "engine"}` to
  `out_json`. Thread-safe and free of side effects; callable any time after the library is loaded.
- **start.** Called exactly once per process, before any `get_service`, under the runtime's lock. It
  MAY fail: it then returns a status other than `PRAENORA_STATUS_OK` and SHOULD write a document of
  the `pmp-error` shape (`{"code", "message", "details"}`) to `error_json`. `host` stays valid for
  the rest of the process.
- **get_service.** After a successful `start`: the table of a service the provider offers, for its
  name and major, or NULL. Tables live until the process ends. Thread-safe.
- **Host.** `host->log` writes a message at a `PRAENORA_LOG_*` level to the provider's own logger,
  `provider.<name>` ([Loading](#loading)); levels below 0 count as trace and above 5 as critical.
  `host->log_enabled` is nonzero when a message at that level would be recorded. Both are
  thread-safe, callable from any thread at any time, and never throw. A provider MUST check that
  `struct_size` covers a field before it uses it.
- **Lifetime.** A provider is never stopped and never unloaded. Its threads MAY outlive every
  `Runtime` object of the process.

## Discovery

The runtime searches these provider search paths, in order; each is made absolute and lexically
normal, and a path named twice is searched once:

1. the provider paths its caller gives, in order (relative ones against the current directory):
   `RuntimeOptions::provider_paths`, the tool's `--provider-path` (repeatable) or the Python
   package's `provider_paths`;
2. the entries of `PRAENORA_PROVIDER_PATH` (`:`-separated, `;` on Windows; empty entries are
   skipped), when the runtime reads the environment;
3. the installation's provider directory, `<prefix>/lib/praenora/providers`: the sibling of the
   installation's module directory, located from the runtime library's own path in the same way
   ([manifest.md](manifest.md#discovery)).

In each path the runtime reads its own `*.provider.json` files, then those of each immediate
subdirectory, each list sorted by UTF-8 bytes. A path that does not exist is skipped silently; an
existing path that is not a directory gets a warning (`NOT_FOUND`). An invalid manifest is a warning
with its error code and location, and is skipped. A manifest file reached twice through overlapping
paths is read once.

A provider is **usable** when its manifest is for the host platform, its `runtime_version` equals the
runtime's version (SemVer equality: build metadata ignored, a pre-release is another version) and its
`abi.major` equals `PRAENORA_PROVIDER_ABI_MAJOR` (any minor). Otherwise it is listed with the reason,
which names the first of these rules it breaks.

The first manifest of a provider name for a platform wins: a later manifest of the same name and
platform is **shadowed**. It is never used, and a warning (code `OK`) names both files. A manifest
for another platform is listed, unusable, and shadows nothing, so one directory can hold a provider
for several platforms.

`Runtime::list_providers()`, `praenora providers list` and the Python package's `list_providers()`
list every manifest found, in search order: each provider's name, version, manifest and library
paths, platform, services and engine, whether it is usable and, when it is not, the reason. A
shadowed manifest is listed unusable, the reason naming the manifest that shadows it, before any
other reason it has; resolution gives a shadowed provider the same reason. Listing reads manifests
only; nothing is loaded.

## Resolution

A requirement `{name, major, minor}` resolves to the first provider, in search order, that is usable,
not shadowed, and offers `name` at `major` with a minor of at least `minor`; the offered minor is the
one the run gets. When none does, every provider that offers a service of that name but cannot serve
the requirement is reported with the reason: unusable (its reason), shadowed, offering other majors
only, or offering a lower minor.

Validation resolves each service that a step's module declares (step 6, after the compatibility
rules of the manifest and before its library checks;
[manifest.md](manifest.md#compatibility-and-library-checks)):

- A required service (`optional` false) that does not resolve is `SERVICE_UNAVAILABLE` (code 407,
  category compatibility, exit code 3), located at `/requires/services/<i>` of the module's
  manifest, for the step. Every such service of every step is reported. The message names the
  service and why no provider serves it. The details, also in the validation report's JSON, are
  `{"service", "major", "minor", "search_paths", "providers": [{"name", "version", "manifest", "reason"}]}`,
  where the providers are those that offer the name but cannot serve the requirement.
- An optional service that does not resolve is not used: the run goes on without it, and the module
  does not see it.
- A run uses one provider for each service name and major. When the steps of a run declare a name
  and major at different minors, the provider that the highest of those minors resolved to serves
  them all (it offers at least every minor they ask for); on a tie, the first step in step-id order
  that asks for that minor decides.
- Then the library file of each provider the run uses is checked as a run's loading checks it first
  (checks 1 and 2 of [Loading](#loading), without a digest from an earlier validation):
  `LIBRARY_NOT_FOUND`, `IO_ERROR` or `LIBRARY_DIGEST_MISMATCH`, located in the provider's manifest
  and reported once, for the first step, in schedule order, that uses the provider. Its SHA-256 is
  kept for the run.
- Nothing is loaded. The warnings of the provider index (manifests that are invalid or shadowed,
  search paths that are not directories) are added to the validation report, as those of the module
  index are.

## Visibility

A module instance's `get_service` ([module-abi.md](module-abi.md#host-api)) returns:

1. the table of a provider service when the module's manifest declares that name and major and it
   resolved for the run (required or optional);
2. otherwise the table of a runtime built-in service of that name and major (in this version only
   test services that tests register, such as `test.gate`), whatever the manifest declares;
3. otherwise NULL, logged as every lookup is (event `service.lookup`, debug).

So a module never sees a provider service it did not declare, even when the run loaded that
provider for another step, and a module whose manifest has format 0.1 sees no provider service at
all. A declared provider service comes before a built-in service of the same name and major: the
run fingerprint records the provider's. An instance that becomes unresponsive and is abandoned, or
whose services the runtime withdraws (an instance kept, never to be called again, while its run goes
away), gets NULL for every service from then on, provider services included.

## Runs and checks

- **Runs.** When a `Workflow` starts, after its output directory and run record are prepared and
  before any module library is loaded or any instance created, the runtime loads and starts every
  provider of the run's services, each once, with every service the run gets from it, in the order
  of the keys of the reproducibility section's `services` (a provider comes where its first service
  does). The library's SHA-256 must still be the one validation computed. Any error of
  [Loading](#loading) fails the run before tick 0, with the loader's code and details, located in
  the provider's manifest; its details name no step. The provider's records go to the run's logs.
  A run stopped before it starts (a stop requested before `Workflow::start()`) ends as `stopped`
  without loading any provider.
- **`validate --load`** (`ValidateOptions::load`) also loads and starts the providers of the run's
  services, as a run does, before it loads the modules. Each problem is reported in the validation
  report for the first step, in schedule order, that uses the provider. Providers loaded this way
  stay in the process.
- **Contract checks** (`praenora modules check`, `Runtime::check_module`) resolve the services the
  module's manifest declares on the runtime's provider search paths and load and start their
  providers as part of the check `library.load`, before the module library loads: a required service
  that does not resolve fails that check with `SERVICE_UNAVAILABLE`, a provider that does not load
  with the loader's error (the check's message starts with the error's name), and the later checks
  are skipped. The module's instances in the checks are then served the provider services that
  resolved, as in a run.

## Fingerprint

The reproducibility section of the run record (format 0.2.0;
[reproducibility.md](reproducibility.md)) holds, always:

- top-level `services`: for every service of the run, by `"<name>@<major>"`,
  `{"minor": <the minor the provider offers>, "provider": {"name", "version", "manifest_sha256",
  "library_sha256"}, "engine": <the provider manifest's engine object>}`; `{}` when the run uses no
  provider service;
- `steps.<step>.services`: the sorted keys of the services that the step's module may use.

The fingerprint therefore changes when a run's provider, its manifest, its library or its engine
changes, and when a step's module declares other services.

## Loading

The runtime loads a provider when something needs one of its services, and only then. Loading is
process-wide: each provider is loaded and started at most once per process, whichever `Runtime`
object asks first, and its library is never unloaded. The process holds at most one library of a
provider name: once a library of that name has opened, it stays in the process whatever its later
checks or its `start()` do, and every other library of that name is refused. A library opened under
a name keeps that name for the process, even when its identity turns out to be another provider's
(a manifest that names the wrong library): correcting the manifest to another file takes a new
process. The state is guarded by a mutex: concurrent loads of one provider load and start it once.

Each load names the services it needs from the provider, which its manifest offers. Every error is
located in the provider's manifest; `LIBRARY_LOAD_FAILED` carries the reason in `details.reason`.
The checks, in order:

| # | Check | Error |
|---:|---|---|
| 1 | The library file exists and can be read. | `LIBRARY_NOT_FOUND`, `IO_ERROR` at `/library/path` |
| 2 | Its SHA-256 equals the manifest's `library.sha256`, when it has one, and the one validation computed, when the load follows a validation. | `LIBRARY_DIGEST_MISMATCH` at `/library/sha256` (pinned) or `/library/path` (changed after validation) |
| 3 | When a library of that name is in the process already, whatever became of it (checks 5 to 8 may have failed): it is the same library file (symbolic links resolved) with the same SHA-256. | `LIBRARY_LOAD_FAILED`, reason `conflict`, `details.loaded` naming the loaded manifest and library |
| 4 | Otherwise: the library loads, with `dlopen(RTLD_NOW \| RTLD_LOCAL)` or `LoadLibraryExW()` as module libraries do ([module-abi.md](module-abi.md#loading-and-table-acceptance)). From here on it is never unloaded, whatever fails later. | `LIBRARY_LOAD_FAILED`, reason `library`, with the operating system's text |
| 5 | It exports `praenora_provider_get_api`. | `ENTRY_POINT_MISSING` |
| 6 | The entry point returns a table whose header reports provider interface major 0, `reserved` 0 and a `struct_size` that covers the v0 table, and none of whose functions is NULL. | `API_TABLE_INVALID` |
| 7 | `describe()` succeeds and writes a provider identity that equals the manifest in name, version (SemVer equality), services (as a set) and engine (JSON equality). | `LIBRARY_LOAD_FAILED`, reason `identity`, `details.differences` `[{"field", "manifest", "describe"}]` |
| 8 | `start()` succeeds. It is called once in the process: a failure is kept, and every later load of the provider reports it again without calling `start()`. | `LIBRARY_LOAD_FAILED`, reason `start`, the provider's error document as `details.cause` |
| 9 | For every load: `get_service()` returns a table for each service the load needs. | `LIBRARY_LOAD_FAILED`, reason `service`, `details.services` `["<name>@<major>", ...]`, at the first one's `/services/<i>` |

A library that is in the process already is not loaded again (check 4). Then: if its `start()`
failed, that failure is reported again; if it started, it skips checks 5, 6 and 8, and its identity
is checked against the new manifest (check 7); if a check after it opened failed, so that `start()`
was never called, checks 5 to 8 run again with the new manifest (they are free of side effects until
`start()`), so a load with a corrected manifest can start it.

**Logging.** A provider's records go to the logger `provider.<name>` of the logs of the load that
gave it logs last, with the attribute `provider` (`<name>@<version>`): a run's logs when a run loads
it, the `Runtime`'s logs for `validate --load` and the contract checks. A later load, by the same or
another `Runtime` or run of the process, takes the provider's records to its logs. When the logs a
provider writes to go (the `Runtime` or the run that owned them is destroyed), the provider lets go
of them, sinks and log file included, and its records go nowhere until a later load gives it logs
again. The first time a provider is loaded and started in the process, the runtime logs the event
`provider.loaded` (info) on that logger, with the attributes `provider` and `library_sha256`.

## Trust

A provider is native code in the runtime's process, with the trust of a module
([module-abi.md](module-abi.md#trust)): whoever can write to a provider search path can run code in
every process that loads a provider found there. Discovery reads manifests without running provider
code, and a digest pinned in the manifest is the only integrity check.

## Building a provider

Providers are built in this repository with `praenora_add_provider()`
([`cmake/PraenoraProvider.cmake`](../../cmake/PraenoraProvider.cmake), not installed):

```cmake
praenora_add_provider(test_echo
    NAME test-echo
    VERSION 0.1.0
    SERVICES test.echo@0.0
    ENGINE_JSON "{\"name\": \"test-echo-engine\", \"version\": \"1.0.0\"}"
    SOURCES test-echo/test_echo_provider.c common/provider_fixture.c)
```

It creates a `MODULE` library with hidden symbol visibility and an export list that holds only
`praenora_provider_get_api` (also against static libraries linked into it), and writes
`<name>.provider.json` beside it for each build configuration, with this runtime's version and
provider interface version, the platform being built for, the services and the engine. The
compile definitions `PRAENORA_PROVIDER_NAME`, `PRAENORA_PROVIDER_VERSION`,
`PRAENORA_PROVIDER_SERVICES` and `PRAENORA_PROVIDER_ENGINE` let `describe()` report exactly what the
manifest says. `INSTALL_DESTINATION` installs the library and its manifest together.

## References

- [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md): runtime services through the
  host API.
- [ADR-0026](../adr/ADR-0026-runtime-service-providers.md): service providers.
- [simulation-service.md](simulation-service.md) and
  [ADR-0027](../adr/ADR-0027-simulation-service-v0.md): the simulation service.
- [module-abi.md](module-abi.md): the module ABI and its host API.
- [manifest.md](manifest.md): module manifests, their service requirements and their discovery.
- [reproducibility.md](reproducibility.md): the run fingerprint.
- [compatibility-policy.md](compatibility-policy.md): versions.
- [error-taxonomy.md](error-taxonomy.md): the error codes.
