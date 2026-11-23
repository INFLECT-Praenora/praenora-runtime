# ADR-0013: Error taxonomy and status codes

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Failures must be attributable to a component and a likely owner (user configuration, module, runtime, protocol or environment), both for people and for scripts. The ABI and the runtime need one shared code space, and the CLI needs stable exit codes.

## Decision

One `int32` code space with stable names. Codes 1-99 are general and ABI-visible, so modules may return them. Codes 100-999 belong to the runtime. Codes at or above 10000 are module-defined and reported verbatim as the `cause`. `status.h` defines 0-99 and the module base (`PRAENORA_STATUS_MODULE_BASE = 10000`); the C++ `praenora::ErrorCode` covers everything. A unit test checks that they agree, and a documentation test checks that every code appears in [docs/specs/error-taxonomy.md](../specs/error-taxonomy.md).

Each code has a name, a category, a likely owner and a CLI exit code, and is marked retryable or not. CLI exit codes: 0 ok, 1 execution, 2 usage, 3 validation, 4 load, 5 timeout, 70 internal, 74 I/O, 130 interrupted or stopped.

| Code | Name | Category | Exit | Likely owner |
|---|---|---|---|---|
| 0 | OK | - | 0 | - |
| 1 | UNKNOWN | internal | 70 | runtime |
| 2 | INVALID_ARGUMENT | usage | 2 | caller |
| 3 | INVALID_STATE | usage | 2 | caller/runtime |
| 4 | NOT_FOUND | environment | 74 | environment |
| 5 | UNSUPPORTED | compatibility | 3 | caller/module |
| 6 | RESOURCE_EXHAUSTED | environment | 74 | environment |
| 7 | CANCELLED | cancelled | 130 | - |
| 8 | TIMEOUT | timeout | 5 | module |
| 9 | IO_ERROR | environment | 74 | environment |
| 10 | INTERNAL | internal | 70 | runtime/module |
| 11 | FAILED | execution | 1 | module |
| 12 | DATA_INVALID | data | 1 | user data/module |
| 100 | PARSE_ERROR | validation | 3 | user |
| 101 | SCHEMA_VIOLATION | validation | 3 | user/module |
| 102 | FORMAT_VERSION_UNSUPPORTED | validation | 3 | user/module |
| 103 | DOCUMENT_KIND_MISMATCH | validation | 3 | user |
| 200 | CONFIG_INVALID | validation | 3 | user |
| 201 | WORKFLOW_NOT_FOUND | validation | 3 | user |
| 202 | BINDING_INVALID | validation | 3 | user |
| 203 | PARAMETERS_INVALID | validation | 3 | user |
| 204 | INPUT_BINDING_INVALID | validation | 3 | user |
| 300 | MANIFEST_INVALID | validation | 3 | module |
| 301 | MODULE_NOT_FOUND | validation | 3 | user/environment |
| 302 | MODULE_IDENTITY_MISMATCH | load | 4 | module |
| 303 | LIBRARY_DIGEST_MISMATCH | load | 4 | environment/module |
| 304 | LIBRARY_NOT_FOUND | validation | 3 | module/environment |
| 400 | ABI_INCOMPATIBLE | validation | 3 | module |
| 401 | PROTOCOL_INCOMPATIBLE | validation | 3 | module |
| 402 | CAPABILITY_NOT_IMPLEMENTED | validation | 3 | user/module |
| 403 | PLATFORM_MISMATCH | validation | 3 | environment |
| 404 | MESSAGE_TYPE_INCOMPATIBLE | validation | 3 | protocol/module |
| 405 | CATALOG_ENTRY_NOT_FOUND | validation | 3 | protocol |
| 406 | CATALOG_ENTRY_DUPLICATE | compatibility | 3 | protocol |
| 500 | LIBRARY_LOAD_FAILED | load | 4 | environment/module |
| 501 | ENTRY_POINT_MISSING | load | 4 | module |
| 502 | API_TABLE_INVALID | load | 4 | module |
| 600 | GRAPH_INVALID | validation | 3 | protocol |
| 601 | GRAPH_CYCLE | validation | 3 | protocol |
| 602 | PORT_UNCONNECTED | validation | 3 | protocol |
| 603 | PORT_UNKNOWN | validation | 3 | protocol |
| 604 | PORT_TYPE_MISMATCH | validation | 3 | protocol |
| 700 | STEP_FAILED | execution | 1 | module |
| 701 | OUTPUT_INVALID | execution | 1 | module |
| 702 | STEP_TIMEOUT | timeout | 5 | module/config |
| 703 | MODULE_UNRESPONSIVE | timeout | 5 | module |
| 704 | INPUT_MISSING | execution | 1 | runtime |
| 705 | RUN_STOPPED | cancelled | 130 | - |
| 800 | TARGET_DATA_INVALID | data | 1 (3 when found during validation) | user data |
| 801 | TARGET_DATA_UNAVAILABLE | data | 1 | environment |

