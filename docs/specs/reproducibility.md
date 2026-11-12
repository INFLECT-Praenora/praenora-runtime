# Reproducibility

- **Version:** runtime 0.1.0; document format `run-record` 0.1.0
- **Status:** Normative, MVP. Proposed ([ADR-0016](../adr/ADR-0016-reproducibility.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The runtime removes every source of nondeterminism it controls and records everything that
determines a run's results. Each run has a fingerprint, the hash of what determines its results, and
a run record. This document specifies both, the seeds, the output files, and the determinism rules
for the runtime and for modules.

## Run fingerprint

```text
fingerprint = lowercase hex of SHA-256(canonical_json(reproducibility section))
```

The reproducibility section is computed during validation, before any module is loaded. Two runs
with the same fingerprint used the same runtime, ABI and protocol versions, the same catalog
entries, the same semantic configuration, the same module manifests and library files, and the same
input data. The first 16 hex digits of the fingerprint prefix every envelope id.

| Key | Content |
|---|---|
| `versions` | `{runtime, abi {major, minor}, pmp}`. |
| `workflow` | `{name, version, digest}` of the workflow definition. |
| `catalog` | `{"<id>@<version>": digest}` for the capability of every step and every message type that the workflow's inputs and outputs and those capabilities' ports use. When a capability and a message type used by one workflow share an `id@version`, both keys take the form `<kind>:<id>@<version>`. |
| `time` | `{unit, start, period}`: the time base. |
| `seed` | The run seed. |
| `ticks` | The tick count, after any override, or `null` when the run lasts until its replay inputs are exhausted. |
| `timeouts` | The run's `{invoke, lifecycle, cancel_grace}`. |
| `inputs` | `{<input>: {source, digest, columns?}}`: the source kind, the SHA-256 of the input file's bytes, and the column mapping when there is one. |
| `steps` | `{<step>: {capability {id, version}, module {name, version, manifest_sha256, library_sha256, sdk_version}, parameters, seed, timeouts}}`, where `parameters` are the resolved parameters, `seed` the step seed and `timeouts` the step's effective timeouts. |

A digest is the SHA-256 of a catalog entry's canonical JSON, of a file's bytes (manifests, libraries,
input files). `sdk_version` is the one the manifest declares. Durations are written canonically (see
[time-cancellation-timeouts.md](time-cancellation-timeouts.md#durations-in-the-configuration)).

What changes the fingerprint, and what does not:

- It changes with the seed, the tick count, any timeout, a parameter value, a module's manifest or
  library file, an input file's bytes (re-encoding a file counts), a catalog entry the workflow uses,
  and the runtime, ABI or PMP version.
- It does not change with paths (of the configuration, the input files, the manifests and the
  libraries), with the `outputs` and `observability` settings, with step log levels, or with the
  wall-clock time: the same configuration and data at other paths give the same fingerprint.
- Parameters are hashed after the defaults of the module's parameter schema are filled in, so an
  omitted parameter and an explicit default give the same fingerprint. Numbers keep their JSON type:
  `100` and `100.0` hash differently ([open-questions.md](../open-questions.md), OQ-20).

## Canonical JSON

Canonical JSON is the serialization of nlohmann/json's `dump()` without indentation on objects whose
keys are sorted:

- object keys sorted by their UTF-8 bytes, no insignificant whitespace;
- strings in UTF-8; a string that is not valid UTF-8 is refused (`DATA_INVALID`), never replaced;
- integers as integers; floating-point numbers in the shortest form that reads back to the same
  value, independent of the locale, with `.0` for whole values (`1.0`, `0.54`, `1e+20`).

The form is not claimed to be RFC 8785 (OQ-20). Another implementation that computes fingerprints
MUST produce the same bytes. Canonical JSON is used for the fingerprint, the catalog digests, the
documents passed to modules, the recorded outputs and the message trace.

## Seeds

- The run seed is the configuration's `run.seed`, an integer from 0 to 9007199254740991 (default 0).
  The caller MAY override it (`praenora run --seed`, `WorkflowOptions::seed`); the override is
  recorded.
- Each step gets its own seed:

  ```text
  step_seed = first 8 bytes, big-endian, of SHA-256("praenora-seed-v1" || u64be(run_seed) || utf8(step_id))
              AND (2^53 - 1)
  ```

  For example, run seed 20261110 and step `estimate` give 4059344203658876.
- The step seed reaches the module in the initialize document (`run.seed`) and is recorded in
  `steps.<step>.seed`. A module MUST draw all of its randomness from it.

## Run record

A run record (`kind: run-record`, [`run-record.0.1.schema.json`](../../spec/schemas/run-record.0.1.schema.json))
describes one run: `{kind, format_version, fingerprint, reproducibility, execution}`. The
`reproducibility` section is the one hashed; the `execution` section holds facts about the run:

| Field | Content |
|---|---|
| `status` | `running` from the start, then `completed`, `stopped` or `failed`. |
| `started_at`, `finished_at` | Wall-clock instants (`finished_at` is null until the end). |
| `ticks_completed` | The ticks completed. |
| `error` | The failure of a failed run, a `RUN_STOPPED` record for a stopped one, null otherwise. |
| `platform` | `{os, arch, compiler, build_type}` of the runtime build. |
| `overrides` | The run settings the caller overrode (`seed`, `ticks`); `{}` when none. |
| `validation` | `strict`, or `off` for a performance measurement. |
| `outputs` | `{<output>: {file, sha256, messages}}` for every recorded output, written at the end. |
| `trace` | `{file, sha256, messages}` of the message trace, written at the end when tracing is on. |
| `final_status` | The final status snapshot ([observability.md](observability.md#status-snapshot)), written at the end. |

- The record is written when the run starts, before any module is loaded, and again when it ends,
  each time replacing the file atomically, as JSON indented by two spaces with a final newline.
  Without an output directory it is kept in memory only (`Workflow::run_record_json()`).
- The record names no host and no user. It identifies each module by name, version and the digests
  of its manifest and library, not by path.
- A writer MUST refuse a fingerprint that is not the hash of the record's reproducibility section.

## Output files

A run writes files only when it has an output directory (`outputs.directory`, `praenora run
--output-dir`, `WorkflowOptions::output_dir`):

```text
<output directory>/
  run-record.json          the run record
  outputs/<output>.jsonl   one envelope per line, for each recorded workflow output
  trace/messages.jsonl     the message trace, when tracing is on
  status.json              the status file, when one is configured at this path
```

- Each line of an output file and of the trace is the canonical JSON of one record followed by
  `\n`, written in binary mode, so the files are byte-deterministic. Every recorded output has a
  file, even one that receives no message.
- A directory that already holds `run-record.json` holds an earlier run. The runtime refuses it
  (`IO_ERROR`) unless the caller asks for an overwrite (`praenora run --overwrite`,
  `WorkflowOptions::overwrite`), which removes the files Praenora manages there, and only those:
  `outputs/`, `trace/`, `status.json` and, last, `run-record.json`. Everything else stays; log files
  are appended to.
- An output file or a final run record that cannot be written turns a completed or stopped run into
  a failed one (`IO_ERROR`).

## Determinism

The runtime guarantees:

- a deterministic schedule (topological order, ties broken by step id), deterministic routing and
  exactly one module call at a time;
- deterministic envelope ids (`<fp16>:<tick>:<source>`) and logical times;
- ordered C++ containers wherever an order reaches an output, and canonical JSON for everything it
  records;
- no wall-clock time in envelopes, recorded outputs, the trace or the reproducibility section;
- numbers in input data read the same under any locale whose decimal separator is a single byte
  ([ADR-0022](../adr/ADR-0022-target-data-intake.md));
- seeds derived only from the run seed.

A module MUST:

- not depend on the wall clock;
- draw its randomness only from the seed of its initialize document;
- iterate in a deterministic order wherever the order reaches a result;
- have no data races and not read uninitialized memory;
- not let results depend on state shared with other instances
  ([threading-and-ownership.md](threading-and-ownership.md#what-a-module-must-do)).

The contract checks' `invoke.determinism` check runs every test-vector case twice, on new instances,
and requires byte-identical results ([ADR-0024](../adr/ADR-0024-contract-checks-and-test-vectors.md)).

**Guarantee.** The same fingerprint on the same platform with the same runtime build gives
byte-identical outputs and traces. Across platforms and compilers, results agree only within a
numeric tolerance, because floating-point contraction and the math library differ. The example
modules are compiled with `-ffp-contract=off` on GCC and Clang to narrow that gap, and the example's
golden outputs are compared with a relative tolerance of 1e-9 and an absolute one of 1e-12.

## References

- [ADR-0016](../adr/ADR-0016-reproducibility.md): reproducibility.
- [ADR-0020](../adr/ADR-0020-message-envelope-and-asyncapi.md): envelope ids, recorded outputs and the trace.
- [ADR-0022](../adr/ADR-0022-target-data-intake.md): input data and its digests.
- [RFC 8785 (JSON Canonicalization Scheme)](https://www.rfc-editor.org/rfc/rfc8785).
- [user-guide.md](../user-guide.md#what-a-run-writes): the files of a run.
