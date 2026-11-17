# ADR-0030: One gate per module library for the whole process

- Status: Proposed
- Date: 2026-11-16
- Supersedes: none
- Superseded by: none

## Context

`features.concurrency: "module"` promises a module that no two instances of its library are called
at the same time ([ADR-0006](ADR-0006-threading-model.md)). Until now the runtime kept the promise
within one run and one contract-check session only: each `Engine` and each session loaded its own
`LoadedModule` of a library and kept its own record of the libraries that had an unresponsive
instance, and the loader kept no process-wide state. A host process was asked to keep the rest
itself: not to run two workflows, or a workflow and a contract-check session, at the same time when
they share a `"module"` library, and not to use such a library again after `MODULE_UNRESPONSIVE`.
Both are easy to break from the C++ API or from Python, whose calls release the GIL, and breaking
them can make module code race. `self_test()`, which the specification calls never concurrently with
another call into the library, had the same gap.

## Decision

- **One gate per loaded image.** Every loaded module library has one gate in the process
  (`LibraryGate`, `src/runtime/loader/library_gate.hpp`), found by the operating system's handle of
  the loaded image (`SharedLibrary::image()`), so every `LoadedModule` of the library shares it,
  whichever run or session loaded it. Several opens of one file, through a symbolic link too, are
  one image, with one copy of the library's globals; a copy of the file at another path is another
  image, with globals and a gate of its own. The registry of gates is never destroyed, so that it
  can be used at any time, also while static objects are destroyed as the process exits.
- **Leases.** Every call into a module takes a lease on its library's gate on the controlling
  thread before it is posted, and the worker gives it back as soon as the module returns: an
  exclusive lease for a `"module"` library and for `self_test()`, a shared one for an `"instance"`
  library. Leases are granted in order of arrival; shared requests in a row are granted together,
  and a shared request behind a waiting exclusive one waits for it, so neither kind can starve the
  other. A lease is plain state under the gate's mutex, not a held `std::mutex`, because it is
  released on another thread than the one that took it.
- **The wait is not part of the deadline.** The call's deadline starts once it has its lease, so a
  short timeout does not expire behind another run's long call. The wait is bounded by the calls
  ahead of it, each by its own deadline and cancellation grace (unless one has none). The caller's
  stop flag ends the wait of a `create`, `initialize` or `invoke`; since a stop may come from a
  signal handler, which cannot wake a waiting thread, the wait looks at the flag every 10 ms. A call
  stopped that way is `cancelled` without calling the module, is not counted as an invocation, and
  the run ends as stopped. The teardown's `finalize` and `destroy`, and the `diagnostics` of a
  failure, still wait for their turn: they run whether or not a stop was requested.
- **Abandoned calls.** When a call becomes unresponsive, its lease is marked abandoned and stays
  held until the call returns, which may be never. A request that conflicts with an abandoned lease
  (an exclusive one with any, a shared one with an exclusive one) is refused at once instead of
  waiting: the call's outcome is `refused` (`CallOutcome::refused`, naming the abandoned call), the
  module is not called and the instance keeps its state. Marking and releasing both take the gate's
  mutex, and marking a lease that is already released does nothing, so a call that returns just as
  its caller gives up on it leaves no stale mark. Once the abandoned call returns, the library can
  be used again: a library whose abandoned call has returned is as safe to call as any other.
