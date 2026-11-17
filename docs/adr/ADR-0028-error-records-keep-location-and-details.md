# ADR-0028: Error records keep the location and details of their error

- Status: Proposed
- Date: 2026-11-16
- Supersedes: none
- Superseded by: none

## Context

A failed run carries its error in three places besides the `praenora::Error` that the control call
throws: the run summary (`RunSummary::error`), the status snapshot (`last_error` of the workflow
and of the failing step) and the run record (`execution.error`). Each holds an `ErrorRecord`, which
had the error's code, message and tick, the time it was recorded and the module's error document
(`cause`), but neither the location nor the details that the thrown error has. A reader of a status
file or a run record could therefore not see where a failure was found (the JSON pointer of an
output that broke its message type, the file of an output that could not be written) nor the
details it was thrown with (step, module, phase, tick, diagnostics).

The gap shows most in the engine's last-resort handler. When an exception that is not a
`praenora::Error` (out of memory, in practice) breaks off the ending of a run, the run's error is
the first failure, and the run records the very error that the control call throws
([ADR-0011](ADR-0011-tick-based-synchronous-execution.md)); but the record it wrote kept only that
error's code and message. `Workflow::run()` on such a run, which has no kept error to throw again,
rebuilds the error from the record and so lost the location and details as well
([ADR-0023](ADR-0023-public-cpp-api.md)).

## Decision

- `ErrorRecord` (`include/praenora/runtime/status.hpp`) gains `SourceLocation location` and
  `std::string details_json`, the error's location and details (`Error::location()` and
  `Error::details_json()`, `"{}"` when it has none). They are appended after the existing members,
  so code that sets the members by name, or the first ones by position, still compiles.
- Every record the runtime makes of a `praenora::Error` keeps the error's location and details.
  The record of a run failure (`Engine::fail()`) holds the details the error is thrown with: the
  error's own, completed with `step`, `module`, `phase`, `tick`, `cause` and `diagnostics`; the
  failing step's last error is the same record. The other records come from one internal helper,
  `error_record_of()` (`src/runtime/observability/status_json.hpp`): the record that the engine's
  last-resort handler makes of the error it returns, the records of an output file that could not
  be completed, of a finalize that failed and of a status-file write that failed. A record that
  does not come from an error, such as `RUN_STOPPED`, has neither (both are null in JSON).
- `Workflow::run()` on a failed run that kept no error (the call that ended the run threw
  something other than a `praenora::Error`) throws the error rebuilt from the run's record, with
  its location and details: the failure that the run recorded.
- In JSON an error record gains `location`, the parts of the location that are set (`file`,
  `line`, `column`, `pointer`; a column only with a line, as `Error::to_json()` writes them), or
  null when none is, and `details`, the details object, or null when the error has none (empty or
  `{}`); text that is not a JSON object is kept as `{"text": ...}`, as for `cause`. Both keys are
  always written, as `tick`, `at` and `cause` are, and the schemas require them (and a `details`
  object with at least one member).
- No version changes. Versions are not bumped for now ([AGENTS.md](../../AGENTS.md#change-rules)):
  the current formats change in place, `status-snapshot` 0.1 and `run-record` 0.2 (the format the
  runtime writes; the `run-record` 0.1 schema, for records written before 0.2, is unchanged), and
  the runtime package stays 0.2.0. A snapshot or record written before this change, without the two
  keys, no longer satisfies its schema; none is in use.

### Builder notes (Proposed)

- Appending members is source-compatible, but `StatusSnapshot`, `StepStatus` and `RunSummary` hold
  `ErrorRecord` by value, so their layout changes under the same runtime version and `SOVERSION`
  0.2: builds from before this change are not binary-compatible with it. Once versions are bumped
  again, such a change needs a new minor version
  ([ADR-0014](ADR-0014-versioning-and-compatibility-policy.md)).
- A record's `details_json` is the error's details text unchanged, so the details of a run
  failure repeat the module's error document (`details.cause` equals `cause`).
- One helper, `location_parts()` (`src/runtime/core/location_json.hpp`), turns a location into
  JSON for both `Error::to_json()` (keys in order, `{}` when no part is set) and the error record
  (keys sorted, null when no part is set), so the two cannot drift apart.
- `praenora status` prints the same text as before (code, message and tick of each error); its
  `--json` output, `Workflow::status()` and the Python package show `location` and `details`.
- The last-resort handler cannot be reached in a test without a fault-injection seam, which the
  runtime does not have. It builds its record with `error_record_of()`, whose output the unit
  tests check; the records of run failures are checked end to end, in the summary, the status and
  the run record.
- In `Engine::fail()` the details are complete before the failing step's metrics take the
  record. Without memory for serializing them, the record goes without details and the metrics
  are still updated; the step is marked failed even when describing the failure runs out of memory
  altogether, so it shows `failed` either way.

## Alternatives considered

- Keep the members in the C++ record only, with no change to the documents. A reader of a status
  file or a run record, often another process or a later one, would still not see where a failure
  was found, which is most of what the issue is about.
- Fill in only the last-resort record. The fields would exist on every record but be empty on all
  the others, although each comes from an error that has a location and details.
- Put the location into the record's message. It would lose its structure and differ from how
  `praenora::Error` and validation diagnostics report a location.
- New format versions, `status-snapshot` 0.2 and `run-record` 0.3, each with a schema file of its
  own, as [compatibility-policy.md](../specs/compatibility-policy.md#document-formats) asks for a
  change to what a schema accepts. Nothing outside this repository reads these formats yet, and
  the maintainers have decided not to bump versions for now, so the current formats change in
  place.
- Optional `location` and `details` in the current formats. Documents written before the change
  would still pass, but no such documents are in use, and required keys tell readers that the
  runtime always writes them.
- Keep the details without `cause`, which the record already holds. The record would no longer be
  the error that was thrown, and `run()` could not rebuild that error from it.
- Omit `location` and `details` when they are unset. The error record writes every other optional
  part as null, so the keys stay predictable for readers.

## Consequences

- Status snapshots and run records say where a failure was found and carry its details, so a tool
  that reads them no longer needs the exception or the log for that.
- The records of run failures grow. A status snapshot holds the failure twice (the workflow's and
  the failing step's last error), and a run record three times (`execution.error` and both in
  `final_status`); with the details, a module's error document (bounded to 64 KiB) appears twice
  in each of these records instead of once, and the diagnostics (bounded to 64 KiB) once.
- A status snapshot or run record written before this change no longer satisfies its schema.
- The text output of `praenora status` does not show locations or details; its `--json` output
  does.

## References

- [observability.md](../specs/observability.md#status-snapshot): the error record.
- [reproducibility.md](../specs/reproducibility.md#run-record): the run record's `execution.error`.
- [error-taxonomy.md](../specs/error-taxonomy.md#how-errors-are-reported): how run failures are
  reported.
- [compatibility-policy.md](../specs/compatibility-policy.md): format versions and their schemas.
- [ADR-0013](ADR-0013-error-taxonomy-and-status-codes.md): errors, their location and details.
- [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md): versions are not bumped for now;
  the versioning rules for when they are again.
- [ADR-0015](ADR-0015-observability.md): status snapshots.
- [ADR-0016](ADR-0016-reproducibility.md): run records.
- [ADR-0023](ADR-0023-public-cpp-api.md): `Workflow::run()` on a failed run.
