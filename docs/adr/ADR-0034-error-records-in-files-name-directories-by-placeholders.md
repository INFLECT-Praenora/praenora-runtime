# ADR-0034: Error records in the files of a run name known directories by placeholders

- Status: Proposed
- Date: 2026-11-17
- Supersedes: none
- Superseded by: none

## Context

The run record names no host and no user, and it says where each manifest was found without an
absolute path ([ADR-0029](ADR-0029-run-record-says-where-manifests-were-found.md)). ADR-0029 left
one exception open: the error records of a failed run (`execution.error` and the error records of
`final_status` in the run record, and those of the status file) could still name files by their
absolute paths, which usually hold the user's home directory and so the user's name. Since
[ADR-0028](ADR-0028-error-records-keep-location-and-details.md) they hold the error's location
and details too. Paths reach them in four ways:

- the runtime's own messages and locations: the output file or directory that could not be
  written, the manifest of a module or provider that could not be loaded, the configuration file;
- text of the operating system inside a message: `dlerror()` names the library it could not open,
  and on macOS 13 and later dyld lists every path it tried for it;
- details: those of a provider conflict name the manifest and library loaded before
  (`loaded.manifest`, `loaded.library`);
- a module's own error document (`cause`), whose text the runtime does not control.

The run record and the status file are written for others to read: a run record travels with the
outputs, and a status file is read by other processes. What stays in the process serves the user
on the machine where the run happens, who needs the absolute paths to find the files.

## Decision

