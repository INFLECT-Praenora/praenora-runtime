# ADR-0026: Runtime service providers: one engine per process, loaded on demand

- Status: Accepted (scope noted in Decision)
- Date: 2026-11-16
- Supersedes: none (amends [ADR-0021](ADR-0021-runtime-services-through-host-api.md))
- Superseded by: none

## Context

[ADR-0021](ADR-0021-runtime-services-through-host-api.md) gave modules runtime services through the
host API's `get_service` and deferred a simulation service backed by libRoadRunner, planned to be
linked into the runtime behind the build option `PRAENORA_WITH_ROADRUNNER`. Engines of that kind are
heavy: libRoadRunner carries a compiler (LLVM) with process-wide state and its own toolchain
requirements. Several forces shape where such an engine lives:

- ABI v0 promises that a module may be built with any compiler and links nothing of the runtime. A
  module that links the engine itself gives that up, and two modules each bundling a copy of the
  engine put two copies of its compiler, with their process-wide state, into one process.
- Linking the engine into the runtime library makes every runtime carry and load it, also when no
  run needs it, and ties the runtime's build to the engine's toolchain.
- A run's results computed by an engine are reproducible only for the same engine, so the engine's
  identity has to be known to the runtime.

## Decision

Status split: service providers, every bullet below but **Provider interface (internal)**, are
Accepted, by the maintainer on 2026-11-22. The bullet **Provider interface (internal)** is Proposed:
the interface is internal to the runtime, may change with any runtime release, and is not offered
to third parties until it has settled (see Alternatives considered). The builder notes are
Proposed.

- **Service providers.** Heavy engines are offered by service providers: separately built shared
  libraries, shipped with the runtime, each described by a provider manifest
  (`<name>.provider.json`, document kind `provider-manifest` 0.1.0) that names its library, the
  runtime version it was built with, the provider interface version, its platform, the services it
  offers (`{name, major, minor}`) and its engine. The runtime owns one engine per process: it loads a
  provider only when something needs one of its services, starts it once and keeps it loaded for the
  rest of the process. Modules reach engines only through `get_service`.
- **Provider interface (internal).** `src/runtime/provider/provider_abi.h` (C99, depending only on
  `abi.h`), version 0.0: one exported entry point `praenora_provider_get_api` returning a table with
  `describe`, `start` and `get_service`, and a host with logging callbacks. It is built only in this
  repository, never installed, and may change with any runtime release; a provider is usable only
  with the runtime version it was built with. Third-party providers are not supported in this
  version.
- **Discovery and resolution.** Provider manifests are found on provider search paths: the
  caller's paths, `PRAENORA_PROVIDER_PATH`, then the installation's provider directory
  `<prefix>/lib/praenora/providers`, the module directory's sibling. A requirement
  `{name, major, minor}` resolves to the first usable provider, in search order, that offers the name
  at that major with at least that minor.
- **Loading.** Before a provider's code runs, its library's digest is checked; after loading, its
  table and its `describe()` identity (name, version, services, engine) are checked against the
  manifest, as for modules. Each provider is loaded and started at most once per process, its library
  is never unloaded, and a second library of the same provider name in one process is refused.
- **Requirements and records.** Modules declare the services they require or can use in their
  manifests, so that validation refuses a missing one before anything is loaded with the new error
  code `SERVICE_UNAVAILABLE` (407, category compatibility); a module sees only the provider services
  it declared; and each service a run resolves, with its provider's identity and engine, enters the
  run fingerprint.