- **Refusals.** A refused `create`, `initialize` or `invoke` fails the run with
  `MODULE_UNRESPONSIVE` ("step 'b' (module m@1.0.0): invoke was refused: invoke by step 'a' of
  workflow w@1.0.0 (module m@1.0.0) did not return within its deadline and the cancellation grace
  and may still be running in the library"), details `{"abandoned_call": {"module", "call",
  "by"}}`, and marks the step failed. A refused invoke never reached the module, so it is the
  step's last error and not an invocation in the metrics, and the failure asks the step for no
  diagnostics, which its library would refuse too. `LoadedModule::load()`
  refuses the library the same way at the end of check 2, after the library is opened (opening a
  loaded library runs none of its code) and before any of the module's code runs, located at the
  manifest's `/library/path`: a run fails at its step's load, `validate --load` reports it with the
  details, and the contract checks fail `library.load`.
- **Teardown.** The engine no longer keeps its own record of libraries with an unresponsive
  instance: it calls `finalize` and `destroy` through the gate, and keeps an instance whose call is
  refused (never finalized, destroyed or freed, its services withdrawn), with the warning
  `teardown.skipped`, which now names the abandoned call. An instance whose `finalize` is refused
  is not destroyed either: a `ready` instance is never destroyed without being finalized, even if
  the abandoned call returns in between. `ModuleInstance`'s destructor, and the contract checks,
  keep an instance whose `finalize` or `destroy` is refused the same way (event `instance.kept`).
  Within one run this is what the teardown did before, and it now also holds for an abandoned
  call of another run.
- **Who made a call.** `InstanceOptions::caller` names who makes an instance's calls, for the
  refusals of others: "step 'a' of workflow w@1.0.0" in a run, "the contract checks" in a session.
- `MODULE_UNRESPONSIVE` is reused for refusals rather than a new code: the library is in effect
  unresponsive, the exit status (5) and the advice are the same, and no new error code is needed.

### Builder notes (Proposed)

- The gate is per copy of the runtime library: two copies loaded into one process (two different
  runtime builds) do not share gates. The CLI, the Python extension and the test executables each
  hold one copy.
- `ModuleInstance::destroy()` returns the call's outcome, so the engine can tell a refused destroy
  from one that is done; `ModuleInstance::last_refusal()` names the abandoned call of the last
  refusal.
- `validate --load` now passes the details of a load error into its diagnostic (they were dropped
  before), so a refusal's `abandoned_call` reaches the report, the CLI's JSON and Python. The
  contract checks' `library.load` message starts with `MODULE_UNRESPONSIVE:` for a refusal, as it
  names the code of a service failure, since a check result carries no code of its own.
- `LibraryGate::acquire()` allocates the lease before the request joins the queue and removes the
  request on every way out but a grant, so that running out of memory can neither leave a request
  at the head of the queue nor a held entry that no lease releases.
- `CallResult::called` tells whether a call reached the module; the call's lease is passed to
  `ModuleInstance::run()` explicitly (exclusive for `self_test()`); an invocation's `tick` is set on
  the worker as the call starts, after its turn at the library.
- The fixture module fx-hang fails an `invoke` or `finalize` that starts while another call into
  its library is running, so the tests see calls that overlap.

## Alternatives considered

- **Keep the rule for the host.** What the runtime did; easy to break from the C++ API and Python,
  and a broken rule is a data race in module code.
- **Refuse the library for the rest of the process after `MODULE_UNRESPONSIVE`.** Simpler and
  deterministic, but stricter than needed once the abandoned call has returned, and it would make
  one test that abandons a call poison every later test of the library in the same process.
- **Count the wait toward the call's deadline.** Two runs with different timeouts sharing a library
  would time out spuriously behind each other's calls.
- **Key the gate by the manifest, the path or the SHA-256 of the library.** A manifest or a path
  misses a library reached through two manifests or a link; a digest would serialize copies at
  different paths, which share no globals.
- **Wait for an abandoned call instead of refusing.** It may never return; the waiting run would
  hang with it.

## Consequences

- Two workflows may run at the same time in one process, and a workflow beside a contract-check
  session, whatever libraries they share: calls into a `"module"` library take turns. Their results
  may still depend on the order of those calls, because the library's globals are shared, so runs
  whose results must be reproducible still run one after another or in separate processes.
- After `MODULE_UNRESPONSIVE`, the process needs no restart once the abandoned call returns; until
  then every use of the library fails with `MODULE_UNRESPONSIVE` naming that call.
- Every module call takes and gives back a lease: one mutex and one condition variable per call,
  negligible beside a call through a worker thread.
- A run that shares a library with a long-running run waits for it; its own timeouts do not cover
  that wait. A stop request ends the wait of a `create`, `initialize` or `invoke`, but the stopped
  run's teardown still waits for its turn to finalize and destroy its instances.

## References

- [docs/specs/threading-and-ownership.md](../specs/threading-and-ownership.md#concurrency-across-instances)
- [docs/specs/time-cancellation-timeouts.md](../specs/time-cancellation-timeouts.md#deadlines)
- [ADR-0006](ADR-0006-threading-model.md), [ADR-0011](ADR-0011-tick-based-synchronous-execution.md),
  [ADR-0012](ADR-0012-time-timeouts-and-cancellation.md),
  [ADR-0021](ADR-0021-runtime-services-through-host-api.md)