Code 406 `CATALOG_ENTRY_DUPLICATE` is a builder addition (Proposed): two catalog entries with the same id and version (for example an extra catalog directory defining an id and version that the embedded catalog already provides) are rejected with this code. Duplicates are errors and never overrides.

On failure a module writes `{"code": <int>, "message": "<text>", "details": {...}}` to its error sink (plain text is accepted as the message). The host wraps it in a runtime error such as `STEP_FAILED`, keeping the module's code and message as the `cause`. The C++ API throws `praenora::Error` (code, name, category, message, details, location); validation returns a report of all diagnostics instead of throwing on the first.

### Builder notes (Proposed)

- `OK` is not a failure and has no category of its own: `ErrorCodeInfo` reports it as `execution` with exit code 0 (the category enumeration has no "none" enumerator). The table's "-" owner is the string `none` (`OK`, `CANCELLED`, `RUN_STOPPED`).
- Retryable codes (retrying the same operation unchanged may succeed): `RESOURCE_EXHAUSTED`, `TIMEOUT`, `IO_ERROR`, `STEP_TIMEOUT`, `TARGET_DATA_UNAVAILABLE`. All others are not.
- `error_code_info()` returns the row of `UNKNOWN` for a value outside the table; module-defined codes (10000 and up) are causes, not `ErrorCode`s. `to_string(ErrorCategory)` is part of the public header.
- `Error::what()` is the message. `Error::to_json()` writes its keys in the order code, name, category, message, location, details; `location` holds only the parts that are set; details that are not valid JSON are kept as a string; invalid UTF-8 is replaced. `Error` has an out-of-line destructor so its type information is emitted once, in the shared library, and catching it works across the library boundary.
- `ValidationReport::to_json()` is `{"ok":bool,"diagnostics":[{"severity","code","name","category","message","location","step","module"}]}`, with `step` and `module` only when set; `to_text()` ends every line with a newline; `ok()` ignores warnings and notes.
- Two codes are defined but not raised by this runtime version: `MESSAGE_TYPE_INCOMPATIBLE`, because v0 requires exact message type versions and connected ports of different versions are `PORT_TYPE_MISMATCH`, and `TARGET_DATA_UNAVAILABLE`, because input sources read all their data during validation, where a missing or unreadable file is `TARGET_DATA_INVALID` (ADR-0022). `CANCELLED` and `TIMEOUT` occur only as statuses of module calls: the runtime reports a stopped run as `RUN_STOPPED` and a call past its deadline as `STEP_TIMEOUT` or `MODULE_UNRESPONSIVE`.
- Later: [ADR-0031](ADR-0031-raise-the-reserved-error-codes.md) raises both codes. A connection whose ends carry one message type at two versions is `MESSAGE_TYPE_INCOMPATIBLE`, and `PORT_TYPE_MISMATCH` keeps different message types; a data file that does not exist, is not a file or cannot be read is `TARGET_DATA_UNAVAILABLE`, and `TARGET_DATA_INVALID` keeps what an observation CSV holds and its size limit. The likely owner of `MESSAGE_TYPE_INCOMPATIBLE` becomes `protocol`, and the summaries of 404, 604 and 801 say where each code applies.
- An error's location is a `SourceLocation` (file, line, column and JSON pointer). The step and the tick of a run failure travel in its details (`details.step`, `details.tick`; ADR-0011), and a validation diagnostic names its step and module in fields of its own.
- Later: [ADR-0042](ADR-0042-control-and-status-api-over-http.md) adds the component range 9xx, the control API's, with `UNAUTHENTICATED = 900` and `PERMISSION_DENIED = 901` (category `usage`, exit 2, owner the caller, not retryable; runtime codes, not in `status.h`), and `IO_ERROR`'s summary becomes "Reading or writing a file or a connection failed."

## Alternatives considered

- **Per-component error enums.** Easier locally, but scripts and the ABI then need translation tables.
- **Exceptions with free-form messages only.** Not machine-readable and no owner attribution.
- **POSIX `errno` or `std::error_code` categories.** Do not cover module-defined codes or owners.

## Consequences

- Test failures and CLI output point at an owner via the code.
- Codes are part of the documented contract; renumbering needs a breaking change.
- Adding a code needs a header entry, a C++ entry and a taxonomy-document row.

## References

- [docs/specs/error-taxonomy.md](../specs/error-taxonomy.md)
- [docs/specs/module-abi.md](../specs/module-abi.md)
- [docs/testing.md](../testing.md)