- **Versions.** The runtime package and the module SDK move to 0.2.0. Document formats are versioned
  per kind (`PRAENORA_FORMAT_VERSIONS` in `cmake/PraenoraVersions.cmake`, reported by
  `version_info().formats`); `provider-manifest` starts at 0.1.0, and `module-manifest` (service
  requirements) and `run-record` (the run's services) move to 0.2.0, their 0.1 schemas kept.
- **Amendment of ADR-0021.** The planned libRoadRunner simulation service is offered by a provider,
  built only with `PRAENORA_WITH_ROADRUNNER`, instead of being linked into the runtime.

### Builder notes (Proposed)

- Provider manifests are read exactly as module manifests are: the shared steps (size limit, one
  read of the bytes, JSON, kind, format version, schema) moved into `ManifestReader`
  (`src/runtime/discovery/manifest_reader.hpp`), which both readers use; messages start with
  "provider manifest <file>: ". The reader's checks after the schema are: `provider.version` and
  `runtime_version` usable semantic versions, `abi` in 32 bits, `library.path` relative, then each
  service's major and minor in 32 bits and its name and major not listed before.
- Later: every integer of a provider manifest, and each service's major and minor in a provider's
  `describe()`, is read by its value, so one written `-0`, which JSON allows and the schema accepts
  as an integer, is 0
  ([ADR-0036](ADR-0036-describe-and-manifest-agree-on-every-field-they-share.md), builder notes).
  The fixture provider `test-negative-zero` writes its service's major and minor as `-0` in its
  `describe()` and loads.
- The provider manifest schema accepts only `name` and `version` in `provider` and only `path` and
  `sha256` in `library`: a provider has no informational fields and no `dependency_paths` in format
  0.1; providers find their own dependencies through run paths.
- Shadowing is by provider name and platform, as module shadowing is by name, version and platform:
  a manifest for another platform neither shadows nor is shadowed, so a directory can hold one
  provider per platform. A shadowed manifest is never resolved; when a requirement does not resolve,
  it is listed among the providers that came close, with the reason "shadowed".
- A provider's usability problem is the first broken rule in the order platform, runtime version,
  provider interface major. Runtime versions compare by SemVer equality, so build metadata is
  ignored and a pre-release is another version.
- The details of an unresolved requirement are `{"service", "major", "minor", "search_paths",
  "providers": [{"name", "version", "manifest", "reason"}]}` (`ProviderIndex::unresolved_details()`),
  where the reasons are a usability problem, "shadowed", "offers ... only" (other majors) or "offers
  ...; ... or a later minor is required".
- The loader (`load_provider()`, `src/runtime/provider/provider_loader.hpp`) takes the requirements
  resolved to the provider; one that the manifest does not offer is the caller's error
  (`INVALID_ARGUMENT`). Its order: the library file and its digests (pinned, then the one validation
  computed) before any process-wide state; then the conflict check against a library of that name
  already in the process, or the opening of the library; then, by the provider's state, its earlier
  `start()` failure, the identity against the new manifest (started), or the entry point, the table,
  the identity, `start()` and the event (not started yet). `get_service()` is called on every load
  for each service the load needs, and its tables are kept for `LoadedProvider::service()`.
- The process-wide record of a provider is made as soon as its library opens, with the state
  `opened`, `start_failed` or `started`, the library's canonical path (symbolic links resolved) and
  its SHA-256. In every state, another library file or the same file with another SHA-256 is a
  `conflict`: a library that failed its entry point, table or identity check stays in the process (its
  static initializers have run), so no other library of the name may join it. A load of the same file
  while the state is `opened` runs the entry point, table and identity checks again with its own
  manifest, since they are free of side effects, and goes on to `start()` when they pass: a load with
  a corrected manifest can start the provider without restarting the process.
- A provider library is pinned as soon as it is opened, also when a later check fails: provider
  libraries are never closed.
- A `start()` that fails is kept: every later load of the provider reports the same failure (reason
  `start`, the same details), saying that `start()` is not called again. An exception that escapes
  `start()` despite the interface's rule counts as a failure with status `INTERNAL`.
- `LIBRARY_LOAD_FAILED` from the operating system carries the reason `library` (the specification
  names the reasons `identity`, `start`, `service` and `conflict`). The provider's error document of a
  failed `start()` is `details.cause`, as a module's is for a failed call; a missing service gives
  `details.services` and the pointer `/services/<i>` of the first one; a conflict gives
  `details.loaded` (`manifest`, `library`, `library_sha256`).
- A table whose header reports another provider interface major is `API_TABLE_INVALID`
  (`ABI_INCOMPATIBLE` stays the module ABI's code). `struct_size` must cover the whole v0.0 table,
  since all three functions are required.
- `describe()` may write at most 1 MiB and a failed `start()` at most 64 KiB of error document. A
  `describe()` document that fails, is not JSON or lacks the identity's shape is reason `identity`
  without `details.differences`; differences carry JSON values (`{"field", "manifest", "describe"}`).
- Logging: a load given a runtime's logs sets the provider's logger to `provider.<name>` of those
  logs, so with one engine per process, a provider's records go to the runtime that loaded it last.
  A first load without logs logs nothing, the event `provider.loaded` included. When the logs the
  provider writes to are destroyed (`LogHub::on_destroy()`), its logger becomes one on a
  process-level null sink, so the provider keeps none of a finished `Runtime`'s or run's sinks and
  files; the next load given logs points it there. The provider's records carry the attribute
  `provider` (`<name>@<version>`); `provider.loaded` also carries `event` and `library_sha256`. The
  provider host's log callbacks share the module host's level mapping (`log_level_from_abi()`,
  `log_enabled()` in `src/runtime/observability/logging.hpp`).
- The process-wide state is allocated once and never freed, so provider threads may log through
  their hosts while the process exits.
- `praenora_add_provider()` (`cmake/PraenoraProvider.cmake`): the export list is
  `-exported_symbol,_praenora_provider_get_api` on macOS and a version script elsewhere on ELF; the
  compile definitions `PRAENORA_PROVIDER_IMPLEMENTATION`, `PRAENORA_PROVIDER_NAME`,
  `PRAENORA_PROVIDER_VERSION`, `PRAENORA_PROVIDER_SERVICES` and `PRAENORA_PROVIDER_ENGINE` give the
  identity to the code; `ENGINE_JSON` becomes one line and must not hold `;` or `$<`; there is a
  `LINK_LIBRARIES` option and no manifest template. Provider code includes the interface as
  `"provider_abi.h"`, from the internal target `praenora_provider_interface`.
- `provider_abi.h` also defines the type `praenora_provider_get_api_fn` and declares the entry point
  only under `PRAENORA_PROVIDER_IMPLEMENTATION`, as `abi.h` does for modules. The build reads the
  provider interface version from the header, as it reads the ABI version from `abi.h`.
- Format versions per kind: `PRAENORA_FORMAT_VERSIONS` lists `<kind>=<version>`, which also define
  `PRAENORA_FORMAT_VERSION_<kind>`; `version_info()` reads a table generated from the list.
- Module and provider discovery share their search-path helpers
  (`src/runtime/discovery/search_paths.hpp`), and module and provider loading the library file
  check, the errors located in a manifest (`LocatedFailure`) and the status text of messages
  (`src/runtime/loader/library_file.hpp`); `SharedLibrary::open()` takes the words that name the
  library in its message ("provider library"). Module behavior and messages are unchanged.
- The summaries of `MANIFEST_INVALID`, `LIBRARY_NOT_FOUND`, `LIBRARY_DIGEST_MISMATCH`,
  `LIBRARY_LOAD_FAILED`, `ENTRY_POINT_MISSING` and `API_TABLE_INVALID` now name providers too. The
  likely owner of `SERVICE_UNAVAILABLE` is the environment (a provider that is not installed).
- Tests: the fixture providers (`tests/fixtures/providers/`, built with
  `praenora_add_fixture_provider()` into `<build>/test-providers/<name>/`, named to the tests by
  `PRAENORA_TEST_PROVIDERS_DIR`) are `test-echo`, which offers the test service `test.echo@0.0`
  (`tests/support/test_echo.h`: `echo(context, host, input, output)` writes its input back) and
  refuses a second `start()`, and providers built from one source that each break one rule or, for
  `test-reordered`, `test-conflict` and `test-retry`, none; those that start refuse a second
  `start()` too. The case of a library without the entry point uses a fixture module's library under
  a provider manifest, since the export list of `praenora_add_provider()` cannot name a symbol the
  library lacks. A test that loads a copy of a fixture under a provider name leaves that copy in the
  process, so a second run of it in one process expects a conflict. The Python tests' environment
  names the fixture providers and unsets `PRAENORA_PROVIDER_PATH`; the C++ tests set environment
  variables with one `ScopedEnvironment` (`tests/support/scoped_environment.hpp`).

- Module requirements: `requires.services` of module manifest format 0.2 is read into
  `ModuleManifest::service_requirements` (minor 0 and optional false by default); a name and major
  listed twice is `MANIFEST_INVALID` at the later entry, after the capabilities and before the
  parameter schema. `ServiceVersion` moved to `src/runtime/instance/services.hpp`, with
  `service_key()` for `"<name>@<major>"`.
- `praenora_add_module()` writes `format_version` from `PRAENORA_MODULE_MANIFEST_FORMAT_VERSION`
  (0.2.0), which `cmake/PraenoraVersions.cmake` and the installed package configuration define and
  templates take as `@PRAENORA_MODULE_MANIFEST_FORMAT_VERSION@`; the repository's templates use it.
  `@PRAENORA_MODULE_REQUIRES_JSON@` is always an object `{"services": [...]}`, required services
  first, each with all four fields; the generated default manifest has `requires` only when the
  module lists services. The helper refuses a malformed entry, a major or minor beyond 32 bits, and
  a name and major listed twice.
- `SERVICE_UNAVAILABLE` carries its details through a new member of the public
  `praenora::Diagnostic`, `details_json` (empty for none), which `ValidationReport::to_json()`
  writes as `details`; the message names the service and each provider that came close with its
  reason, or the searched paths when none did. One function builds it for validation and the
  contract checks (`service_unavailable()`, `src/runtime/api/validation.hpp`).
- A required service that does not resolve does not stop the step's other checks (library file,
  parameters), so every problem is reported. The provider library checks run once per provider,
  after every step's services resolved; a problem is reported for the first step, in schedule order,
  that uses the provider (in step-id order when the graph is invalid and has no schedule), the same
  rule as for `validate --load`. The warnings of the provider index are added to every validation
  report, as the module index's are, also when no module declares a service.
- One provider per service name and major in a run: the provider the highest minor any step asks
  for resolved to (the first step in step-id order on a tie), since it offers every lower minor
  too; the reproducibility section's `services` holds the minor it offers.
- Visibility: an instance gets its step's provider services (`InstanceOptions::provider_services`,
  copied into the instance) before the built-in registry, so a built-in service of the same name and
  major never hides the provider's, which the fingerprint records. Withdrawal and abandonment stop
  serving both, through one function (`ModuleInstance::stop_serving()`).