- **Split by audience.** In the run record (`run-record.json`, and `Workflow::run_record_json()`
  and the Python package's `Workflow.run_record()`, which return the same document) and in the
  status file, the error records name every directory the run knows by a placeholder. What stays in
  the process keeps absolute paths: the thrown `praenora::Error`, `RunSummary`,
  `Workflow::status()` and `status_json()`, `StatusSnapshot::to_json()`, the Python package's
  `Workflow.status()`, the CLI's error output and the logs. How errors are made, their messages
  and validation reports do not change.
- **The directories and their placeholders**, fixed when the workflow is opened:
  - `<output>`: the run's output directory;
  - `<configuration>`: the directory of the configuration file;
  - `<modules:ORIGIN:INDEX>` and `<modules:installation>`: each module search path that the run's
    validation used, named as `execution.manifests` names a search path (ADR-0029): ORIGIN is
    `caller`, `configuration`, `environment` or `installation`, INDEX the path's position among the
    non-empty entries of its origin, and the installation's directory has none;
  - `<providers:ORIGIN:INDEX>` and `<providers:installation>`: each provider search path;
  - `<home>`: the user's home directory, the catch-all for free text: `HOME` on POSIX systems,
    the password database's entry (`getpwuid_r()`) when `HOME` is unset or empty, and
    `USERPROFILE` on Windows.

  A replaced path keeps the rest of it: `<output>/run-record.json`,
  `<modules:caller:0>/fx-echo/fx-echo.pmm.json`, `<home>/data/x.csv`.
- **Matching.**
  - Each directory is an anchor in its lexical, normalized form, without a trailing separator,
    and, when it differs, in its form with symbolic links resolved (`weakly_canonical()`); both
    give its placeholder (on macOS `/var/folders/...` and `/private/var/folders/...`). A root
    directory (`/`, `C:\`) and an empty or relative path are never anchors, so a home directory
    that is unset, empty, relative or a root is skipped.
  - An anchor matches only at a whole-path boundary: it is preceded by the start of the string or
    by a character that does not continue a path segment, and followed by the end of the string, a
    separator, a character that does not continue a path segment, or a run of `.` that is followed
    by the end of the string or by a character that neither continues a segment nor is a
    separator, as at the end of a sentence (`... not found in /Users/<user>.` gives
    `... not found in <home>.`). ASCII letters and digits, `.`, `_`, `-`, `~`, bytes outside ASCII
    (UTF-8) and separators continue a segment; every other character does not, among them quotes,
    whitespace, `(`, `)`, `,`, `:`, `;`, `=`, `[` and `]`. So `/out` matches none of `/out2`,
    `/out-old`, `/out.d`, `/out./x`, `/out...x` and `/a/out`.
  - On macOS, an anchor that directly follows `/System/Volumes/Preboot/Cryptexes/<one segment>`,
    that prefix itself beginning at a boundary, matches as well, and the prefix is kept (see the
    builder notes).
  - The longest anchor wins: an output directory in the home directory gives `<output>`. Of
    anchors of the same directory, the first in this order wins: output, configuration, the module
    search paths in search order, the provider search paths in search order, home.
  - A string is scanned from left to right; replaced text is never scanned again.
  - On Windows `/` and `\` are both separators, either matching the other, and letters are
    compared without regard to ASCII case (not yet verified).
- **Where.** The parts of an error record that can name files: `message`, `location.file`, and
  every string of `details` and of `cause`, however deeply nested; never an object key, never a
  value that is not a string, and never the record's `name`, `code`, `tick`, `at` or
  `location.pointer`, a JSON pointer that can read like a path under a directory at the root
  (`/library/path` under a search path `/library`). In the run record: `execution.error`, and the
  `last_error` of the workflow and of every step of `final_status`. In the status file: the same
  `last_error`s. The replacement works on parsed JSON values, never on written text. Nothing else
  in the run record changes: the `reproducibility` section and the fingerprint,
  `execution.manifests`, `outputs` and `trace` stay as they are.
- **No format change.** The `run-record` 0.2 and `status-snapshot` 0.1 schemas accept any string
  in an error record; only their descriptions change, to say what the strings hold. No version
  changes: versions are not bumped for now ([AGENTS.md](../../AGENTS.md#change-rules)).

### Builder notes (Proposed)

- dyld (macOS 13 and later) lists every path it tried for a library it could not open, and lists
  the library's absolute path a second time behind the OS cryptex's prefix, with no separator of
  its own: `'/System/Volumes/Preboot/Cryptexes/OS/Users/<user>/m/lib.dylib' (no such file)`. The
  boundary rule alone refuses that occurrence (it follows the letter `S`), so the user's name would
  stay in the record of a library that could not be opened, the headline case of this decision. On
  macOS an anchor therefore also matches right after `/System/Volumes/Preboot/Cryptexes/` and one
  segment, any segment rather than only `OS`; the prefix must itself begin at a boundary, and
  exactly one non-empty segment must lie between it and the anchor. The prefix is kept on purpose,
  so the message still says what dyld tried:
  `'/System/Volumes/Preboot/Cryptexes/OS<modules:caller:0>/fx/lib.dylib' (no such file)`. Any
  segment adds no false positive, since an anchor must still follow the prefix. dyld also lists
  the paths with their symbolic links resolved (`/private/tmp/...` for `/tmp/...`), which the
  resolved form of each anchor covers.
- The anchors are the internal type `detail::PathAnchors`
  (`src/runtime/observability/path_anchors.hpp`), built from `detail::RunDirectories` once per
  workflow, in the constructor of `Workflow::Impl`, and immutable afterwards. One copy, held by
  `std::shared_ptr<const PathAnchors>`, goes to the engine (`EngineSettings::path_anchors`) and on
  to `RunRecordWriter`, and to the status-file writer's producer, which runs on a background
  thread. A `RunRecordWriter` without anchors (the default) writes absolute paths as before.
- `ModuleIndex` and `ProviderIndex` keep the search paths they scanned with their origins and
  indexes (`scanned_search_paths()`). Validation records the module search list it used in
  `ValidatedWorkflow::module_search_paths`: the context index's, or the list rebuilt with the
  configuration's `modules.search_paths` when it names any. The provider search paths are those of
  the runtime's provider index, all of them, whether or not the run uses a provider.
- The anchors are applied to JSON values. `RunRecordWriter::write_final()` applies them to the
  record's `execution.error` and to the parsed final status snapshot before it keeps the document,
  so `current()` (and so `run_record_json()`) and the file are the same document. The status-file
  producer builds the snapshot's JSON with `detail::status_snapshot_json()`, the function that
  `StatusSnapshot::to_json()` also writes, applies them, and writes the result. The `ErrorRecord`s
  of the metrics and of the run summary are never changed.
- The home directory is read when the workflow is opened (`detail::home_directory()`,
  `src/runtime/platform/`). The password database is consulted only when `HOME` is unset or empty;
  a `HOME` set to a relative path or a root is taken as it is, and so skipped.
- The configuration's directory is that of the configuration file as validation read it: absolute
  and lexically normal, the directory its relative paths resolve against.
- Other directories, such as catalog paths, the directory of a log file or of the status file, and
  data files outside the known directories, are not anchors; under the home directory they are
  named through `<home>`.
- The end-to-end tests provoke the three failures that put paths from outside the runtime's own
  text into a record: a module library that cannot be opened (a text file, so `dlerror()` names
  it), an output directory that cannot be written (a file where `outputs/` goes), and a provider
  conflict, reached through the public API: a first run loads the fixture provider
  `test-conflict` from one copy, and a second runtime, whose provider search paths put another copy
  first, fails to load that copy. Each checks that the run record, `run_record_json()` and the
  status file hold none of the run's directories, as given or resolved, and no `<home>` (the home
  directory holds every directory of these runs, so a missed specific directory would show as
  `<home>`), while `Workflow::status()` still names them. A fourth test reaches `<configuration>`:
  a status file in the configuration's directory that cannot be written becomes the workflow's
  last error, which the final status of the run record holds. The conflict test is skipped in a
  process that loaded `test-conflict` before (see the comment at the test).

## Alternatives considered

- **Keep absolute paths.** The record of a failed run would keep naming the user wherever it goes,
  which ADR-0029 avoided everywhere else in the record.
- **Relativize the structured parts only.** Making `location.file` and the paths in `details`
  relative, and rewording the runtime's messages, misses the text of the operating system
  (`dlerror()`) and of modules (`cause`), and changes how errors are made.
- **An opt-in flag** (a `WorkflowOptions` field, a CLI flag, a Python argument). It needs all three
  front ends, and the default would still name the user.
- **`~` for the home directory only.** It hides the user's name only under the home directory, not
  in `/scratch/<user>` or in a directory given by another variable, and it does not say which of
  the run's directories a path lies in.

## Consequences

- A reader of a run record or a status file sees where a failure was found relative to the run's
  directories, and resolves a placeholder against the run's settings (its output directory,
  configuration, search paths and home directory). On the machine itself, `Workflow::status()`,
  the logs and the CLI's error output give the absolute paths.
- This is best effort: a path under no anchor stays as it is written, for example an absolute data
  path under `/scratch/<user>`, a user name that a module writes into its error document, or a path
  in a form that matches no anchor (another spelling of a directory on a case-insensitive file
  system outside Windows).
- The boundary rules have two edges. A character that does not continue a segment ends a path, so
  a directory whose name extends an anchor's last segment by such a character and more
  (`/runs/out (old)` against the anchor `/runs/out`) is taken for the anchor: `<output> (old)`.
  And dots at the end of a text, or before a character that ends a path (a quote, a space, a
  parenthesis), are taken for punctuation, so a directory literally named like an anchor followed
  by dots (`/runs/out.`) is taken for the anchor there: `<output>.`.
- The status file and `Workflow::status_json()` differ in their error records, and `praenora status`
  (which reads the status file) shows placeholders.
- The question that ADR-0029 left open is answered; its decision on `execution.manifests` stands.

## References

- [observability.md](../specs/observability.md#known-directories-in-the-files-of-a-run): the
  placeholders and the matching rules.
- [reproducibility.md](../specs/reproducibility.md#run-record): the run record's error records.
- [ADR-0015](ADR-0015-observability.md): status snapshots and the status file.
- [ADR-0016](ADR-0016-reproducibility.md): the run record.
- [ADR-0028](ADR-0028-error-records-keep-location-and-details.md): error records keep the location
  and details of their error.
- [ADR-0029](ADR-0029-run-record-says-where-manifests-were-found.md): where each manifest was
  found, and the search paths' origins and indexes.
- [platforms.md](../platforms.md): the verified platforms (macOS arm64; Linux and Windows not yet).
