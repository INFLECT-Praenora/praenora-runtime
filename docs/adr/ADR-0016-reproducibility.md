# ADR-0016: Reproducibility: run fingerprint, run record, seed derivation, determinism contract

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Results must be reproducible, and the runtime must eliminate each source of nondeterminism or record it. Users need to know whether two runs are comparable.

## Decision

- **Fingerprint:** `SHA-256(canonical_json(reproducibility_section))`. The section contains runtime, ABI and PMP versions; the workflow definition id, version and digest and the digests of the catalog entries used; the resolved semantic configuration (steps, modules, parameters with defaults, time base, seed, input bindings; excluding `outputs` and `observability`); per step the module name, version, manifest digest, library digest and the SDK version reported by `describe`; the digest of every input source; and the run seed with the derived step seeds.
- **Canonical JSON:** nlohmann `dump()` with no indent on `nlohmann::json` (sorted keys), UTF-8, locale-independent shortest round-trip numbers. Full RFC 8785 compliance is not claimed (OQ-20).
- **Seeds:** lie in [0, 9007199254740991]; default 0. Step seed is the first 8 bytes, big-endian, of `SHA-256("praenora-seed-v1" || u64be(run.seed) || utf8(step_id))`, masked to 2^53 - 1.
- **Runtime determinism:** deterministic schedule, ordered containers wherever order reaches outputs, deterministic envelope ids (`<fp16>:<tick>:<source>`), canonical serialization, no wall-clock time in messages or in the `reproducibility` section.
- **Module determinism contract** (checked by `invoke.determinism`): no wall-clock dependence, randomness only from the provided seed, deterministic iteration order, no data races, no uninitialized memory.
- **Guarantee:** same fingerprint, same platform, same binaries gives byte-identical outputs. Across platforms outputs agree within numeric tolerance. Example modules compile with `-ffp-contract=off` on GCC and Clang.
- **Run record** (`kind: run-record`): the hashed `reproducibility` section plus an `execution` section (start and finish times, status, ticks completed, error, platform, output digests and counts, final status snapshot). It records no host name or user name.

### Builder notes (Proposed)