- A run loads its providers after it prepared its output directory and wrote its first run record,
  so a provider that does not load leaves a failed run record; the failure keeps the loader's code,
  location and details, and names no step. A run stopped before it starts loads no provider and ends
  `stopped`. `validate --load` loads the providers before the modules, reports each problem for the
  first step in schedule order that uses the provider, and leaves the providers loaded. The contract
  checks load providers within `library.load`, after the library file's check, without a digest from
  a validation; a failure's message starts with the error's name, and a pass lists each service and
  its provider (or that an optional one is not available). `check_module()` takes the `LogHub` that
  receives the providers' records; the `Runtime` passes its own.
- The caller's provider paths are `RuntimeOptions::provider_paths`, searched before
  `PRAENORA_PROVIDER_PATH` (with `use_environment`) and the installation's provider directory; a
  workflow configuration names no provider paths. White-box tests build contexts over the fixture
  providers.
- Listing: `Runtime::list_providers()` returns a `ProviderInfo` for every indexed manifest in
  search order, as `list_modules()` lists shadowed module manifests too. A shadowed manifest, never
  used, is listed with `usable` false and the problem "shadowed by '<manifest>', which comes first
  on the provider search path; it is never used", naming the manifest that shadows it
  (`IndexedProvider::shadowed_by`); shadowing is reported before any other problem, and
  resolution's reason for a shadowed provider is the same text (`shadowed_reason()`), so the
  listing and `SERVICE_UNAVAILABLE` details agree. Manifests for other platforms, runtime versions
  or provider interfaces are listed with `usable` false and the first rule they break.
  `ProviderInfo::problem` is a string, empty for a usable provider (as `ModuleInfo`'s optional
  fields are empty when absent); `engine_json` is the engine object as canonical JSON; `services`
  are `"<name>@<major>.<minor>"` in manifest order. Listing loads nothing. The `runtime.ready`
  event also logs the provider search paths and the number of indexed provider manifests.
