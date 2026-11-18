# Versioning and compatibility policy

- **Version:** applies to runtime 0.2.0, SDK 0.2.0, PMP 0.1.0, ABI 0.0, the provider interface 0.0,
  the simulation service 0.0 and the document formats `module-manifest` and `run-record` at 0.2.0
  and every other kind at 0.1.0
- **Status:** Normative, MVP. Semantic versions for the packages and an ABI version independent of
  them are Accepted; the compatibility rules are Proposed
  ([ADR-0014](../adr/ADR-0014-versioning-and-compatibility-policy.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The runtime, the module SDK, the protocol, the document formats, the catalog entries, the modules
and the service providers change at different rates, and independent teams ship modules. This policy
says how each is versioned and which combinations the runtime accepts. Every rule is decided from
manifests and documents before any module code runs, except the checks of a loaded library's table
and identity.

## Versioned items

| Item | Scheme | Current | Declared in | Rule the runtime enforces |
|---|---|---|---|---|
| Runtime package | SemVer | 0.2.0 | `project(VERSION)` in the top-level `CMakeLists.txt` | None; reported by `praenora version` and recorded in the run record. |
| Module SDK | SemVer | 0.2.0 | `cmake/PraenoraVersions.cmake`, published in the generated `praenora/sdk/version.h` | None for compatibility. A module's manifest and `describe()` report it and agree on it ([manifest.md](manifest.md#identity-check)); the run record holds the manifest's. |
| Module ABI | `major.minor` | 0.0 | [`abi.h`](../../include/praenora/sdk/abi.h); the table header; the manifest's `abi` | The major is equal; the minor is negotiated. |
| Protocol (PMP) | SemVer | 0.1.0 | `cmake/PraenoraVersions.cmake`; `pmp_version` in manifests and ABI documents | Same compatibility class and not newer than the runtime's. |
| Document formats, one per kind | SemVer | `module-manifest` and `run-record` 0.2.0, every other kind 0.1.0 | `PRAENORA_FORMAT_VERSIONS` in `cmake/PraenoraVersions.cmake`; `format_version` in each document | The runtime has a schema for the version's major and minor. |
| Catalog entries | SemVer per entry | 0.1.0 | `version` of each entry | References name exact versions. |
| Modules | SemVer | the module's | the manifest's `module.version` and `describe()` | The configuration pins an exact version. |
| Provider interface (internal) | `major.minor` | 0.0 | [`provider_abi.h`](../../src/runtime/provider/provider_abi.h); the table header; the provider manifest's `abi` | The major is equal. |
| Service providers | SemVer | the provider's | the provider manifest's `provider.version` and `describe()` | Its `runtime_version` equals the runtime's version. |
| Simulation service `praenora.simulation` | `major.minor` | 0.0 | [`simulation.h`](../../include/praenora/sdk/simulation.h); the table header; a provider manifest's `services` | A requirement is satisfied by the same major at a minor of at least the required one; a module uses only the functions that the table's `struct_size` covers ([simulation-service.md](simulation-service.md#evolution)). |
| Model conversion service `praenora.model-conversion` | `major.minor` | 0.0 | [`model_conversion.h`](../../include/praenora/sdk/model_conversion.h); the table header; a provider manifest's `services` | As for the simulation service ([model-conversion-service.md](model-conversion-service.md#evolution)). |

`praenora version --json` prints `{"ok": true, "result": ...}`, whose `result` holds the runtime,
SDK, PMP and ABI versions and the format version of every document kind that carries
`format_version`, each kind's own.

## Semantic versions

- Versions MUST follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html) exactly:
  `MAJOR.MINOR.PATCH[-prerelease][+build]`, numeric parts without leading zeros and below 2^64, no
  `v` prefix and no surrounding whitespace. Anything else is invalid.
- Versions compare by SemVer precedence: a pre-release precedes its release, and build metadata is
  ignored in every comparison, equality included.
- **Compatibility class.** Before 1.0.0, two versions are in the same class when their majors are 0
  and their minors are equal (`0.y.*` with `0.y.*`); from 1.0.0 on, when their majors are equal. A
  `0.y` version and a version of 1.0.0 or later are never in the same class.
- **Pre-1.0 convention.** For `0.y.z`, a change of `y` MAY break compatibility and a change of `z`
  MUST NOT (the Cargo convention). From 1.0.0 on, only a change of the major MAY break it.

## Rules

### ABI

- The runtime accepts a module only when its ABI major equals the runtime's: the manifest's
  `abi.major` before loading (`ABI_INCOMPATIBLE`), and the table header's `abi_major` after
  (`ABI_INCOMPATIBLE`). Any minor is accepted.
- The minor is negotiated through `struct_size` on both tables: the runtime uses only the module
  functions that the module's `struct_size` covers, and a module MUST use only the host callbacks
  that the host's `struct_size` covers. A module that cannot work without a callback MAY refuse
  `create` with `PRAENORA_STATUS_UNSUPPORTED`; the C++ adapter refuses a host API of another major,
  or one too short for its fixed fields.
- The rules of the tables are in [module-abi.md](module-abi.md#versioning-and-evolution).

### Protocol

A module's `pmp_version` (from its manifest) MUST be in the runtime's compatibility class and MUST
NOT be newer than the runtime's PMP version (`PROTOCOL_INCOMPATIBLE`). For the runtime's PMP 0.1.0:

| Manifest `pmp_version` | Accepted | Why |
|---|---|---|
| `0.1.0`, `0.1.0+build.7` | yes | Equal (build metadata ignored). |
| `0.1.0-rc.1` | yes | Same class; a pre-release precedes its release. |
| `0.1.1` | no | Newer than the runtime's. |
| `0.2.0`, `1.0.0` | no | Another compatibility class. |

The `pmp_version` that `describe()` reports is checked for its form only.

### Document formats

- A document is read with the schema of its `format_version`'s major and minor: runtime 0.2.0 reads
  format `0.1.x` of every kind, and `0.2.x` of `module-manifest` and `run-record`, and nothing else
  (`FORMAT_VERSION_UNSUPPORTED`, naming the formats it reads). `praenora schema list` prints the
  embedded schemas.
- The runtime writes each kind at its current format: run records of format 0.2.0, which add the
  services of the run to the reproducibility section ([reproducibility.md](reproducibility.md)).
  `praenora_add_module()` of SDK 0.2.0 writes module manifests of format 0.2.0, which may declare
  service requirements ([manifest.md](manifest.md)), also when a module declares none. A runtime
  0.1 reads neither: a module built with SDK 0.2 needs a runtime 0.2 or later, while a runtime 0.2
  still reads the manifests that SDK 0.1 wrote. A manifest of format 0.1 declares no service.
- Once versions are bumped again ([Changing a version](#changing-a-version); for now a format
  changes in place), a change to what a format's schema accepts MUST be a new format version with a
  schema file of its own (`<kind>.<major>.<minor>.schema.json`). Patch versions share their
  schema, so a patch change MUST NOT change what the schema accepts.

### Catalog entries

In v0 every reference to a catalog entry is exact (build metadata ignored), and the policy for
evolving message types and capabilities is open ([open-questions.md](../open-questions.md), OQ-7):

- a workflow step names one capability version, and a port one message type version;
- the two ends of a connection carry the same message type id (`PORT_TYPE_MISMATCH`) and version
  (`MESSAGE_TYPE_INCOMPATIBLE`);
- a module implements a step's capability only when its manifest lists exactly that version
  (`CAPABILITY_NOT_IMPLEMENTED`), and its `describe()` document lists the same set of capabilities as
  its manifest (`MODULE_IDENTITY_MISMATCH`).

Once versions are bumped again, a published entry is never changed; a change is a new version
([workflow-definitions.md](workflow-definitions.md#versioning)). For now an entry changes in place
([Changing a version](#changing-a-version)).

### Modules

- A step binding pins a module's exact version. Resolution compares versions by SemVer precedence,
  build metadata ignored, and takes the first manifest for the host platform in search order
  ([manifest.md](manifest.md#discovery)). Version ranges and lock files are open (OQ-4, OQ-14).
- A manifest's `platform.os` and `platform.arch` MUST equal the host's (`PLATFORM_MISMATCH`). The
  host platform is the one the runtime was compiled for: os `linux`, `macos` or `windows`, arch
  `x86_64` or `arm64`.
- `describe()` MUST agree with the manifest on name, version, ABI major, SDK version and
  capabilities ([manifest.md](manifest.md#identity-check)).

### Service providers

Service providers are built with the runtime and shipped with it; their interface is internal to the
runtime and may change with any release ([runtime-services.md](runtime-services.md)). The runtime
uses a provider only when its manifest is for the host platform, its `runtime_version` equals the
runtime's version (SemVer equality, build metadata ignored) and its `abi.major` equals the provider
interface major; otherwise the provider is listed as unusable, with the reason. A provider's
`describe()` MUST agree with its manifest on name, version, services and engine
([runtime-services.md](runtime-services.md#loading)).

A service a module requires is satisfied by a provider that offers the same name and major at a
minor of at least the required one; a later minor of a service only appends to its table
([runtime-services.md](runtime-services.md#services)). A required service that no usable provider
offers is `SERVICE_UNAVAILABLE`.

### Order of the checks

For each step, validation applies the rules in this order and reports the first one a module
breaks: platform, ABI major, PMP version, capability; then every required service that does not
resolve, and the library file and its pinned digest; then, for the run, the library file of each
provider it uses. Loading then checks the table and the identity
([module-abi.md](module-abi.md#loading-and-table-acceptance)), and a run first loads its providers
([runtime-services.md](runtime-services.md#runs-and-checks)).

## Changing a version

For now no version is bumped: nothing outside this repository uses the runtime, the SDK, the ABI,
the document formats or the catalog yet, so a change goes into the current versions in place,
schemas, catalog entries and this repository's modules included
([AGENTS.md](../../AGENTS.md#change-rules)). Until versions are bumped again, only these rules wait:
the pre-1.0 convention ([Semantic versions](#semantic-versions)), the "Version to bump" column
below, the rule that a change to what a schema accepts is a new format version, the rule that a
changed catalog entry is a new version, and the deprecation period (a feature deprecated one release
before its removal). The "Also required" column applies now, and so do the checks of the rules
above, which the runtime enforces whatever the versions.

| Change | Version to bump | Also required |
|---|---|---|
| An ABI change that existing modules or hosts cannot handle | `PRAENORA_ABI_MAJOR` in `abi.h` (a new table generation) | An ADR. |
| A function appended to the module table or a callback to the host API | `PRAENORA_ABI_MINOR` in `abi.h` | An ADR. |
| A breaking protocol change | PMP `y` (before 1.0), in `cmake/PraenoraVersions.cmake` | An ADR; schemas, catalog and documentation together. |
| A change to what a document schema accepts | That kind's format version, with a new schema file | An ADR; the documentation. |
| A change to a catalog entry | The entry's version (a new file) | Regenerated AsyncAPI documents when a workflow definition, message type or the envelope changes. |
| A change to the provider interface | `PRAENORA_PROVIDER_ABI_MAJOR` or `PRAENORA_PROVIDER_ABI_MINOR` in [`provider_abi.h`](../../src/runtime/provider/provider_abi.h) | An ADR. |
| A change to the simulation service | `PRAENORA_SIMULATION_SERVICE_MAJOR` (breaking) or `PRAENORA_SIMULATION_SERVICE_MINOR` (appended functions, optional document fields) in [`simulation.h`](../../include/praenora/sdk/simulation.h), and the SDK version | An ADR; [simulation-service.md](simulation-service.md) and the service checks together. |
| A change to the model conversion service | `PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR` (breaking) or `PRAENORA_MODEL_CONVERSION_SERVICE_MINOR` (appended functions, optional option and report fields) in [`model_conversion.h`](../../include/praenora/sdk/model_conversion.h), and the SDK version | An ADR; [model-conversion-service.md](model-conversion-service.md) and the service checks together. |
| A runtime release | `project(VERSION)` | Rebuilt service providers: a provider is usable only with the runtime version it was built with. |

- The runtime's shared library carries `VERSION` 0.2.0 and `SOVERSION` 0.2
  (`libpraenora_runtime.0.2.dylib` on macOS); in a Python wheel its `VERSION` is its `SOVERSION`,
  so the wheel holds one file and no link ([python.md](../python.md#building-a-wheel)), and its
  Mach-O current version stays 0.2.0. The installed CMake package checks versions with
  `SameMinorVersion` while the version is 0.x, so `find_package(Praenora 0.2)` accepts every 0.2.z.
- The C++ API is source-compatible under SemVer but not binary-stable across compilers. The only
  binary-stable interface is the module ABI.
- **Deprecation.** A feature is deprecated in one release before it is removed in the next breaking
  release. A superseded decision gets a new ADR, and the old ADR is marked Superseded.

## References

- [ADR-0003](../adr/ADR-0003-native-module-abi-v0.md): the ABI and its version.
- [ADR-0014](../adr/ADR-0014-versioning-and-compatibility-policy.md): versioning and compatibility.
- [ADR-0026](../adr/ADR-0026-runtime-service-providers.md): service providers and their versions.
- [ADR-0027](../adr/ADR-0027-simulation-service-v0.md): the simulation service and its version.
- [open-questions.md](../open-questions.md): OQ-4 (module pins), OQ-7 (evolution of message types),
  OQ-14 (module lock files).
- [module-developer-guide.md](../module-developer-guide.md#versioning): versioning a module.
