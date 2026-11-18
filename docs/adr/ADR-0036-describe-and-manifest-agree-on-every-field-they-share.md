# ADR-0036: describe() and the manifest agree on every field they share

- Status: Proposed
- Date: 2026-11-18
- Supersedes: [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md)
- Superseded by: none

## Context

A module reports its identity twice: in its manifest, which the runtime reads before any library is
loaded, and in the document its `describe()` writes after loading. The two share the fields `name`,
`version`, `abi` (`major` and `minor`), `sdk_version`, `pmp_version` and the capabilities. The
identity check ([ADR-0009](ADR-0009-module-manifests.md)) compared name, version, ABI major and
capabilities, and [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md) added
`sdk_version`, so that a manifest left beside a library rebuilt against another SDK is refused. It
left `abi.minor` and `pmp_version` for a decision of their own.

The same stale-manifest argument applies to both, with a sharper edge for one of them:

- The manifest's `pmp_version` decides protocol compatibility before the library is loaded
  ([ADR-0014](ADR-0014-versioning-and-compatibility-policy.md)). A manifest that names PMP 0.1.0
  beside a library rebuilt to speak PMP 0.2.0 passes that rule, and the runtime would then load the
  library and send it documents of a protocol it does not implement. The `pmp_version` that
  `describe()` reported was checked for its form only.
- The manifest's `abi.minor` decides nothing: any minor is accepted, and the tables' `struct_size`
  negotiates what each side may use. A stale minor misinforms, though: it appears in the
  `ABI_INCOMPATIBLE` message and in the results of `praenora modules check`.

`praenora_add_module()` writes both values into the manifest from the SDK it builds with, and the
C++ adapter's `describe()` reports the same SDK's `PRAENORA_ABI_MINOR` and
`PRAENORA_PMP_VERSION_STRING`, so a module built that way never reports two different values.

## Decision

- The identity check compares every field the two documents share. The `describe()` document MUST
  agree with the manifest on `name`; `version`, by SemVer precedence; `abi.major` and `abi.minor`,
  as integers; `sdk_version`, by SemVer precedence; `pmp_version`, by SemVer precedence; and the
  capabilities, compared as sets of `{id, version}`. Build metadata is ignored in every precedence
  comparison.
- A difference is `MODULE_IDENTITY_MISMATCH`. The message names every differing field
  (`abi.minor (manifest 7, describe() 0)`, `pmp_version (manifest 0.1.0, describe() 0.2.0)`), and
  so does `details.differences` (`{"field": "pmp_version", "manifest": "0.1.0", "describe":
  "0.2.0"}`), in the order of the `describe()` document's fields: name, version, abi.major,
  abi.minor, sdk_version, pmp_version, capabilities.
- A `describe()` whose `version`, `sdk_version` or `pmp_version` cannot be compared (the schema's
  pattern admits numbers beyond 64 bits) gives no usable identity: `MODULE_IDENTITY_MISMATCH`.
- The compatibility rules still read the manifest before the library is loaded, and are unchanged:
  any ABI minor is accepted, and the PMP version must be in the runtime's compatibility class and
  not newer. Since `describe()` must report the same ABI and PMP versions, what they decided holds
  for the library too.
- From [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md), unchanged: the SDK
  version decides no compatibility, and no SDK version is refused for being older or newer than the
  runtime's; the run fingerprint and the run record keep the manifest's `sdk_version`.
- No schema, document format, fingerprint or version changes: a manifest's digest is in the
  fingerprint already.
- `praenora modules check` reports a difference under `describe.identity`, and the message of a
  passing `describe.identity` names the versions it compared: `describe() agrees with the manifest:
  fx-echo@0.1.0, ABI 0.0, SDK 0.2.0, PMP 0.1.0, 1 capability`.

### Builder notes (Proposed)

- `abi.minor` is compared as `abi.major` is: a value in `describe()` that is not an unsigned integer
  equal to the manifest's differs, and the difference shows the value as the host read it.
- The table header's `abi_minor` is not compared with either document.
- Every example module and every fixture module except `fx-other-protocol`, `fx-newer-pmp` and
  `fx-pmp-overflow` reports the ABI minor and the PMP version of the SDK it is built with, in both
  documents; none of them changes. The C fixtures write ABI 0.0 by hand, which is the SDK's;
  `c_fixture.h` stops compiling when the SDK's ABI minor is no longer 0.
- The fixture `fx-other-protocol` reports ABI 0.7 in its manifest, its `describe()` and its
  table's header, and PMP 0.1.0-rc.1 in its manifest and its `describe()`. It loads, and its
  manifest passes the compatibility rules, which shows that the runtime's own ABI minor and PMP
  version play no part when they are compatible. `fx-newer-pmp`, built from the same source,
  reports PMP 0.2.0 beside its generated manifest's 0.1.0: the manifest passes the compatibility
  rules and the library is refused. `fx-pmp-overflow` reports a PMP version whose minor number does
  not fit in 64 bits and is refused. `c_fixture.h` gains `C_FIXTURE_DESCRIBE_JSON_VERSIONS()`.
- The loader tests give `fx-echo` a manifest that names ABI minor 7, PMP 0.1.0-rc.1 (refused) and
  PMP 0.1.0 with build metadata (accepted), and pin the order of all seven fields; the contract
  checks get a copy of `fx-echo` whose manifest names ABI minor 7, and `fx-newer-pmp`.

## Alternatives considered

- **Refuse a `pmp_version` difference only.** It closes the protocol hazard, but keeps an exception
  to explain, and a stale minor still shows in messages and contract check results.
- **Compare neither, and say why.** It leaves the runtime able to load a library that speaks a
  protocol its manifest does not name.
- **Apply the protocol rule to `describe()`'s `pmp_version` instead of requiring equality.** It
  would accept a library whose PMP version differs from its manifest's within the compatibility
  class, so the manifest, whose digest is in the fingerprint, would still contradict the library.
  Equality is simpler and implies compatibility.
- **A warning.** As in [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md):
  `LoadedModule::load()` either returns a checked module or throws, and a run record has no place
  for warnings.

## Consequences

- A manifest whose ABI minor or PMP version differs from what its library reports is refused when
  the module is loaded, also when the manifest pins no digest. The fix is to rebuild the module,
  which writes its manifest again, or to correct a manifest written by hand.
- A module built without `praenora_add_module()` must write the same ABI and PMP versions into both
  documents, as it must the SDK version. A module whose `describe()` is written by hand, as in C,
  must report the ABI minor and the PMP version of the SDK it builds with, since
  `praenora_add_module()` writes those into the manifest: when a later SDK raises the ABI minor,
  such a module is refused until its `describe()` is updated.
- Nothing outside this repository uses the runtime yet, so no published module is affected.

## References

- [docs/specs/manifest.md](../specs/manifest.md#identity-check)
- [docs/specs/protocol.md](../specs/protocol.md#describe-document)
- [docs/specs/compatibility-policy.md](../specs/compatibility-policy.md#rules)
- [docs/specs/module-abi.md](../specs/module-abi.md)
- [docs/specs/reproducibility.md](../specs/reproducibility.md#run-fingerprint)
- [ADR-0004](ADR-0004-lifecycle-functions-and-documents.md),
  [ADR-0009](ADR-0009-module-manifests.md),
  [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md),
  [ADR-0016](ADR-0016-reproducibility.md),
  [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md)