- The tool's `--provider-path P` is a global option, as `--module-path` is, so every command takes
  it (it matters to `validate`, `run`, `modules check` and `providers list`). `praenora providers
  list` prints a table (name, version, platform, the services joined by ", ", the manifest, then
  `usable` or `unusable: <reason>`), and a note on standard error when no manifest is found (exit
  0); with `--json` the result is the array that `praenora::frontend::provider_json()` builds, the
  services split back into `{name, major, minor}` by `frontend::service_json()` (an internal error
  for text of another form). The Python package's `list_providers()` returns the same array.
- Python: `provider_paths` is keyword-only on `validate()`, `check_module()` and `Workflow`, where
  it follows positional arguments, and positional on `list_providers()`, as `module_paths` is on
  `list_modules()`. `inspect_module()` takes no `provider_paths`, because inspecting a module loads
  no provider; the tool's global `--provider-path`, like `--catalog-path`, has no effect on
  `modules inspect`. The facade's `validate_json()` and `check_module_json()` take the provider
  paths, `list_providers_json()` is new, and `WorkflowHandle` takes the option `provider_paths`.
- The tool rebuilds the diagnostics of a validation report that an error carries (`run` and
  `graph` of an invalid configuration) with their `details`, so its `--json` diagnostics have them
  for every command, as `validate`'s already had.
- The run-record 0.2 schema keys `services` by `^<service name>@<major>$`, requires
  `{minor, provider {name, version, manifest_sha256, library_sha256}, engine}` for each, and
  requires `steps.<step>.services` as an array of unique keys.