- The module SDK version in the fingerprint is the `sdk_version` declared in the module manifest, not a value computed at run time or reported by `describe()`: the identity check compares name, version, ABI major and capabilities only (ADR-0009), so a `describe()` that reports another SDK version is not refused.
- `canonical_json()` refuses strings that are not valid UTF-8 (`DATA_INVALID`) instead of replacing their bytes, so a fingerprint never hides a substitution.
- SHA-256 is an in-tree FIPS 180-4 implementation (no dependency); step seeds are checked against fixed test vectors, for example run seed 20261110 and step `estimate` give 4059344203658876.
- The reproducibility section has exactly the shape of the `run-record` schema: `versions {runtime, abi {major, minor}, pmp}`, `workflow {name, version, digest}`, `catalog {"<id>@<version>": digest}`, `time {unit, start, period}`, `seed`, `ticks` (the run's tick count, or `null` when the run lasts until its replay inputs are exhausted), `timeouts {invoke, lifecycle, cancel_grace}` (the run's), `inputs {<input>: {source, digest[, columns]}}` and `steps {<step>: {capability {id, version}, module {name, version, manifest_sha256, library_sha256, sdk_version}, parameters, seed, timeouts}}`. Canonical JSON sorts every object's keys, so the order in which the section is built does not matter.
- `catalog` holds the digests of the capabilities of the steps and of every message type that the workflow's inputs and outputs and those capabilities' ports use; the workflow definition's own digest is under `workflow`. When a capability and a message type used by one workflow share an `id@version`, both keys take the form `<kind>:<id>@<version>`.
- Each step's `timeouts` are the effective ones (the run's, with the step's fields put over them), written canonically: the count in the largest of `s`, `ms`, `us` and `ns` that holds it exactly (`"300s"`), and `"none"` when disabled.
- The tick count is part of the semantic configuration, because a different tick count gives different outputs: it is in the section (`ticks`, after any override of the caller), so it changes the fingerprint, and so do the run-level timeouts. Not in the section: paths (configuration, input files, manifests, libraries), `outputs` and `observability` settings, step log levels and wall-clock time. The same configuration and data at other paths give the same fingerprint.
- Input digests are the SHA-256 of the input file's bytes (CSV or document), so re-encoding a file changes the fingerprint even when its content reads the same. Library digests are computed during validation from the file, without loading it.
- Parameters are hashed after the module's schema defaults are filled in, so an explicit default and an omitted parameter give the same fingerprint. Numbers keep their JSON type, so `100` and `100.0` hash differently (OQ-20).
- The `run-record` schema's `execution` section follows the specification: `status` is the run's status (`running` from the start, then `completed`, `stopped` or `failed`), and the final status snapshot is `final_status`. `overrides` (the `seed` and `ticks` the caller overrode, `{}` when none) and `validation` (`strict` or `off`) are required as well.
- `final_status` is written when the run ends, taken after the teardown with the final state; the record written at the start has none.
- The record is written when the run starts, before any module is loaded, and again when it ends, each time replacing the file atomically, as JSON indented by two spaces with a final newline. Without an output directory it is kept in memory only. The writer refuses a fingerprint that is not the hash of the reproducibility section.
- `execution.error` is the failure of a failed run, a `RUN_STOPPED` record for a stopped one, and null otherwise. At the end, `execution.outputs` lists every recorded output as `{file, sha256, messages}`, the file relative to the output directory (`outputs/<output>.jsonl`), even when it holds no message; `execution.trace` describes `trace/messages.jsonl` the same way when tracing is on, its `messages` counting the envelopes (not the records that carry only diagnostics).
- An output file or a final record that cannot be written turns a completed or stopped run into a failed one (`IO_ERROR`).
- Later: [ADR-0028](ADR-0028-error-records-keep-location-and-details.md) gives `execution.error` the location and details of the error.
- The example modules (`examples/modules/`) follow the determinism contract with nothing but `+ - * /` on doubles, and are compiled with `-ffp-contract=off` on GCC and Clang, so no multiply and add are fused into one rounding (AppleClang fuses them by default on arm64). Choices of their own: the estimator emits, per series and variable, the estimate at its latest usable observation, takes only `latent_mean` prior rows, and carries a prior over to the window's end, which is the invocation's `logical_time`; the predictor counts its output times as `j * output_interval <= horizon` up to a relative 1e-9, so that decimal parameters such as 0.3 and 0.1 give the expected count, integrates forward only (an estimate after the window's end is `INVALID_ARGUMENT`), takes a remainder within a relative 1e-9 of `step_size` as the last, shortened step instead of adding a step of almost nothing, and refuses parameters that need more than 10^7 steps or 10^5 rows per estimate.
- `tools/generate_example_data.py` writes the example's observations: times are whole hundredths of a day, so they are exact decimal text, and values are written with three decimals, which makes the file practically independent of the last digits of the platform's `exp` and `sin`; it writes bytes with `\n` line endings on every platform.
- The golden outputs of the example (`tests/fixtures/golden/example.estimate-predict/`) are its recorded output files with sixteen zeros in place of the run fingerprint's first 16 hex digits in each envelope `id`: the fingerprint covers the module libraries' digests, which differ from build to build. They are compared with a relative tolerance of 1e-9 and an absolute one of 1e-12 (`json_near()`, ADR-0024), and rewritten only with `PRAENORA_UPDATE_GOLDEN=1`.
- The run record identifies each step's module by name, version, manifest digest and library digest. Neither section holds a path, the resolved manifest's included, so the records of runs made at different paths compare equal; `praenora modules list` and `praenora modules inspect` show where a manifest is.
- Later: [ADR-0029](ADR-0029-run-record-says-where-manifests-were-found.md) adds `execution.manifests`, where each module and provider manifest was found, relative to its search path and never as an absolute path; the reproducibility section still holds no path, and the fingerprint does not change.
- Later: [ADR-0033](ADR-0033-describe-and-manifest-agree-on-the-sdk-version.md) makes the identity check compare `sdk_version` as well, so a `describe()` that reports another SDK version than the manifest is refused (`MODULE_IDENTITY_MISMATCH`). The fingerprint still holds the manifest's value, which in any run that loads the module equals the one `describe()` reports by SemVer precedence (build metadata may differ).

## Alternatives considered

- **Hashing the configuration file text.** Changes with comments and key order, and misses module and data changes.
- **No fingerprint, record only.** Cannot answer whether two runs are comparable.
- **RFC 8785 exactly.** Stricter number rules, but needs a dedicated implementation (OQ-20).

## Consequences

- Equal fingerprints are a strong, cheap comparability signal.
- Changing log level or output paths leaves the fingerprint unchanged; changing the seed changes it.
- Cross-platform byte equality is not promised.

## References

- [docs/specs/reproducibility.md](../specs/reproducibility.md)
- [RFC 8785 (JSON Canonicalization Scheme)](https://www.rfc-editor.org/rfc/rfc8785)
- [docs/open-questions.md](../open-questions.md)
