# Versioning and compatibility policy

- **Version:** applies to runtime 0.1.0, SDK 0.1.0, PMP 0.1.0, ABI 0.0 and the document formats
  0.1.0
- **Status:** Normative, MVP. Semantic versions for the packages and an ABI version independent of
  them are Accepted; the compatibility rules are Proposed
  ([ADR-0014](../adr/ADR-0014-versioning-and-compatibility-policy.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The runtime, the module SDK, the protocol, the document formats, the catalog entries and the modules
change at different rates, and independent teams ship modules. This policy says how each is
versioned and which combinations the runtime accepts. Every rule is decided from manifests and
documents before any module code runs, except the checks of a loaded library's table and identity.

## Versioned items

| Item | Scheme | Current | Declared in | Rule the runtime enforces |
|---|---|---|---|---|
| Runtime package | SemVer | 0.1.0 | `project(VERSION)` in the top-level `CMakeLists.txt` | None; reported by `praenora version` and recorded in the run record. |
| Module SDK | SemVer | 0.1.0 | `cmake/PraenoraVersions.cmake`, published in the generated `praenora/sdk/version.h` | None; informational. Manifests and `describe()` report it; the run record holds the manifest's. |
| Module ABI | `major.minor` | 0.0 | [`abi.h`](../../include/praenora/sdk/abi.h); the table header; the manifest's `abi` | The major is equal; the minor is negotiated. |
| Protocol (PMP) | SemVer | 0.1.0 | `cmake/PraenoraVersions.cmake`; `pmp_version` in manifests and ABI documents | Same compatibility class and not newer than the runtime's. |
| Document formats, one per kind | SemVer | 0.1.0 each | `format_version` in each document | The runtime has a schema for the version's major and minor. |
| Catalog entries | SemVer per entry | 0.1.0 | `version` of each entry | References name exact versions. |
| Modules | SemVer | the module's | the manifest's `module.version` and `describe()` | The configuration pins an exact version. |

`praenora version --json` prints `{"ok": true, "result": ...}`, whose `result` holds the runtime,
SDK, PMP and ABI versions and the format version of every document kind that carries
`format_version`.

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

- A document is read with the schema of its `format_version`'s major and minor: runtime 0.1.0 reads
  format `0.1.x` of every kind and nothing else (`FORMAT_VERSION_UNSUPPORTED`, naming the formats it
  reads). `praenora schema list` prints the embedded schemas.
- A change to what a format's schema accepts MUST be a new format version with a schema file of its
  own (`<kind>.<major>.<minor>.schema.json`). Patch versions share their schema, so a patch change
  MUST NOT change what the schema accepts.

### Catalog entries

In v0 every reference to a catalog entry is exact (build metadata ignored), and the policy for
evolving message types and capabilities is open ([open-questions.md](../open-questions.md), OQ-7):

- a workflow step names one capability version, and a port one message type version;
- the two ends of a connection carry the same message type id and version (`PORT_TYPE_MISMATCH`);
- a module implements a step's capability only when its manifest lists exactly that version
  (`CAPABILITY_NOT_IMPLEMENTED`), and its `describe()` document lists the same set of capabilities as
  its manifest (`MODULE_IDENTITY_MISMATCH`).

A published entry is never changed; a change is a new version
([workflow-definitions.md](workflow-definitions.md#versioning)).

### Modules

- A step binding pins a module's exact version. Resolution compares versions by SemVer precedence,
  build metadata ignored, and takes the first manifest for the host platform in search order
  ([manifest.md](manifest.md#discovery)). Version ranges and lock files are open (OQ-4, OQ-14).
- A manifest's `platform.os` and `platform.arch` MUST equal the host's (`PLATFORM_MISMATCH`). The
  host platform is the one the runtime was compiled for: os `linux`, `macos` or `windows`, arch
  `x86_64` or `arm64`.
- `describe()` MUST agree with the manifest on name, version, ABI major and capabilities
  ([manifest.md](manifest.md#identity-check)).

### Order of the checks

For each step, validation applies the rules in this order and reports the first one a module
breaks: platform, ABI major, PMP version, capability, then the library file and its pinned digest.
Loading then checks the table and the identity ([module-abi.md](module-abi.md#loading-and-table-acceptance)).

## Changing a version

| Change | Version to bump | Also required |
|---|---|---|
| An ABI change that existing modules or hosts cannot handle | `PRAENORA_ABI_MAJOR` in `abi.h` (a new table generation) | An ADR. |
| A function appended to the module table or a callback to the host API | `PRAENORA_ABI_MINOR` in `abi.h` | An ADR. |
| A breaking protocol change | PMP `y` (before 1.0), in `cmake/PraenoraVersions.cmake` | An ADR; schemas, catalog and documentation together. |
| A change to what a document schema accepts | That kind's format version, with a new schema file | An ADR; the documentation. |
| A change to a catalog entry | The entry's version (a new file) | Regenerated AsyncAPI documents when a workflow definition, message type or the envelope changes. |
| A runtime release | `project(VERSION)` | None by this policy. |

- The runtime's shared library carries `VERSION` 0.1.0 and `SOVERSION` 0.1
  (`libpraenora_runtime.0.1.dylib` on macOS); in a Python wheel its `VERSION` is its `SOVERSION`,
  so the wheel holds one file and no link ([python.md](../python.md#building-a-wheel)), and its
  Mach-O current version stays 0.1.0. The installed CMake package checks versions with
  `SameMinorVersion` while the version is 0.x, so `find_package(Praenora 0.1)` accepts every 0.1.z.
- The C++ API is source-compatible under SemVer but not binary-stable across compilers. The only
  binary-stable interface is the module ABI.
- **Deprecation.** A feature is deprecated in one release before it is removed in the next breaking
  release. A superseded decision gets a new ADR, and the old ADR is marked Superseded.

## References

- [ADR-0003](../adr/ADR-0003-native-module-abi-v0.md): the ABI and its version.
- [ADR-0014](../adr/ADR-0014-versioning-and-compatibility-policy.md): versioning and compatibility.
- [open-questions.md](../open-questions.md): OQ-4 (module pins), OQ-7 (evolution of message types),
  OQ-14 (module lock files).
- [module-developer-guide.md](../module-developer-guide.md#versioning): versioning a module.