- The provider loader makes a provider's host once and keeps it, and records the result of
  `start()` before it builds any message, so an exception thrown afterwards cannot lead to a second
  `start()` or to a host that a log hub still calls being replaced; the null sink of released
  loggers is never freed, like the registry. The status text of messages is one function for the
  loaders, the engine and the contract checks; a module-defined status reads
  `10001 (module-defined)`.
- Tests: fixture modules built from one source (`tests/fixtures/modules/fx-service/`) declare
  services in each way (`fx-service` requires `test.echo@0.0` and can use `test.other@1.0` through a
  template; `fx-service-optional` and `fx-service-minor` use the generated manifest;
  `fx-service-format-0-1` has a manifest of format 0.1) and report what `get_service()` returned; a
  fixture module without a template gets the generated manifest. The fixture provider `test-order`
  serves only the test that providers start before any module. Tests copy fixture modules and
  providers into scratch directories to change a manifest or a library
  (`tests/support/provider_fixtures.hpp`).
- Front-end tests: the configuration `tests/fixtures/configs/cli/service/workflow.yaml` (an
  fx-service step that requires `test.echo@0`) and the provider search path
  `tests/fixtures/configs/cli/providers/`, which holds the manifest of a provider built for runtime
  0.0.1 and linux-x86_64 and, in `shadowed/`, another version of it that the first shadows, both
  without a library, are shared by the CLI and Python tests. CLI tests that must find no provider
  set `PRAENORA_PROVIDER_PATH` empty, which counts as unset; the test of an empty listing exists
  only in builds without `PRAENORA_WITH_ROADRUNNER`, whose provider would sit in the build tree's
  provider directory.
- Later: [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) loads and starts the
  providers of a step in a module host in that module host, with the same loader and checks, so
  that an engine lives in the process of the module that calls it. One engine per process still
  holds, and a module host is a process of its own: a provider that steps in module hosts use is
  started once in each of their module hosts, and the runtime's process loads only the providers
  of its own steps. The out-of-process providers below are built that way, with their module.
  [ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md) appends `load_compiled` and
  `save_compiled` to `praenora_provider_host`, NULL in the runtime's process, within interface
  version 0.0.

## Alternatives considered

- **Each module links the engine.** Gives up ABI v0's freedom of compilers, duplicates a large
  dependency in every module, and puts several copies of a compiler with process-wide state into one
  process.
- **The engine linked into the runtime library** (the plan of ADR-0021). Every runtime would carry
  and load the engine, and the runtime's build would depend on the engine's toolchain.
- **Modules link the engine's C++ API directly, as an optional tier.** The engine's installed
  headers and its exports do not support it on every platform, and it pins modules to one exact
  build of the engine.
- **Out-of-process providers.** Isolate crashes ([open-questions.md](../open-questions.md), OQ-5),
  at the cost of serializing every call; not needed yet.
- **A public provider interface for third parties.** Deferred until the interface has settled.

## Consequences

- A runtime without providers, or a run that needs none, loads no engine; one with a provider loads
  it once per process, whatever number of runs or `Runtime` objects use it.
- Providers are rebuilt with every runtime release, since a provider is usable only with the runtime
  version it was built with.
- An engine's process-wide state is shared by every module of the process that uses its services;
  the services' specifications must say how they keep modules apart.
- Module manifests of format 0.2 declare the services a module requires or can use; validation
  resolves them, runs, `validate --load` and the contract checks load their providers, and the run
  record (format 0.2.0) records each service with its provider and engine. `praenora_add_module()`
  of SDK 0.2 writes manifests of format 0.2.0, which a runtime 0.1 cannot read.
- The front ends take the caller's provider search paths (`RuntimeOptions::provider_paths`, the
  tool's `--provider-path`, Python's `provider_paths`) and list the providers they find
  (`Runtime::list_providers()`, `praenora providers list`, `list_providers()`).

## References

- [docs/specs/runtime-services.md](../specs/runtime-services.md)
- [ADR-0021](ADR-0021-runtime-services-through-host-api.md)
- [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md): versioning; this ADR makes the
  document format versions per kind (`PRAENORA_FORMAT_VERSIONS` replaces `PRAENORA_FORMAT_VERSION`).
- [docs/specs/compatibility-policy.md](../specs/compatibility-policy.md)
- [docs/specs/error-taxonomy.md](../specs/error-taxonomy.md)
- [docs/feasibility/libroadrunner.md](../feasibility/libroadrunner.md)
- [docs/open-questions.md](../open-questions.md)
