# ADR-0033: describe() and the manifest agree on the SDK version

- Status: Superseded
- Date: 2026-11-17
- Supersedes: none
- Superseded by: [ADR-0036](ADR-0036-describe-and-manifest-agree-on-every-field-they-share.md)

## Context

A module reports the SDK it was built with twice: in its manifest's `sdk_version`, which
`praenora_add_module()` writes from the SDK it builds with, and in the `sdk_version` of its
`describe()` document, which the C++ adapter (`module.hpp`) and C modules take from
`PRAENORA_SDK_VERSION_STRING` of the SDK they compile against. The SDK version decides no
compatibility ([ADR-0014](ADR-0014-versioning-and-compatibility-policy.md)), and the identity check
after loading compared name, version, ABI major and capabilities only
([ADR-0009](ADR-0009-module-manifests.md)), so the two values were never compared.

The run fingerprint holds the manifest's value, because it is computed before any library is loaded
([ADR-0016](ADR-0016-reproducibility.md)). A library rebuilt against another SDK and left beside its
old manifest, with the same name, version and capabilities, therefore loaded and ran, and its run
record named an SDK that the library contradicts. Only a manifest that pins `library.sha256` caught
it.

## Decision

- The identity check compares `sdk_version` too: the `describe()` document's MUST equal the
  manifest's by SemVer precedence, build metadata ignored, as `version` does. A difference is
  `MODULE_IDENTITY_MISMATCH`, named in the message (`sdk_version (manifest 9.9.9, describe()
  0.2.0)`) and in `details.differences` (`{"field": "sdk_version", "manifest": "9.9.9",
  "describe": "0.2.0"}`) beside every other differing field.
- A `describe()` whose `sdk_version` cannot be compared (the schema's pattern admits numbers beyond
  64 bits) gives no usable identity: `MODULE_IDENTITY_MISMATCH`, as for its `version`.
- The SDK version still decides no compatibility: no SDK version is refused for being older or newer
  than the runtime's. The rule is that the two documents agree.
- The run fingerprint and the run record keep the manifest's `sdk_version`. In a run that loads the
  module, the library now confirms that value. No schema, document format or version changes.
- `praenora modules check` reports a difference under `describe.identity`, and the message of a
  passing `describe.identity` names the SDK version.
- `abi.minor` and `pmp_version` of the `describe()` document are still not compared with the
  manifest.

### Builder notes (Proposed)

- The SDK version is compared after `abi.major` and before the capabilities, so the differences are
  listed in the order of the `describe()` document's fields.
- Every example module and every fixture module except `fx-other-sdk` and `fx-sdk-overflow`
  (below) takes both values from `PRAENORA_SDK_VERSION` (`cmake/PraenoraVersions.cmake`): the
  manifests through `@PRAENORA_SDK_VERSION@`, `describe()` through `PRAENORA_SDK_VERSION_STRING`.
  None of them changes.
- The tests give the loader an `fx-echo` manifest that names SDK 9.9.9, the SDK's own version with
  build metadata (accepted) and with a pre-release (refused); the contract checks get a copy of
  `fx-echo` whose manifest names SDK 9.9.9. The fixture `fx-other-sdk` reports SDK 0.0.1 in both
  documents and loads, which shows that the runtime's own SDK version plays no part;
  `fx-sdk-overflow`, built from the same source, reports an SDK version whose minor number does not
  fit in 64 bits and is refused. One more loader test pins the order of the differences.

## Alternatives considered

- **A warning.** `LoadedModule::load()` either returns a checked module or throws. Its callers (a
  run, `validate --load`, `modules inspect --load`, `modules check`) report in different ways, and a
  run record has no place for warnings, so a warning would reach the logs at best, while the record
  kept a value that the library contradicts.
- **Record the value `describe()` reports.** The fingerprint is computed during validation, before
  any library is loaded, and `validate` without `--load` loads none, so that value is not known in
  time.
- **Compare `abi.minor` and `pmp_version` as well.** The stale-manifest argument applies to them
  too, but they are left for a decision of their own: the manifest's `pmp_version` decides
  compatibility, and the ABI minor is negotiated through the table's `struct_size`.

## Consequences

- A manifest left beside a library rebuilt against another SDK is refused when the module is
  loaded, also when the manifest pins no digest. The fix is to rebuild the module, which writes its
  manifest again, or to correct a manifest written by hand.
- A module built without `praenora_add_module()` must write the same SDK version into both
  documents.
- Nothing outside this repository uses the runtime yet, so no published module is affected.

## References

- [docs/specs/manifest.md](../specs/manifest.md#identity-check)
- [docs/specs/protocol.md](../specs/protocol.md#describe-document)
- [docs/specs/compatibility-policy.md](../specs/compatibility-policy.md#versioned-items)
- [docs/specs/reproducibility.md](../specs/reproducibility.md#run-fingerprint)
- [ADR-0009](ADR-0009-module-manifests.md),
  [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md),
  [ADR-0016](ADR-0016-reproducibility.md)
