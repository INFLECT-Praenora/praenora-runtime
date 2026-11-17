# ADR-0032: Service lookups in the message trace

- Status: Proposed
- Date: 2026-11-17
- Supersedes: none
- Superseded by: none

## Context

[ADR-0021](ADR-0021-runtime-services-through-host-api.md) decided that each use of a runtime
service is logged in the trace. Its builder note met that with the `service.lookup` log event,
because the message trace (`trace/messages.jsonl`,
[ADR-0020](ADR-0020-message-envelope-and-asyncapi.md)) held envelopes and result diagnostics only
and no production service existed. The simulation service `praenora.simulation@0` now exists
([ADR-0027](ADR-0027-simulation-service-v0.md)), served by the providers `test-simulation` and
`roadrunner`. A run's trace showed the messages between its steps, but which services a step asked
for, and whether it got them, appeared only in debug logs: they are not deterministic, carry
wall-clock timestamps and are not part of the run's output directory.

Modules mostly look a service up once and keep the table, which lives until the process ends: the
test module `simulating-module` does so when it is created, before the first tick.

## Decision

- With tracing on, the message trace also holds the runtime-service lookups of the run's modules,
  in lookup records: the canonical JSON of `{"tick", "step", "phase", "service", "major",
  "provided", "count"}` followed by `\n`, like every line of the trace.
  - `phase` is the call into the step's instance that made the lookups: `create`, `initialize`,
    `invoke`, `finalize`, `destroy` or `diagnostics`.
  - `tick` is the tick of the invocation, and null for every other call.
  - `service` and `major` are the name and major version the module asked for; `provided` says
    whether `get_service` returned a table.
  - `count` is the number of lookups the record stands for.
- There is one lookup record per service, major version and outcome for each call, in the order of
  each one's first lookup in that call, so a module that looks a service up in a loop adds one
  record, not one per lookup.
- The lookups traced are those a module makes while one of the run's calls into its instance is in
  flight, from any of its threads. Lookups made between calls, by a module's own threads, are not
  traced: their place in the trace would depend on timing. A call that never returns, and is
  abandoned, has the lookups it made before it was abandoned.
- A call's lookup records are written as soon as the call returns. For an invocation that is after
  the records of the envelopes going in and before those of the envelopes coming out. The lookups
  of `create` and `initialize` therefore come before tick 0, in schedule order, and those of
  `finalize` and `destroy` at the end of the trace, in teardown order. The lookups of a call that
  does not succeed, and of the `diagnostics` call that a failure makes, are written when the
  teardown starts: nothing is written to the trace in between, so they land at the same place, and
  a trace that cannot be written never hides how the call ended.
- A service name that is not valid UTF-8 is written with what is not UTF-8 replaced by U+FFFD, so
  the name a module asks for cannot make a traced run fail where an untraced one does not.
- The run record's `execution.trace.messages` still counts envelopes only; its `sha256` covers the
  whole file, lookup records included.
- The `service.lookup` log event stays as it is. The contract checks write no trace and record no
  lookups.
- The trace has no schema and no format version of its own: its records are described here, in
  ADR-0020 and in [observability.md](../specs/observability.md#message-trace), which change in place
  ([ADR-0014](ADR-0014-versioning-and-compatibility-policy.md); versions are not bumped for now).

### Builder notes (Proposed)

- `InstanceOptions::record_lookups` turns the recording on; the engine sets it when the run writes
  a trace, so a run without one records nothing. `ModuleInstance` starts recording when a call is
  posted and stops when the call returns or is abandoned, under the lock that `get_service` takes;
  a call that cannot be posted keeps nothing. `take_lookups()` hands the engine the lookups of the
  calls that ended since it last asked (`ServiceLookup`, `src/runtime/instance/services.hpp`); the
  engine takes them after every call, traced or not.
- The records of a call are told apart by service, major version and outcome; an invoke's records
  carry the tick it was given when it started. A lookup that cannot be recorded for lack of memory
  is left out; the module gets its table all the same.
- The engine writes the lookups of `create` and `initialize` once the call has succeeded, and those
  of `invoke` once its result has been checked and counted, before its `out` records. The teardown
  writes those not written yet when it starts, in schedule order, then those of each `finalize` and
  `destroy`. A trace write that fails after a call that succeeded fails the run with `IO_ERROR`, as
  the `out` records always could. The teardown cannot throw: a write that fails there leaves the
  trace's file in error, and closing it reports `IO_ERROR`; a run that is failing keeps its own
  error.
- Message records have `direction`, lookup records `phase`: a reader tells them apart by either
  key.
- The fixture module `fx-service` takes the parameters `lookup_in` (the calls in which it looks up
  its `lookups`), `repeat` (how many times) and `fail_in` (`create` or `initialize`, which then fail
  after their lookups), and its manifest declares `features.diagnostics`.

## Alternatives considered

- **One record per lookup, in order.** The most literal trace, but a module that looks a service
  up in an inner loop would fill the trace, and the memory that holds a call's lookups until it
  returns.
- **Keep lookups in the logs only.** Logs are not deterministic, are not part of the run's output
  directory and need the debug level.
- **Trace only the lookups of invocations.** It would miss the usual case, a module that looks a
  service up when it is created.
- **Name the provider in each record.** The run record's reproducibility section already says which
  provider serves each service of the run (`services`), and the fingerprint covers it.
- **Give every record a `kind`.** It would change every message record, and `direction` and
  `phase` already tell the two kinds apart.

## Consequences

- One deterministic file shows, for each step and call, the messages and the services used; two
  runs of a deterministic workflow still give byte-identical traces.
- A reader that expects every trace record to carry an envelope has to skip lookup records.
- A traced run whose modules use services writes a few more lines per call; how many depends on
  the services each call asks for, not on how often it asks.

## References

- [docs/specs/observability.md](../specs/observability.md#message-trace)
- [docs/specs/runtime-services.md](../specs/runtime-services.md#visibility)
- [docs/specs/module-abi.md](../specs/module-abi.md#host-api)
- [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md),
  [ADR-0020](ADR-0020-message-envelope-and-asyncapi.md),
  [ADR-0021](ADR-0021-runtime-services-through-host-api.md),
  [ADR-0027](ADR-0027-simulation-service-v0.md)
