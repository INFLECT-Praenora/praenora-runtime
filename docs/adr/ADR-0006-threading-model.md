# ADR-0006: Threading: per-instance worker threads, serialized calls, thread-safe host callbacks

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Modules are arbitrary native code. The host needs enforceable timeouts, predictable thread affinity for module state, and deterministic behavior, without requiring modules to be thread-safe.

## Decision

- Each module instance gets a dedicated host worker thread. All calls for one instance (`create` through `destroy`, including `diagnostics`) run on that thread and never concurrently. Workers are named `praenora:<step>` where the platform allows.
- `praenora_module_get_api` and `describe` must be thread-safe and side-effect free.
- The manifest field `features.concurrency` is `"module"` (default: never call two instances of the library concurrently) or `"instance"` (instances are independent). The MVP engine runs one module call at a time, so the field has one effect, in the teardown: once an instance of a `"module"` library is unresponsive, its call may still be running, so the engine calls no other instance of that library, not even to finalize or destroy it, and keeps them for the rest of the process; with `"instance"` it tears them down as usual (ADR-0011).
- Host callbacks (`log`, `log_enabled`, `cancel_requested`, `get_service`) are thread-safe and may be called from any module thread while the instance exists. Modules must stop their own threads before `destroy` returns.
- One orchestrator thread runs ticks and waits on a deadline for each posted call.
- Timeouts are enforced by the host; cancellation is cooperative (ADR-0012).
- Modules run in-process. A crash inside a module takes down the host; this is documented (OQ-5).

### Builder notes (Proposed)

- Each instance owns one `std::thread` (`InstanceWorker`), named `praenora:<step>` through `pthread_setname_np` (Linux keeps 15 bytes; macOS names only the calling thread, so the worker names itself) or `SetThreadDescription`, which is looked up at run time so the runtime still loads on Windows versions without it. The worker runs posted tasks in order; the controlling thread blocks on a condition variable until the call returns or its time is up.
- Every task holds shared ownership of what the module may touch during the call: the call's input document and sinks, the instance's host API context and the loaded library. A call that outlives its instance (an unresponsive one) therefore never reaches freed memory, and a detached worker keeps its queue alive by itself.
- The host API context of an abandoned (unresponsive) instance is kept for the rest of the process, together with the module's instance pointer, because the module may still use the host API; its service registry pointer is cleared under a lock first, so `get_service()` returns NULL from then on and the registry may be destroyed.
- One thread controls an instance (create through destroy, diagnostics); `request_cancel()` (lock-free atomic operations only, so async-signal-safe) and `state()` may be called from any thread. Instance state is a `std::atomic<StepState>` that each control call takes to `busy` with an atomic compare-exchange before it reads anything else of the instance, so a control call made while another one is in progress is refused: create, initialize, invoke and finalize throw `INVALID_STATE`, `destroy()` does nothing but log a warning (event `call.refused`), and `diagnostics()` returns nothing. A call whose preparation throws before it reaches the worker restores the earlier state.
- `diagnostics()` also returns nothing instead of throwing when no module instance exists or the instance is unresponsive, so failure reports can always ask for it.
- The runtime links the platform thread library (`Threads::Threads`) and, where it is separate, the dynamic-loader library (`CMAKE_DL_LIBS`) through `praenora_runtime_deps`.
- `features.concurrency: "module"` is kept within one run and one contract-check session, not across them. Each `Engine` loads its own `LoadedModule` of every library and keeps its own record of the libraries that have an unresponsive instance, and so does each contract-check session; the loader keeps no process-wide state. So two workflows run at the same time in one process (allowed: `Runtime` is thread-safe and the Python calls release the GIL), or a run or check after `MODULE_UNRESPONSIVE`, can call instances of one `"module"` library concurrently. The specifications state the scope and the host's part ([docs/specs/threading-and-ownership.md](../specs/threading-and-ownership.md#concurrency-across-instances)): a process must not run two workflows, or a workflow and a contract-check session, at the same time when they share a `"module"` library, and must not use such a library again after `MODULE_UNRESPONSIVE` (separate processes, or a restart). The `praenora` tool runs one workflow or session per process. Enforcement in the loader (refusing a library while an abandoned call into it still runs; serializing concurrent runs that share one) is not built in this version: it needs process-wide concurrency state in the loader.
- Later: [ADR-0030](ADR-0030-library-gate-across-runs.md) builds that enforcement: one gate per loaded library for the whole process. Calls into a `"module"` library take turns across runs and contract-check sessions, `self_test()` waits for every other call, and while an abandoned call may still be running in a library, every call and load that would have to wait for it is refused with `MODULE_UNRESPONSIVE`. The host's two rules above no longer apply.
- Later: [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) amends "Modules run in-process" above, which stays the default (isolation mode `in-process`): a step in isolation mode `process` runs its module instance in a module host, a process of its own, where one thread makes the instance's calls, one at a time, as its worker thread does here, and the host callbacks stay thread-safe. A crash there ends only the module host and fails the step with `MODULE_CRASHED`, and a call that does not return within its deadline and the cancellation grace is ended by killing the module host, so no thread is detached and no library pinned. The out-of-process isolation below is built that way, per step; OQ-5 is resolved.

## Alternatives considered

- **Calls on the orchestrator thread.** Simpler, but a hung module blocks the host and no timeout can be enforced.
- **Thread pool with arbitrary thread per call.** Breaks thread-affine module state.
- **Out-of-process isolation.** Contains crashes and allows kill-on-timeout, but needs a transport, serialization of lifecycle state, and platform-specific process management. Deferred (OQ-5).
- **Parallel step execution.** Deferred (OQ-11); sequential execution keeps runs deterministic.

## Consequences

- Timeouts work even if a module never returns, at the price of an unresponsive instance that is detached and whose library is pinned.
- Thread-affine module state is safe.
- Each call costs a thread handoff; the perf probe reports the overhead.
- Status queries stay thread-safe and do not wait on module code.

## References

- [docs/specs/threading-and-ownership.md](../specs/threading-and-ownership.md)
- [docs/specs/time-cancellation-timeouts.md](../specs/time-cancellation-timeouts.md)
- [docs/open-questions.md](../open-questions.md)
