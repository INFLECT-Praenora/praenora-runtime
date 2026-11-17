# Threading and ownership

- **Version:** ABI 0.0 and runtime 0.2.0
- **Status:** Normative, MVP. Proposed
  ([ADR-0005](../adr/ADR-0005-ownership-and-allocation-across-the-abi.md),
  [ADR-0006](../adr/ADR-0006-threading-model.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

A module and the runtime may be built with different compilers, standard libraries and allocators,
and modules are arbitrary native code running in the runtime's process. This document says who owns
what across the [module ABI](module-abi.md), which threads call into a module and what a module may
do with threads of its own, and how the public C++ API may be used from several threads.

## Ownership across the ABI

| Object | Owner | Valid | Rule for the module |
|---|---|---|---|
| Input documents (`praenora_bytes`) | host | during the call | Read only. Copy whatever is needed after the call returns. |
| Host API (`const praenora_host_api*`) | host | from the `create` call until `destroy` returns | MUST NOT be used after `destroy` returns. |
| Sinks (`praenora_sink*`) | host | during the call | Write only during the call; never keep the pointer. |
| Bytes passed to `sink->write` | module | until `write` returns | The host copies them before `write` returns, so the buffer may be reused at once. |
| Instance (`praenora_instance*`) | module | from `create` until `destroy` | Allocated by `create` and freed by `destroy`, with the module's own allocator. |
| Function table and `describe()` output | module | while the library is loaded | Static data; never changed. |
| Service tables (`get_service`) | host (a service provider's tables: the provider) | while the instance exists; a provider's tables until the process ends | Never freed by the module. A service's own objects follow the service's specification. |

- Nothing else crosses the boundary: no `malloc`/`free` pairing between host and module, no
  `FILE*`, no C++ object, no exception and no `longjmp`.
- The runtime keeps each call's input document alive for as long as the call runs, even after it
  stopped waiting for the call, so a module that overruns its deadline never reads freed memory.
- A sink refuses a write that would take it beyond its limit (256 MiB for each sink of a call, 1 MiB
  for `describe()`) with `PRAENORA_STATUS_RESOURCE_EXHAUSTED`, and then every later write, so a
  refused document never continues with a gap. A module SHOULD stop and return that status.
- A zero-length write or log message MAY come with a NULL data pointer. A NULL data pointer with a
  nonzero size is invalid: a sink refuses the write with `PRAENORA_STATUS_INVALID_ARGUMENT`, and the
  log callback drops the message.

## Threads that call a module

The runtime gives each module instance a dedicated worker thread, named `praenora:<step>` where the
platform allows it. A host MUST give these guarantees:

- Every call for one instance (`create`, `initialize`, `invoke`, `finalize`, `destroy` and
  `diagnostics`) runs on that instance's thread, in the order posted, never two at a time. A module
  MAY therefore keep thread-affine state in an instance.
- `praenora_module_get_api` and `describe()` may be called from any thread, also while instances of
  the library are being called; they MUST be thread-safe and free of side effects.
- `self_test()` is called only by the contract checks, never concurrently with another call into the
  library.
- Calls into different instances of one library are never concurrent unless the manifest declares
  `features.concurrency: "instance"` (see below).

The runtime of this version makes exactly one module call at a time in a run: one control thread
runs the ticks, posts each call to the instance's worker and waits for it on a deadline
([time-cancellation-timeouts.md](time-cancellation-timeouts.md)).

## What a module must do

- The host callbacks (`log`, `log_enabled`, `cancel_requested`, `get_service`) are thread-safe. A
  module MAY call them from any of its threads while the instance exists. Which threads may use a
  service's table, and the objects it hands out, is the service's specification's to say (for the
  simulation service: [simulation-service.md](simulation-service.md#threading)); a provider's engine
  is shared by every module of the process that uses its services. Objects a service hands out to a
  module (the simulation service's sessions, plans and kernels) are the module's to close before its
  `destroy` returns, like its threads.
- A module that starts threads MUST stop and join them, and finish any work it handed to other
  threads, before `destroy` returns. After `destroy` returns no thread of the module may touch the
  instance or its host API.
- Mutable state shared by several instances of the library (global or static variables) MUST be
  synchronized when the manifest declares `features.concurrency: "instance"`, and SHOULD be avoided
  altogether: it makes results depend on how instances are interleaved, which the contract checks'
  `invoke.determinism` check detects in the common case
  ([reproducibility.md](reproducibility.md#determinism)).
- A module SHOULD poll `cancel_requested()` in long computations. A module that blocks without
  polling cannot be stopped and ends up unresponsive.
- Modules run in the runtime's process: a crash inside a module (a segmentation fault, an abort)
  ends the host process. Isolating modules in their own processes is an open question
  ([open-questions.md](../open-questions.md), OQ-5).

## Concurrency across instances

The manifest field `features.concurrency` declares whether the library's instances may be called at
the same time:

| Value | Meaning | Effect in this runtime version |
|---|---|---|
| `module` (default) | Two instances of the library MUST NOT be called concurrently. | The calls of a run are sequential anyway. When an instance of the library becomes unresponsive, its call may still be running, so the run calls no other instance of the library again. |
| `instance` | Instances are independent and MAY be called concurrently. | The calls of a run are sequential anyway. The other instances are finalized and destroyed as usual. |

The runtime keeps the `module` rule within one run and within one contract-check session
(`praenora modules check`, `Runtime::check_module()`), whose calls it serializes, and not across
them: what one run or session knows about a library, an unresponsive instance included, does not
reach another. A host process that uses the C++ API or the Python package therefore keeps the rule
itself:

- It MUST NOT run two workflows at the same time, on two threads for example, nor a workflow and a
  contract-check session, when both use the same `module` library. Run them one after another, or
  in separate processes.
- After `MODULE_UNRESPONSIVE`, it MUST NOT use that library again in the same process, in a later
  run or contract-check session: the abandoned call may still be running in it. Restart the process
  instead.

The `praenora` tool runs one workflow or one contract-check session per process, so it keeps both
rules. This runtime version does not enforce them across runs; a host that breaks them can make
module code race.

## Unresponsive instances

When a call does not return within its deadline plus the cancellation grace, the instance is
abandoned for the rest of the process:

- its state becomes `unresponsive`; when the call was a `create`, `initialize` or `invoke`, the run
  fails with `MODULE_UNRESPONSIVE`, while a `finalize` or `destroy` of the teardown, or a
  `diagnostics` call made while a failure is reported, leaves the run's ending as it was
  ([time-cancellation-timeouts.md](time-cancellation-timeouts.md#deadlines));
- its worker thread is detached and keeps running the call; its library stays loaded (pinned);
- its host API stays valid, but `get_service` returns NULL from then on for every service, provider
  services included, because the runtime's service registry may go away;
- it is never finalized or destroyed, and its memory is never freed.

With `features.concurrency: "module"`, the run's other instances of the library that were created
are kept the same way: never called again, never finalized, destroyed or freed, their services
withdrawn. A warning (event `teardown.skipped`) names each and says what was left undone. Instances
that were never created, or already destroyed, are released as usual. Other runs in the process are
not told ([Concurrency across instances](#concurrency-across-instances)). The rationale is in
[ADR-0006](../adr/ADR-0006-threading-model.md) and
[ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md).

## Runtime threads

| Thread | Name | Work |
|---|---|---|
| The caller's thread | the caller's | Validation, loading (including `describe()`), and the ticks of `Workflow::start()`, `tick()` and `run()`. `Workflow::start()` (and `validate --load` and the contract checks) loads and starts the providers of the services on that thread, before any module instance is created. |
| A service provider's own threads, if it starts any | the provider's | Whatever the provider does in the background. A provider is never stopped or unloaded: its threads MAY outlive every `Runtime` of the process ([runtime-services.md](runtime-services.md#provider-interface)). |
| One worker per module instance | `praenora:<step>` | Every call into that instance. |
| The status-file writer, when a status file is configured | `praenora:status` | Rewrites the status file ([observability.md](observability.md#status-file)). |
| The CLI's signal watcher (`praenora run` only) | none | Turns SIGINT, SIGTERM or a Windows console event into `request_stop()`. |

## Public C++ API

- `praenora::Runtime` is immutable once constructed and thread-safe: any number of threads MAY use
  one `Runtime` at the same time. Copies share its state, and a `Workflow` it opened MAY outlive it.
- `praenora::Workflow` is one run. Its control calls (`start`, `tick`, `run`, `close`) MUST be made
  by one thread at a time: a control call that starts while another one is in progress fails with
  `INVALID_STATE` without touching the run, except `close()`, which then only requests a stop.
- The queries (`state`, `status`, `status_json`, `metrics_json`, `latest_outputs_json`,
  `run_record_json`, `fingerprint`, `output_dir`) and `request_stop()` MAY be called from any thread
  at any time, also during a control call. Status and metrics come from a copy guarded by a mutex,
  so a query never waits for a module call. `request_stop()` is lock-free and async-signal-safe.
- Destructors never throw. A `Workflow` MUST NOT be destroyed while another thread still calls it.
- The Python bindings release the interpreter lock during their long calls, so another Python thread
  can query the status or request a stop during `run()`
  ([ADR-0018](../adr/ADR-0018-python-bindings-with-swig.md)).

## References

- [ADR-0005](../adr/ADR-0005-ownership-and-allocation-across-the-abi.md): ownership and allocation.
- [ADR-0006](../adr/ADR-0006-threading-model.md): threading.
- [ADR-0023](../adr/ADR-0023-public-cpp-api.md): the public C++ API.
- [module-abi.md](module-abi.md): the ABI.
- [runtime-services.md](runtime-services.md): service providers and their lifetime.
- [module-developer-guide.md](../module-developer-guide.md#abi-rules): the rules for module authors.
