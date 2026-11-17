# ADR-0029: The run record says where each manifest was found, without an absolute path

- Status: Proposed
- Date: 2026-11-16
- Supersedes: none
- Superseded by: none

## Context

The run record identifies each step's module, and each provider of the run's services, by name,
version and the digests of its manifest and library ([ADR-0016](ADR-0016-reproducibility.md),
[ADR-0026](ADR-0026-runtime-service-providers.md)). The digests say which bytes a run used, not
where they were. When several copies of a module or provider are installed, on several search paths
or shadowing one another, a reader of a record cannot tell which file the run used without
repeating discovery on the machine where it ran.

The obvious record, the manifest's absolute path, usually contains the user's home directory and so
the user's name, which a run record does not hold
([reproducibility.md](../specs/reproducibility.md#run-record)). Paths must also stay out of the
fingerprint: the same configuration and data at other paths give the same fingerprint.

## Decision

- The run record's `execution` section gains `manifests`: `steps`, the location of each step's
  module manifest by step, and `services`, the location of the provider manifest of each service of
  the run by `"<name>@<major>"`, the keys of the reproducibility section's `steps` and `services`.
- A location is `{"search_path": {"origin", "index"}, "file"}`, which names no absolute path:
  - `search_path` is the search path that reached the manifest first, named by where it comes
    from, never by its value. `origin` is `caller` (the caller's paths, from the C++ API, the
    tool's options or the Python package), `configuration` (the configuration's
    `modules.search_paths`), `environment` (`PRAENORA_MODULE_PATH`, `PRAENORA_PROVIDER_PATH`) or
    `installation` (the installation's module or provider directory). `index` is the path's
    position among the non-empty entries of its origin as given, from 0, an entry that repeats an
    earlier path included; the installation's directory has none.
  - `file` is the manifest file relative to that search path, `/`-separated on every platform.
    Discovery reads a search path's own manifests and those of its immediate subdirectories, so
    `file` is `<file>` or `<directory>/<file>`. It holds a user name only when a search path is
    itself the parent of home directories (`/home`, `/Users`), so that `<directory>` is one.
- The locations are in `execution`, not in the hashed `reproducibility` section: they never change
  the fingerprint, and the reproducibility section still holds no path.
- The library is not located: `library.path` is relative to the manifest's directory and is part of
  the manifest's bytes, so the manifest's location and digest determine the library's location.
- The record says, more precisely than before, what it does not hold: no host, no user, and no
  absolute path written by the runtime except in two places. An error record (`execution.error`
  and the error records of `final_status`) can still name a file by its absolute path, in its
  `location`, `message`, `details` and `cause`; and the values copied from the configuration and
  from manifests (parameters and their defaults, a provider's `engine` object) hold whatever those
  documents hold, paths included.
- The `run-record` 0.2 schema changes in place: `execution.manifests` is required, and its
  `file` pattern refuses a `file` that begins like an absolute path (`/`, `\` or a drive such as
  `C:/` or `C:\`), one with more than one `/`, one with an empty segment, and one with a `.` or
  `..` segment. A backslash elsewhere is accepted, because it is a legal character of a file name
  on POSIX systems; the runtime writes `/` on every platform. The price is that a POSIX search path
  holding a directory named exactly `<letter>:`, or a name that begins with `\`, gives a record
  that the runtime writes but its schema refuses (the runtime does not validate its own record): a
  pattern cannot tell these names from Windows absolute paths. No version changes: versions are not bumped for now
  ([AGENTS.md](../../AGENTS.md#change-rules)). A record written before this change no longer
  satisfies the schema; none is in use.

### Builder notes (Proposed)

- `module_search_paths()` and `provider_search_paths()` return `SearchPath`s (path, origin, index;
  `src/runtime/discovery/search_paths.hpp`); `ModuleIndex::scan()` and `ProviderIndex::scan()`
  give each manifest they index its `location` (`ManifestLocation`). A manifest read directly
  (`load_manifest()`, `load_provider_manifest()`, as `praenora modules check` does) has none. A
  scan of plain paths treats them as the caller's, each at its position.
- Validation builds `ValidatedWorkflow::manifest_locations` beside the reproducibility section, and
  `RunRecordWriter` writes it into `execution.manifests` from the first record on, so a run that
  fails while loading still says which files it was loading.
- The index counts non-empty entries because empty ones were never search paths: an empty caller
  path and an empty entry of an environment variable are skipped, as before.
- `file` is computed lexically from the normalized search path and the normalized file, so a
  trailing separator on a caller's path changes nothing; symbolic links are not resolved.
- The schema's `index` rule (required unless the origin is `installation`, refused for it) is the
  first `if`/`then`/`else` in `spec/schemas/`; the catalog's message types
  `pmp.prediction-set` and `pmp.observation-batch` already use one, with the same validator.
- What a location does not give is the search path's value: a reader resolves `origin` and
  `index` against the run's caller paths, configuration and environment. On the machine itself,
  `praenora modules inspect` and `praenora providers list` show absolute paths.

## Alternatives considered

- The path relative to the configuration file's directory. Across Windows drives no relative path
  exists, the `../` chains are long, and the path can still hold the home directory (a
  configuration under `/tmp` and modules under the home directory give `../Users/<user>/...`).
- The absolute path with the home directory replaced by `~`. It hides the user name in one place
  only (not in `/scratch/<user>`, nor in `C:\Users\<user>` when `HOME` is not set), and the record
  would depend on an environment variable.
- An opt-in absolute path (a `WorkflowOptions` field, a CLI flag, a Python argument). It needs all
  three front ends for something that `praenora modules inspect` already shows on the machine where
  the run happened. Deferred: an optional key can be added later without breaking readers.
- The search path's value as the caller, the configuration or the environment gave it. It is
  often absolute, and then holds the home directory again.
- The locations in the reproducibility section. They would change the fingerprint with the layout
  of the search paths, against ADR-0016.

## Consequences

- Given the run's search paths, a reader can tell which manifest file, and so which library, a run
  used, even with several copies installed. Runs whose search paths hold the same files in the same
  layout record the same locations wherever the paths are.
- The fingerprint does not change.
- A run record written before this change no longer satisfies the `run-record` 0.2 schema.
- Error records can still hold absolute paths, in their location, message, details and cause;
  whether they should stays open.

## References

- [reproducibility.md](../specs/reproducibility.md#where-a-manifest-was-found): the locations.
- [manifest.md](../specs/manifest.md#discovery) and
  [runtime-services.md](../specs/runtime-services.md#discovery): the search paths.
- [ADR-0009](ADR-0009-module-manifests.md): module manifests and discovery.
- [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md): versions are not bumped for now.
- [ADR-0016](ADR-0016-reproducibility.md): the fingerprint and the run record.
- [ADR-0026](ADR-0026-runtime-service-providers.md): providers and their discovery.
- [ADR-0028](ADR-0028-error-records-keep-location-and-details.md): error records keep their
  location.
