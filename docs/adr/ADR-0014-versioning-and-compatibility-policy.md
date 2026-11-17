# ADR-0014: Versioning and compatibility policy

- Status: Accepted (scope noted in Decision)
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

The runtime, SDK, protocol, documents, catalog entries and modules evolve at different rates, and independent teams ship modules. The host must reject incompatible combinations early and say why.

## Decision

Status split: SemVer for packages and an ABI version independent of package versions are Accepted (required by the project); the compatibility rules are Proposed.

Accepted:
- Runtime package, SDK, PMP, document formats, catalog entries and modules follow SemVer. Runtime, SDK and PMP start at 0.1.0.
- The ABI version is a `major.minor` pair starting at 0.0, owned by `include/praenora/sdk/abi.h`, independent of package versions.

Proposed rules:
- **ABI:** the host accepts iff the major is equal; the minor is negotiated through `struct_size` on both tables.
- **PMP:** compatible iff the compatibility class matches (`0.y.*` with `0.y.*` before 1.0; equal major afterwards) and module version is not newer than the host.
- **Document formats:** the host lists supported versions; anything else is `FORMAT_VERSION_UNSUPPORTED`.
- **Catalog entries:** v0 requires exact version equality on connections and between step capabilities and those declared by modules (OQ-7).
- **SDK version:** informational; modules report it in `describe` and it is recorded.
- **C++ adapter:** refuses `create` with `UNSUPPORTED` when a host function it needs is missing (negotiated through `struct_size`).
- **Modules:** configuration pins an exact version (OQ-4).
- **Pre-1.0:** a change in `y` of `0.y.z` may break compatibility, a change in `z` must not.
- **CMake package version file:** `SameMinorVersion` while the version is 0.x. The shared library gets `VERSION 0.1.0` and `SOVERSION 0.1`.
- **Deprecation:** a feature is deprecated one release before removal in the next breaking release.
- CMake owns package, SDK and PMP versions (`cmake/PraenoraVersions.cmake`).

### Builder notes (Proposed)

- The runtime package version has one source: `project(VERSION)` in the top-level `CMakeLists.txt`, where packaging tools can read it. `cmake/PraenoraVersions.cmake` republishes it as `PRAENORA_RUNTIME_VERSION`, defines `PRAENORA_SDK_VERSION`, `PRAENORA_PMP_VERSION` and `PRAENORA_FORMAT_VERSION`, and reads `PRAENORA_ABI_MAJOR` and `PRAENORA_ABI_MINOR` from `abi.h`. Without that header the ABI version is 0.0; a header that cannot be parsed fails the configuration.
- `praenora::version_info()` reports `formats` for the nine document kinds that carry `format_version`, all at `0.1.0`; `platform` as `<os>-<arch>` in manifest vocabulary (for example `macos-arm64`); `compiler` as the CMake compiler id and version; and `build_type` as the build configuration.
- The host platform comes from the compiler's target macros (`_WIN32`, `__APPLE__`, `__linux__`; `__x86_64__`/`_M_X64`, `__aarch64__`/`__arm64__`/`_M_ARM64`); anything else is `unknown`, which no manifest matches. A test checks that it agrees with `version_info().platform`, which the build derives the way `praenora_add_module()` writes manifests.
- `check_module_compat()` returns the first rule a manifest breaks as an error diagnostic located at the manifest file and the field's pointer (`/platform`, `/abi/major`, `/pmp_version`, `/capabilities`), naming the module. Any ABI minor is accepted. A pre-release of the runtime's own PMP version precedes it, so it is accepted. Capability versions must be equal by SemVer precedence (build metadata ignored).
- The installed `PraenoraConfigVersion.cmake` comes from `write_basic_package_version_file(... COMPATIBILITY SameMinorVersion)`, which also refuses a consumer of another pointer size, since the package holds a compiled library. The installed `PraenoraConfig.cmake` defines `PRAENORA_SDK_VERSION`, `PRAENORA_PMP_VERSION`, `PRAENORA_ABI_MAJOR` and `PRAENORA_ABI_MINOR`, the versions the package was built with; `praenora_add_module()` writes them into the manifests of modules built against the installation, as `cmake/PraenoraVersions.cmake` does for the modules of this repository.
- In a Python wheel build the runtime library's `VERSION` is its `SOVERSION` (0.1), so the wheel holds one library file named by its soname instead of a second copy for the soname link, which a wheel cannot store; its Mach-O current version stays the full runtime version ([ADR-0018](ADR-0018-python-bindings-with-swig.md), [python.md](../python.md#building-a-wheel)). Builds and installations keep `VERSION` 0.1.0.
- Later: [ADR-0026](ADR-0026-runtime-service-providers.md) makes the document format versions per kind (`PRAENORA_FORMAT_VERSIONS` replaces `PRAENORA_FORMAT_VERSION`).

## Alternatives considered

- **SemVer ranges and a lock file for modules.** More flexible, but needs resolution rules and a lock format. Deferred (OQ-4, OQ-14).
- **Tying the ABI version to the package version.** Forces module rebuilds on every runtime release.
- **Compatible-by-schema checks only.** Cannot express behavioral changes.

## Consequences

- Compatibility decisions are mechanical and testable.
- Exact pins make configurations reproducible but require editing on module updates.
- Pre-1.0 versions need care: any `y` bump can break consumers.

## References

- [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html)
- [docs/specs/compatibility-policy.md](../specs/compatibility-policy.md)
- [docs/open-questions.md](../open-questions.md)
