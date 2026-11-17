# ADR-0031: Raise MESSAGE_TYPE_INCOMPATIBLE and TARGET_DATA_UNAVAILABLE

- Status: Proposed
- Date: 2026-11-17
- Supersedes: none
- Superseded by: none

## Context

Two codes of the error taxonomy ([ADR-0013](ADR-0013-error-taxonomy-and-status-codes.md)) were
defined but never raised. The cases they describe were reported with other codes, whose meanings
fit them less well:

- `MESSAGE_TYPE_INCOMPATIBLE` (404, "a message type version differs from the one required"): a
  connection whose ends carry one message type at two versions was `PORT_TYPE_MISMATCH` (604), the
  code of a connection between two different message types. Graph rule 5 checks both
  ([workflow-definitions.md](../specs/workflow-definitions.md#graph-rules)), and the message was the
  only way to tell the two cases apart.
- `TARGET_DATA_UNAVAILABLE` (801, "target data could not be read"; owner environment, retryable): a
  data file that did not exist, was not a file or could not be read was `TARGET_DATA_INVALID` (800,
  "target data is malformed or breaks the observation rules"; owner user data, not retryable), so
  [ADR-0022](ADR-0022-target-data-intake.md) kept 801 for data that cannot be read during a run.
  Input sources read all of their data during validation, so no run ever met that case.

Each code either had to be raised where it applies or marked as reserved for a later version.

## Decision

Both codes are raised now.

- **`MESSAGE_TYPE_INCOMPATIBLE` on a connection.** Graph rule 5 reports a connection whose ends
  carry different message type ids as `PORT_TYPE_MISMATCH`, as before, and one whose ends carry the
  same id at different versions as `MESSAGE_TYPE_INCOMPATIBLE`, located at `/connections/<i>` of the
  workflow definition: "connection inputs.source -> a.in carries test.blob@0.2.0 into a port that
  requires test.blob@0.1.0; both ends of a connection carry the same version of a message type".
  `PORT_TYPE_MISMATCH` now means only different types. The code keeps its category (validation,
  exit 3), and its likely owner becomes `protocol`, like that of every other code graph
  validation raises: the definition's inputs, outputs and step capabilities disagree on a version,
  and the catalog's authors fix that. Modules take no part in it in v0.
- **`TARGET_DATA_UNAVAILABLE` for a data file that cannot be read.** Validation reports a workflow
  input's data file that does not exist, is not a file or whose status cannot be read (a directory
  on its path cannot be searched, for example) as `TARGET_DATA_UNAVAILABLE`, located at
  `/inputs/<name>/path` of the configuration, and both kinds of source (`observation-csv` and
  `json-document`) report a file they cannot open or read as `TARGET_DATA_UNAVAILABLE`, located at
  the file. `TARGET_DATA_INVALID` keeps what the file holds: CSV structure, observation rows, a
  missing column, and the observation CSV's size limit. A `json-document` beyond the documents' 16
  MiB stays `PARSE_ERROR`, like every document.
- **Exit codes do not change.** Both codes are found by validation, whose failures exit with 3
  whatever their codes (with 74 only when every error is `IO_ERROR`), so `praenora validate` and
  `praenora run` still exit with 3 for these cases. The categories stay as they are: outside a
  validation report, 801 would exit with 1 (`data`), but nothing raises it there.
- The table's summaries say so: 404 "A message type version differs from the one required, such as
  at the two ends of a connection.", 604 "A connection joins ports of different message types.",
  801 "Target data could not be read, for example because its file does not exist."

### Builder notes (Proposed)

- The two cases of rule 5 exclude each other: a connection between different ids is
  `PORT_TYPE_MISMATCH` whatever its versions, so one connection never yields both codes.
- When the policy for evolving message types is decided (OQ-7 in
  [open-questions.md](../open-questions.md)) and a port may accept more than one version, the
  versions it does not accept stay `MESSAGE_TYPE_INCOMPATIBLE`; the code's meaning does not
  change. A source that reads data during a run (OQ-8) would report data it cannot read with
  `TARGET_DATA_UNAVAILABLE` too.
- The CLI's help of `validate` and the comment of `exit_code_of()` name `TARGET_DATA_UNAVAILABLE`
  for a missing or unreadable data file.
- Validation classifies the data file by its status (`std::filesystem::status()`): a file that does
  not exist "does not exist", one whose status cannot be read "cannot be read: <the system's
  reason>", anything but a regular file "is not a file". Before, a status that could not be read
  was reported as a file that does not exist.

## Alternatives considered

- **Mark both codes reserved.** Keeps the behavior, but leaves 800 reporting files that cannot be
  read with a meaning, owner and retryable flag that describe their content, and leaves a version
  mismatch distinguishable from a type mismatch only by its message.
- **Remove the codes.** Both cases exist today, and the codes name them better than the codes
  that reported them.
- **`IO_ERROR` for a data file that cannot be read.** It would exit with 74 when it is the only
  error, like a missing configuration, but it would lose the data category and the input's
  location at its `path`, and a target-data code exists for exactly this.
- **Keep a missing data file `TARGET_DATA_INVALID` and raise 801 only for files that exist but
  cannot be read.** It splits one situation, a path that leads to no readable data, over two codes,
  and keeps a meaning ("malformed") that does not describe a missing file.

## Consequences

- A diagnostic's code tells a type mismatch from a version mismatch, and a data file's content from
  its absence, without reading the message; scripts that matched `PORT_TYPE_MISMATCH` or
  `TARGET_DATA_INVALID` for these cases see the new codes. Nothing outside this repository uses the
  runtime yet.
- `TARGET_DATA_UNAVAILABLE` is retryable: a file that is being written by another process may
  exist when the validation is repeated.
- Every code of the taxonomy is now raised by the runtime except `UNKNOWN`, `CANCELLED` and
  `TIMEOUT`, which modules return as call statuses (`UNKNOWN` is also the front ends' fallback for
  a name they do not know); `OK` reports no failure.

## References

- [docs/specs/error-taxonomy.md](../specs/error-taxonomy.md)
- [docs/specs/workflow-definitions.md](../specs/workflow-definitions.md#graph-rules)
- [docs/configuration.md](../configuration.md#observation-csv)
- [ADR-0013](ADR-0013-error-taxonomy-and-status-codes.md),
  [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md),
  [ADR-0022](ADR-0022-target-data-intake.md), [ADR-0023](ADR-0023-public-cpp-api.md)
