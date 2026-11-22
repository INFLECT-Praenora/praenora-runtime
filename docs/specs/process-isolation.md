# Process isolation of module instances

- **Version:** runtime 0.2.0; module ABI 0.0, which does not change
- **Status:** Normative. That a step's module instance can run in a module host of its own,
  chosen per step and with its providers, where its crash fails only that step and a call that
  never returns is killed, is Accepted
  ([ADR-0037](../adr/ADR-0037-process-isolation-of-module-instances.md), in part), and so are
  sharing compiled models between such steps, opt-in per step, through a bounded store in the
  runtime's process ([ADR-0038](../adr/ADR-0038-shared-compiled-models-between-module-hosts.md),
  in part), and the restricted sandbox profile on macOS
  ([ADR-0039](../adr/ADR-0039-restricted-sandbox-profile-for-module-hosts.md), in part); the rest
  is Proposed
  ([ADR-0037](../adr/ADR-0037-process-isolation-of-module-instances.md),
  [ADR-0038](../adr/ADR-0038-shared-compiled-models-between-module-hosts.md),
  [ADR-0039](../adr/ADR-0039-restricted-sandbox-profile-for-module-hosts.md)). The channel between
  the runtime and a module host is internal to one runtime build: it is not a public format, and it
  may change with any runtime release.
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

A module instance can run in a process of its own, a *module host*, instead of the runtime's
process. A crash in the module (a segmentation fault, an `abort()`, an `exit()`) then ends the
module host and fails the step, never the runtime; and a call that never returns can be ended for
good, by killing the module host. Modules do not change: the [module ABI](module-abi.md), the SDK,
the protocol and every document a module sees are the same in both places, and a module cannot
tell where it runs other than by its process id.

This document describes how a workflow configuration chooses where each step's instance runs, the
module host, how the runtime uses it for one module instance, where the service providers of its
module run, how module hosts share compiled models, the sandbox profile a module host can apply to
itself, and how runs, validation and the front ends use it.

## Terms

- **Module host**: the executable `praenora-module-host`, and each process started from it. One
  module host serves exactly one module instance.
- **Channel**: the private connection between the runtime and one module host, a POSIX socket pair.
  Its frames are an internal protocol of one runtime build.
- **Shared compiled models**: a store in the runtime's process through which the module hosts of
  the steps that share compiled models hand them to each other.
- **Sandbox profile**: what a module host may do, which it sets for itself before it loads any
  library: `none`, or `restricted` (macOS).

## Configuration

A step's *isolation mode* says where its module instance runs: `in-process` (the default, in the
runtime's process) or `process` (in a module host of its own). The workflow configuration
(`workflow-config` 0.1, [configuration.md](../configuration.md#isolation)) sets it in an object
`isolation`, at `run.isolation` (the default of every step) and at `steps.<id>.isolation` (that
step):

| Field | Values | Default |
|---|---|---|
| `mode` | `"in-process"`, `"process"` | `"in-process"` |
| `share_compiled_models` | boolean | `false` |
| `sandbox` | `"none"`, `"restricted"` | `"none"` |

- A step's settings are resolved field by field: the step's field, else the run's, else the
  default. The object admits no other field (`SCHEMA_VIOLATION`).
- `share_compiled_models` makes the step's module host share compiled models with the module hosts
  of the other steps that share them ([Shared compiled models](#shared-compiled-models)).
- `sandbox: "restricted"` makes the step's module host apply the restricted profile to itself
  ([The restricted profile](#the-restricted-profile)).
- A caller MAY override the mode of every step for one run: `WorkflowOptions::isolation`
  (`IsolationMode::in_process` or `IsolationMode::process`), `praenora run --isolation
  <in-process|process>`, Python's `isolation="in-process"` or `"process"`. The override
  `in-process` also turns every step's `share_compiled_models` off and sets its `sandbox` to
  `none`; the override `process` leaves both as the file says. The run record lists the override
  in `execution.overrides.isolation`, as it lists the other overrides.
- Validation MUST refuse, with `UNSUPPORTED` located at the field that set the step's mode
  (`/steps/<id>/isolation/mode` or `/run/isolation/mode`; no location when the caller's override
  set it), a step whose mode is `process` on a platform without process isolation (Windows in this
  version).
- Validation MUST refuse, with `CONFIG_INVALID` located at the field that set it
  (`/steps/<id>/isolation/share_compiled_models` or `/run/isolation/share_compiled_models`), a step
  whose mode is `in-process` and whose `share_compiled_models` is `true`, or whose `sandbox` is
  `restricted` (`/steps/<id>/isolation/sandbox` or `/run/isolation/sandbox`): each applies only to
  a step that runs in a module host. A field of the run refuses each such step.
- Validation MUST refuse, with `UNSUPPORTED` located at the field that set it, a step in mode
  `process` whose `sandbox` is `restricted` on a platform without the restricted profile (anything
  but macOS in this version).
- A step's module may declare runtime services, required or optional, in either mode: the
  providers of a step in mode `process` are loaded in its module host
  ([Service providers](#service-providers)).
- The isolation is not part of the run fingerprint: results do not depend on it
  ([Determinism](#determinism)). The run record's `execution.isolation` lists every step's resolved
  settings, all fields, `{<step>: {"mode": "in-process" | "process", "share_compiled_models":
  <boolean>, "sandbox": "none" | "restricted"}}`
  ([reproducibility.md](reproducibility.md#run-record)).

## The module host

- The executable `praenora-module-host` is built with the runtime on POSIX systems (not on
  Windows). It holds the runtime's own code: the loader, the provider loader, the documents and
  their checks.
- **Where it is.** An installation has it at `<prefix>/libexec/praenora/praenora-module-host`; a
  build tree at `build/<preset>/libexec/praenora/praenora-module-host`. The runtime finds it
  relative to its own binary, at
  `<directory of the runtime library>/praenora/../../libexec/praenora/` (the same directory as
  above), so a build tree and an installation, moved or not, each use their own. No environment
  variable names it. A runtime installed from the Python wheel has no module host.
- **Starting.** The runtime starts it with `posix_spawn()`, never by `fork()` of its own process,
  with the runtime's process id as its only argument:
  - descriptor 3 is the module host's end of the channel, standard input is `/dev/null`, and
    standard output and error are the runtime's, as an instance in the runtime's process has them;
  - every other descriptor is closed: on macOS by the spawn (`POSIX_SPAWN_CLOEXEC_DEFAULT`),
    elsewhere by the module host itself, first thing; and the module host makes descriptor 3
    close-on-exec, so no program the module runs inherits the channel;
  - it gets a process group of its own, so a terminal's Ctrl+C reaches only the runtime (which
    then stops the call in flight), and an empty signal mask; it inherits the environment and the
    current directory;
  - a process group of its own is a background one of the runtime's terminal, if the runtime has
    one, which would stop a process of it that writes to the terminal under `stty tostop`, changes
    the terminal's settings (`SIGTTOU`) or reads from it (`SIGTTIN`): the module host ignores both
    signals, first thing, so such a write or change goes on as in the runtime's process, and a read
    fails (`EIO`; standard input is `/dev/null` anyway). The processes a module starts inherit the
    ignored signals.

  The runtime changes no process-wide signal disposition of its own.
- **Standard streams.** Standard output and error are the runtime's descriptors, but their stdio
  buffers (`stdout`, `std::cout`) are the module host's own. What a module writes through a buffer
  reaches the runtime's standard output when the module host flushes it: when the buffer fills,
  and at the latest as it exits on the runtime's request ([Ending](#ending)). Its order among the
  runtime's own output can therefore differ from an instance in the runtime's process, and what is
  still buffered is lost when the module host is killed or crashes (a call that does not return,
  a crash, a stop that ends it, an exit that does not come within the lifecycle timeout). Standard
  error is unbuffered, so what goes there arrives at once; a module that needs its standard output
  to arrive flushes it (`fflush(stdout)`, `std::flush`).
- **Handshake.** The runtime sends the channel's protocol number and its version; the module host
  answers with its own. A module host of another runtime version or channel protocol is refused.
- **When it cannot be used**, loading the module fails with `MODULE_HOST_UNAVAILABLE`, located in
  the module's manifest, whose `details.reason` says why ([error-taxonomy.md](error-taxonomy.md)):

  | `details.reason` | Meaning |
  |---|---|
  | `missing` | No executable where the runtime looks for it (`details.executable`). |
  | `spawn` | It could not be started, for example because it is not executable (`details.system`: the operating system's text); always on Windows. |
  | `handshake` | It ended, answered something else than a module host's answer, or did not answer within 30 seconds. |
  | `build` | It answered with another runtime version or channel protocol. |
  | `sandbox` | The restricted profile cannot be applied (`details.system`: the system's text, "Operation not permitted" in a runtime that runs in a sandbox already), its temporary directory cannot be made, or the platform has no restricted profile ([The restricted profile](#the-restricted-profile)). |

- **The runtime's death.** The module host ends at once, whatever the module is doing, when its end
  of the channel reaches the end of the stream: when the runtime's process ends, however it ends,
  no module host is left behind. On Linux it also asks for `SIGKILL` when its parent dies
  (`PR_SET_PDEATHSIG`) and checks that its parent is still the runtime.
- **Waiting.** The runtime waits for every module host it started (no zombie is left). When a
  module host ends, however it ends, whatever is left in its process group (processes the module
  started) is killed before the module host is waited for. A module host that is stopped (by
  `SIGSTOP`, say) has not ended: a call into it waits as for a module that does not return, and
  the call's deadline and grace end it.

## The channel

The channel carries frames: a fixed little-endian header (a magic number, the kind of frame, the
id of the request or call it belongs to, and the length of its payload) and the payload. It is
internal; this section says what flows over it, not how it is laid out.

| From | What |
|---|---|
| runtime | The handshake (the channel protocol and the runtime's version). |
| runtime | A load: the module's manifest path and SHA-256, and the library SHA-256 it expects; for each provider of the module's services, its manifest path and SHA-256, its library path and the SHA-256 validation computed, the services the module gets from it, and the lowest log level the runtime records of it; whether the step shares compiled models; and the sandbox profile, with the module host's temporary directory for the restricted one. |
| runtime | Each call, `create`, `initialize`, `invoke`, `finalize`, `destroy` and `diagnostics`, with its document, the limits of its sinks, the lowest log level the runtime records, and whether a stop was requested before it started. |
| runtime | A cancellation of a call, and the request to exit. |
| runtime | The answer to a request of the shared compiled models: whether the store holds the key, and its entry. |
| module host | The answer to the handshake. |
| module host | The load's result: the module's `describe()` document, its library's SHA-256 and which optional functions it has; or the load's error: code, message, location (in the module's manifest, or in the manifest of the provider it names) and details. |
| module host | What the module writes to a call's sinks (output or error), in chunks. |
| module host | Log records (level and message) and service lookups (name, major version, whether a table was provided); a provider's log records (the provider, level, attributes and message). |
| module host | A call's result: its status, and for `create` whether it made an instance. |
| module host | Requests of the shared compiled models, only for a step that shares them: a load (the provider, by its index in the load, and the key), a save (the provider, the key and the entry), and the notice of a save it refused (the provider, the sizes of the key and of the entry, and the key's first 256 bytes). |

- **Order.** Everything a call produced (sink writes, log records, lookups) reaches the runtime
  before that call's result: one writer in the module host serializes the frames of all its
  threads.
- **Untrusted input.** The runtime MUST treat every frame from a module host as untrusted. It checks
  each frame's magic, kind and length against the kind's limit before it reads the payload, and
  grows the payload only as its bytes arrive and never beyond the announced length, so a frame that
  announces a huge length never makes it allocate that much (for a save of the shared compiled
  models, no more than the entry limit and the key limit together, and a few bytes); it then checks
  the payload's fields and that the frame belongs to the request in flight. A frame of the shared
  compiled models from the module host of a step that does not share them is refused at its
  header, before any of its payload is read. Any violation ends the connection, kills the module
  host and counts as a crash with reason `protocol` ([Crashes](#crashes)).
- **No `SIGPIPE`.** Writes on the channel never raise `SIGPIPE`, on either side (`SO_NOSIGPIPE` on
  macOS, `MSG_NOSIGNAL` elsewhere).
- **No write of the runtime waits for good** on a module host that stops reading its channel, or
  that keeps taking a few bytes at a time. Each keeps its bounds itself: it waits for room in the
  stream with `poll()` until the end of its slice or its deadline, and then sends without waiting,
  no more than the stream has room for (on macOS it sends at most the socket's send low-water mark,
  which `poll()`'s answer promises room for; elsewhere it sends with `MSG_DONTWAIT`, and the send
  takes what fits). The socket's send timeout, which leaves reads alone, is a second bound; it is
  not the bound itself, since it bounds each wait for room and not a whole send. While a write
  waits it holds the channel's writer, which every other write waits for in the same way:
  - the handshake and the load are taken within the handshake's 30 seconds (a stop ends the load's
    write too);
  - a call's document is written in slices of 5 ms: a stop that comes before the module host has
    taken all of it ends the module host within a slice ([Stops](#stops)), however many bytes the
    module host takes meanwhile; otherwise the call's deadline cannot reach a module whose call
    has not arrived, and the end of its grace kills the module host
    ([Unresponsive calls](#unresponsive-calls));
  - a cancellation frame is tried for 5 ms, and again at each next look, until it is written;
  - the answer to a store request is taken within 30 seconds
    ([Shared compiled models](#shared-compiled-models));
  - the request to exit is taken within the lifecycle timeout, or waited for without limit when
    the step has none ([Ending](#ending)).

  A write that gives up within its frame leaves the stream unusable: nothing more is written to
  that module host, which is ended; one that wrote nothing is tried again where that is said.

## Loading

The module host first applies its sandbox profile, if it has one
([The restricted profile](#the-restricted-profile)), then loads and starts the providers of the
module's services ([Service providers](#service-providers)): no library of a provider or of the
module is loaded before the profile, so their static initializers run under it. It then loads the
module with the checks the runtime makes in its own process
([module-abi.md](module-abi.md#loading-and-table-acceptance)): the library file and its digest,
the library, the entry point, the function table, `describe()` and the identity; with the library
SHA-256 the runtime expects (validation's), checked before any of the library's code runs
(`LIBRARY_DIGEST_MISMATCH` at `/library/path`), exactly as the runtime checks it for a module it
loads in its own process. It also checks that the manifest file is still the one the runtime read
(`MANIFEST_INVALID` otherwise). A load error keeps the code, message, location in the manifest and
details it has in the runtime's process.

The runtime's process opens no library of such a module and runs none of its code. It checks the
`describe()` document it receives again, against the schema and the manifest.

The load runs on a thread with the stack of a program's main thread, as a load in the runtime's
process runs on its caller's thread (the main thread of the `praenora` tool and of Python): the
soft limit of `RLIMIT_STACK`, at least 1 MiB and at most 1 GiB, or 8 MiB when the limit is
unlimited or cannot be read. A static initializer, `describe()` or a provider's `start()` that needs
a deep stack then loads in a module host as it does in the runtime's process. The calls run on
another thread ([Calls and the host API](#calls-and-the-host-api)).

A module host that ends while it loads the module, for example in a static initializer of the
library or in a provider's `start()`, fails the load with `MODULE_CRASHED` and `details.phase`
`load`.

## Runs and validation

- **Loading.** A run loads the module of each step in process mode, at the step's turn in schedule
  order as for any step, in a module host of the step's own: two steps never share a module host,
  even when they use one module. The module host loads and starts the providers of the module's
  services first, and the runtime's process only those of its own steps
  ([Service providers](#service-providers)). The module host checks the library against
  validation's SHA-256 before any of its code runs (`LIBRARY_DIGEST_MISMATCH`), as the runtime does
  for a module it loads in its own process. A step whose module host crashes while it loads the
  module fails the run with `MODULE_CRASHED` (`details.phase` `load`) and its state is `crashed`;
  one for which no module host can be started fails it with `MODULE_HOST_UNAVAILABLE`.
- **Teardown.** The other steps are finalized and destroyed as usual; a crashed instance is not
  called again, and an unresponsive one's module host was killed. A `finalize` or `destroy` of the
  teardown whose module host crashes, or that outlives its deadline and the cancellation grace, so
  that its module host is killed ([Unresponsive calls](#unresponsive-calls)), leaves the run's
  ending as it was (a completed run stays `completed`), but not unreported: its step is `crashed`
  or `unresponsive`; its last error is `MODULE_CRASHED` with that `details.phase` and how the
  module host ended, or `MODULE_UNRESPONSIVE` with that `details.phase` and `details.terminated:
  true`; a warning says so (event `teardown.finalize_failed` or `teardown.destroy_failed`); and
  the run's health is `unhealthy`.
- **Status.** A step's state is `crashed` once its module host crashed, in status snapshots, the
  run record's final status, the CLI and Python; the health of a run with a crashed step is
  `unhealthy` ([observability.md](observability.md#health)). The metrics count a crashed call as
  failed.
- **`validate --load`** (`ValidateOptions::load`) loads each module once for each place its steps
  run: once in the runtime's process for its steps in mode `in-process`, and in a module host,
  which it then ends, once for each set of isolation settings its steps in mode `process` have (the
  sandbox profile, and whether the module host shares compiled models), so that a load that fails
  under one step's settings only, such as the restricted profile's, is reported. Each problem is
  reported for the first step with those settings. It loads and starts the providers of the steps
  in mode `in-process` in the runtime's process, and a module host those of its step, first. A
  load error in a module host is reported with the code, message, location and details it has in
  the runtime's process; a crash at load is reported as `MODULE_CRASHED`, and the runtime goes on.

## Calls and the host API

The instance's state machine, deadlines, stop flags, logging and the recording of service lookups
are the runtime's, and the same as for an instance in its process
([threading-and-ownership.md](threading-and-ownership.md),
[time-cancellation-timeouts.md](time-cancellation-timeouts.md)). Calls are made one at a time; in
the module host they all run on one thread, made as the runtime makes an instance's worker thread,
with the same stack (the load runs on a thread with a main thread's stack, [Loading](#loading)).
What the module does with its host API:

- **`log`, `log_enabled`.** A record whose level the runtime records is passed to the runtime, which
  logs it with the attributes it has in the runtime's process (`run.fingerprint`, `tick` during an
  invocation, `step`, `module`). The level comes with each call. A module's own thread may log
  between calls; the record arrives then.
- **`cancel_requested`.** It reads a flag in the module host, without a round trip to the runtime.
  When the call in flight passes its deadline or has a stop requested, the runtime sends one
  cancellation frame within a few milliseconds (it looks every 5 ms; it never waits busily); a stop
  requested before the call starts comes with the call. A call therefore ends `cancelled` or
  `timed_out` exactly as in the runtime's process.
- **`get_service`.** Services are served in the module host, by the providers loaded there
  ([Service providers](#service-providers)): the module gets the table of each service its
  manifest declares that resolved for the run, and NULL for any other, as in the runtime's process.
  The runtime's built-in services (those a caller registers, such as test services) are not
  offered to an instance in a module host. Each lookup is reported to the runtime, which records it
  and logs it (event `service.lookup`) as in its own process
  ([observability.md](observability.md)).
- **Sinks.** A call's sinks have the limits they have in the runtime's process, enforced in the
  module host with the runtime's own rules: a module meets the same refusal
  (`RESOURCE_EXHAUSTED`) at the same byte in either place.
- **`self_test`** runs only in the runtime's process (the contract checks, which run every module
  there; whether they should use a module host is an open question,
  [open-questions.md](../open-questions.md), OQ-32).

## Service providers

The providers of the services that a step's module declares, required or optional, and that
resolved for the run ([runtime-services.md](runtime-services.md#resolution)), run in the step's
module host, never in the runtime's process: a service's tables, and the engine behind them, live
in the process of the module that calls them (a kernel of the simulation service is a set of
function pointers into the engine's memory).

- **Loading.** The runtime sends the module host, with the load, each provider of the step's
  services, in the order of the run's services (a provider coming where its first service does):
  its manifest's path and SHA-256 as the runtime read it, its library's path and the SHA-256
  validation computed, and the services the step gets from it. The module host reads the manifest
  again (`MANIFEST_INVALID` when it changed since) and loads and starts the provider with the
  runtime's provider loader, before it loads the module's library: the same checks in the same
  order, with the same codes and messages ([runtime-services.md](runtime-services.md#loading)),
  the library's digest checked before any of its code runs (`LIBRARY_DIGEST_MISMATCH` at
  `/library/path`). An error is located in the provider's manifest, as in the runtime's process. It
  fails the step's load (`details.phase` `load`, naming the step), where a provider in the
  runtime's process fails the run before any step loads, naming no step.
- **One in each module host.** A module host is a process of its own: it loads and starts each of
  its providers once, for its step. A provider whose services several steps use is started in the
  module host of each step in mode `process` that uses it, and once in the runtime's process when a
  step in mode `in-process` uses it too; its `provider.loaded` event, and whatever it logs as it
  starts, come once for each. A provider whose `start()` failed in one module host is started again
  in the next, while in the runtime's process every later load reports its first failure again.
- **The runtime's process** loads only the providers of its own steps (mode `in-process`), with the
  services they use; so does `validate --load`. A provider that only steps in module hosts use is
  never loaded in the runtime's process. The run fingerprint does not change: it is made at
  validation, from the providers' manifests and their libraries' digests, wherever the providers
  then run ([runtime-services.md](runtime-services.md#fingerprint)).
- **Lookups.** Each lookup is served in the module host and reported to the runtime, which records
  it ([Calls and the host API](#calls-and-the-host-api)): the message trace's lookups, their names,
  order and whether a table was provided, are those of the runtime's process.
- **Logs.** A provider's records go to the logger `provider.<name>` of the run's logs (of the
  `Runtime`'s for `validate --load`), as in the runtime's process. The module host records the
  levels that logger records and passes each record on with its level, its attributes (`provider`;
  `event` and `library_sha256` for `provider.loaded`) and its message, and the runtime logs it with
  them: the record is the one the runtime's process would log, with no process id added. Records
  of an engine that its provider routes to its own logger come the same way. A provider of a step
  in a module host starts when that step loads, not when the run starts, so its records come later
  in the run's log.
- **Crashes.** A provider runs in its module host's process: one that ends it, in its `start()`, in
  a service call or on a thread of its own, crashes the module host ([Crashes](#crashes)), with
  `details.phase` `load` when that happens during the load (a provider's `start()` included), or
  the call that was running, or, between calls, the next call.
- **Calls that do not poll.** A service call that does not poll `cancel_requested()`, such as a
  long translation of the provider `antimony`
  ([model-conversion-service.md](model-conversion-service.md#the-antimony-provider)), keeps the
  module's call running. When its deadline and the grace have passed, the module host is killed,
  with the call and the engine in it ([Unresponsive calls](#unresponsive-calls)): nothing of it is
  left, and no lock of that engine is held any more. A step whose module host holds another
  instance of the provider never waits for it. For a step in mode `process`, this answers how long
  one translation may hold the lock of the provider `antimony`
  ([open-questions.md](../open-questions.md), OQ-30): no longer than its call's deadline and grace.
  A call without a deadline, or with a grace of zero, is waited for without limit, as in the
  runtime's process ([time-cancellation-timeouts.md](time-cancellation-timeouts.md#deadlines)): a
  stop then only asks it to cancel, and its module host is not killed for it.

## Shared compiled models

A provider that compiles models (an engine that turns a model text into native code, say) can hand
a model it compiled in one module host to the module hosts of other steps, which load it instead of
compiling it again. A step takes part with `share_compiled_models: true`, and only a step in mode
`process` may (see [Configuration](#configuration)); it is off by default.

- **The store** lives in the runtime's process, in memory only (never on disk), for the rest of the
  process: a second run in the same process finds what the first kept. It is the same for every
  run and `Runtime` of the process.
- **The provider host's callbacks.** A provider reaches the store through two callbacks of its
  `praenora_provider_host`, appended after `log_enabled`
  ([runtime-services.md](runtime-services.md#provider-interface)): `load_compiled(context, key,
  key_size, out)` writes the entry under `key` to the sink `out` and returns 1, or returns 0 when
  the store does not hold it; `save_compiled(context, key, key_size, data, size)` keeps `data` under
  `key`. Both are NULL where the process offers no store: always in the runtime's own process (a
  provider there keeps its own caches), and in a module host whose step does not share. A provider
  checks `struct_size` before it reads them. The key is the provider's own, made of what decides
  the compiled model (the text, the options, the engine), never of anything of its process.
- **Namespaces.** Entries are kept by the provider's name and its library's SHA-256, and the key.
  A request names its provider by its index in the module host's load; the runtime takes the name
  and the SHA-256 from its own load request, never from the module host. A module host can neither
  read nor write the entries of another provider, or of another library of the same provider.
- **Limits.** A key takes at most 256 bytes; an entry at most 128 MiB (134217728 bytes); keys and
  entries together at most 512 MiB (536870912 bytes). A save that would go beyond the total
  evicts the least recently used entries first, until it fits; a load that finds an entry, and a
  save, count as uses. A save under a key replaces its entry.
- **Refusals.** A module host refuses a save whose entry or key is over its limit itself: it sends
  none of its bytes, only a notice with their sizes and the key's first 256 bytes, and the runtime
  logs `compiled_models.refused`. A load under a key over the limit finds nothing, without asking
  the runtime (and without an event).
- **When.** The runtime answers a module host's requests at any time: during its load (a provider's
  `start()` may use the store), during a call, and between calls (from a provider's own thread).
  A load waits for its answer. What a call saved reaches the store before the call's result.
- **Events** (debug, on the logger `module.<step>` of the step whose module host asked, with the
  attributes `module`, `pid`, `provider` and `key`): `compiled_models.hit` (with `size`),
  `compiled_models.miss`, `compiled_models.saved` (with `size` and `evicted`, the entries evicted
  for it) and `compiled_models.refused` (with `key_size` and `size`). The provider says, at debug
  through the calling instance's host, whether it compiled a model or loaded it from the store.
- **Protocol.** A store request from the module host of a step that does not share, one that names
  no provider of the load, one whose key or entry is over its limit, and a notice that refuses an
  entry within the limits, break the channel's protocol ([Crashes](#crashes), reason `protocol`).
  So does a module host that does not take the answer to its request within 30 seconds (it stopped
  reading its channel): the runtime never waits longer for it, and the module host is killed.
- **Providers that use it.** The provider `roadrunner` keeps a model's saved state
  ([simulation-service.md](simulation-service.md#the-roadrunner-provider)): it keys an entry by the
  key of its own cache of compiled models, and looks in that cache first. The test provider
  `test-simulation` keeps its canonical parsed model under `test-decay:` and the FNV-1a hash of the
  model's text; it has no cache, and asks the store at every session it opens. Both compile a
  model whose entry does not load, and keep the model compiled; a warning through the calling
  instance's host says so, and so does one when a model they compiled cannot be kept.
- **Trust.** The store hands bytes from one module host to another. A module host whose step
  shares may give every other sharing step of the process an entry of its choosing, for any key of
  its provider's namespace; for `roadrunner` that is compiled native code, which the loading
  module host runs. A provider's checks of an entry (the `roadrunner` envelope's engine identity,
  length and SHA-256) find an entry that is corrupt or cut short, not one made on purpose. Share
  compiled models only between steps whose modules are trusted alike, as code in one process
  would be; a step that does not share never reads the store.

## The restricted profile

A step in mode `process` with `sandbox: "restricted"` has its module host apply the restricted
sandbox profile to itself, for good, before it loads any provider or module library (macOS only in
this version). The runtime sends the module host the profile's name and its temporary directory;
the module host makes the profile's text with the runtime's own code and applies it with
`sandbox_init()`.

- **Refused**, with `EPERM`: the network (`network*`: connecting, listening, sending to an
  address); new processes (`process-fork`, `process-exec`: `fork()`, `posix_spawn()`, `exec*()`),
  and the system's other ways to start one for the process (`job-creation`: a launchd job;
  `lsopen`: LaunchServices opening an application; `appleevent-send`: scripting another
  application); and writing files (`file-write*`: creating, changing, renaming or removing)
  anywhere but below the module host's temporary directory and to `/dev/null`. A file outside is
  not written through a link either: a hard link to it cannot be made in the directory, and a
  symbolic link to it can be made there but not written through. Opening `/dev/stdout`,
  `/dev/stderr`, `/dev/tty` or `/dev/fd/<n>` by its path for writing is refused too.
- **Allowed**, as without a profile: reading files, loading libraries (`dlopen()`), threads, memory
  mapped for code generation (`MAP_JIT`) and memory made executable with `mprotect()` (a JIT's code,
  such as LLVM's MCJit in the provider `roadrunner`), links of every kind within the directory,
  and the descriptors the module host already has: its channel (so its records, outputs, lookups
  and the shared compiled models travel as without a profile), and the runtime's standard output
  and error, written through descriptors 1 and 2.
- **What it is, and what it is not.** The profile limits what the module host's own process does:
  it keeps a module that keeps to its documents from reaching the network, starting programs or
  changing files by accident or by a library's habit. It is not a boundary against a module meant
  to do harm: `(allow default)` leaves the module the system services it can reach over Mach IPC,
  and such a service acts for it with its own rights.
- **The temporary directory** is the module host's own: the runtime makes it, for that module host
  only (`praenora-module-host-XXXXXX`, for its owner only), below the real path of the system's
  temporary directory (symlinks resolved: `/var` is `/private/var` on macOS). The module host's
  `TMPDIR` names it, and so does `std::filesystem::temp_directory_path()`. Once the module host has
  ended, the runtime removes it with everything in it: it first gives itself its rights back on
  every directory in it (a module may have taken them away), never following a symbolic link. What
  cannot be removed even then (a file the module made immutable, say) is left, with a warning
  (event `module_host.directory_left`, naming it). A runtime that is killed removes nothing: its
  module hosts' directories stay until the system cleans its temporary directory. A module host
  without a profile keeps the temporary directory it inherits.
- **Failure.** A profile that cannot be applied, or a temporary directory that cannot be made,
  fails the step's load with `MODULE_HOST_UNAVAILABLE`, `details.reason` `sandbox`, and the
  system's text in `details.system`, located in the module's manifest. (macOS's sandbox library
  writes a line of its own to standard error when it fails; the module host keeps it from the
  runtime's.) On a platform without the profile, validation refuses the step
  ([Configuration](#configuration)).
- **Nested sandboxes.** A process that runs in a sandbox already cannot apply another one: macOS
  refuses it (`EPERM`), and a module host inherits the sandbox of the runtime that started it. A
  runtime that runs in a sandbox (another tool's, or a test environment's) therefore cannot run a
  step under the profile: its load fails with `MODULE_HOST_UNAVAILABLE`, reason `sandbox`,
  "Operation not permitted". The tests of the profile skip there
  ([testing.md](../testing.md#module-hosts)).
- **Results** do not change: a module that does nothing the profile refuses gives the same
  outputs, records and fingerprint under it as without it; the provider `roadrunner` needs no write
  under it (it compiles in memory, and loads the shared compiled models from its channel).
- **Signed module hosts** (not verified): a module host signed with the hardened runtime needs the
  entitlements `com.apple.security.cs.allow-jit` (or
  `com.apple.security.cs.allow-unsigned-executable-memory`) for code generation and
  `com.apple.security.cs.disable-library-validation` to load modules and providers signed otherwise
  ([platforms.md](../platforms.md#macos)).

## Crashes

When the module host ends (a signal, or any exit) while a load or a call is outstanding, or breaks
the channel's protocol, the call's outcome is *crashed*. The runtime notices the end of the
channel, and, while a request waits, also looks at the module host's process every few
milliseconds: a process the module started may hold the channel open after the module host has
ended (a forked child).

- the error is `MODULE_CRASHED` (706) with `details` `{"phase", "signal", "signal_name"}`,
  `{"phase", "exit_code"}`, `{"phase", "reason": "protocol"}`, or `{"phase", "reason": "ended"}`
  when the system cannot say how it ended (its status was taken by someone else, as when children
  are reaped automatically); `phase` is the call or `load`; a module host that ended between calls
  is reported by the next call, with that call's name;
- the step's state becomes `crashed` (status snapshots, the CLI and Python show it), and the
  instance is never called again: no `finalize`, `destroy` or `diagnostics` reaches the channel;
- the module host, and whatever was left in its process group, have been ended and waited for by
  the time the call returns, and the event `module_host.crashed` (error) names its process id and
  how it ended. A module host that crashes just as the call's grace ends is reported as crashed,
  not as unresponsive.

Under sanitizers a crash may end the module host with another signal (AddressSanitizer on macOS
aborts after its report) or with an exit code instead of its signal.

## Unresponsive calls

When a call outlives its deadline plus the cancellation grace, the runtime kills the module host's
process group (`SIGKILL`) and waits for it; the call returns at once. The outcome is
*unresponsive* (`MODULE_UNRESPONSIVE` with `details.terminated: true`) and the step's state
`unresponsive`, as in the runtime's process; but nothing is left behind: no thread is detached, no
library is pinned or kept, no lease is left abandoned, and the instance's host context is released.
The same library can be loaded and used again at once, in a module host or in the runtime's
process. The event `module_host.terminated` (warn) names the module host.

## Ending

After a successful `destroy`, the runtime asks the module host to exit; it flushes its standard
streams and exits. The runtime writes the request, and waits for the exit, within the lifecycle
timeout, and kills the module host after that (event `module_host.terminated`; "did not take the
request to exit" when the request could not even be written; what the module host still had
buffered is then lost); otherwise the event is `module_host.exited` (debug). Without a lifecycle
timeout (`timeouts.lifecycle: none`) it waits for the exit without limit, as it waits for the
`destroy` itself. A module host that breaks the channel's protocol while it is being ended (it did
not take an answer of the store) is killed at once, with a warning that names the problem
(`module_host.terminated`, "broke the channel's protocol while it was being ended"). A module host
whose instance was never created exits when the runtime lets go of the module, within 5 seconds.

## Determinism

Results do not depend on where a module runs, on whether its module host shares compiled models,
or on its sandbox profile (for a module that does nothing the profile refuses): a call gives the
same outputs, error documents, sink refusals and service lookups, and the run the same message
trace and fingerprint, in a module host as in the runtime's process, sharing or not, restricted or
not.

Log records are the same too, but for the module host's own events (`module_host.started`,
`module_host.exited`, `module_host.terminated`, `module_host.crashed` and
`module_host.directory_left`), which only a step in a module host has, and for those that report
what a process did before: a provider logs its start once in each process
([One in each module host](#service-providers)), so the runtime's process has those records only
for the first run that starts it, and each module host always; a provider's cache of compiled
models is its process's, so the debug record of a call that finds a model compiled before
(`roadrunner`'s "the model was compiled before") follows that history; and a module host that
shares compiled models logs the store's events, and its provider says whether it compiled a model
or loaded it, as the store's content at that moment decides.

The other exceptions are at limits no real module or provider reaches: a log message longer than
1 MiB is cut to 1 MiB (a module's or a provider's); a provider's record whose attributes take more
than 64 KiB as JSON is passed on without them; and a service name longer than 4 KiB in a lookup is
cut to 4 KiB. A name that long never names a service, so the lookup's result is the same; the
message trace records it cut.

## Stops

A stop (`request_stop()`, Ctrl+C, SIGTERM) reaches the call in flight through a cancellation frame.
A terminal's Ctrl+C does not reach the module host, which has a process group of its own. A stop is
never a crash.

A stop that comes while the runtime still writes a call's document that the module host has not
taken (it stopped reading its channel, or takes it slowly) cannot be passed on: the module host,
which has not begun the call, is killed within a slice of 5 ms (event `module_host.terminated`),
and the call ends cancelled, so the run ends as `stopped`. The module's instance went with the
module host: the teardown calls neither its `finalize` nor its `destroy`, and reports no failure;
a warning (event `teardown.skipped`, with the step, the module and the instance's `state`) says
that it "is neither finalized nor destroyed: its module host was ended when a stop came before it
had taken a call", the step ends `destroyed`, and the run's health does not change for it.

A load has no timeout of its own, in a module host as in the runtime's process. A stop that comes
while a module host starts (its handshake) or loads the step's module, for example in a static
initializer that never returns, kills that module host and waits for it (event
`module_host.terminated`, with its `module` and `pid`; a module host stopped during its handshake
was never announced as started, so no `module_host.started` comes before it); the run then ends
as `stopped` (`RUN_STOPPED`). A stop requested before the step's load starts no module host.

## Platforms

| Platform | Process isolation | The restricted profile |
|---|---|---|
| macOS | Available; verified on arm64. | Available (`sandbox_init()`), outside another sandbox. |
| Linux | Available; its tests pass on x86-64 in CI ([releasing.md](../releasing.md#what-the-runs-verified)), though none there checks the closing of descriptors, which is tested on macOS only. It uses `PR_SET_PDEATHSIG` (module hosts are started from one thread of the runtime's own, which lasts as long as the process), `MSG_NOSIGNAL`, bounded writes sent with `MSG_DONTWAIT`, and the module host closes the descriptors above 3 itself. | Not available in this version: validation refuses it (`UNSUPPORTED`); loading under it fails with `MODULE_HOST_UNAVAILABLE`, `details.reason` `sandbox`. |
| Windows | Not available in this version: validation refuses a step in mode `process` (`UNSUPPORTED`); loading in a module host fails with `MODULE_HOST_UNAVAILABLE`, `details.reason` `spawn`; and the module host is not built. | Not available. |

Process isolation and a restricted profile on Linux and Windows are an open question
([open-questions.md](../open-questions.md), OQ-31).

## Limits

| What | Limit |
|---|---|
| Module hosts | One per module instance: one per step in mode `process`. |
| A sink write passed on | In chunks of at most 1 MiB; a longer write travels in several. |
| A log message | 1 MiB; a longer one is cut. |
| A provider's log record's attributes | 64 KiB as JSON; a record with longer ones goes without them. |
| A service name in a lookup | 4 KiB; a longer one is cut. |
| The load's answer | 4 MiB; a load error beyond it is shortened, without its details. |
| The handshake | 30 seconds. |
| A key of the shared compiled models | 256 bytes; a save under a longer one is refused in the module host. |
| An entry of the shared compiled models | 128 MiB (134217728 bytes); a larger one is refused in the module host. |
| The shared compiled models | 512 MiB (536870912 bytes) of keys and entries, least recently used first out. |
| The answer to a store request | Taken by the module host within 30 seconds, or it is killed. |

## References

- [ADR-0037](../adr/ADR-0037-process-isolation-of-module-instances.md): process isolation of
  module instances.
- [ADR-0038](../adr/ADR-0038-shared-compiled-models-between-module-hosts.md): shared compiled
  models between module hosts.
- [ADR-0039](../adr/ADR-0039-restricted-sandbox-profile-for-module-hosts.md): a restricted sandbox
  profile for module hosts.
- [ADR-0006](../adr/ADR-0006-threading-model.md): the threading model.
- [threading-and-ownership.md](threading-and-ownership.md),
  [time-cancellation-timeouts.md](time-cancellation-timeouts.md),
  [error-taxonomy.md](error-taxonomy.md), [observability.md](observability.md).
- [open-questions.md](../open-questions.md): OQ-5 (resolved by ADR-0037), OQ-13 (the
  Python wheel holds no module host), OQ-19 (trust), OQ-30 (answered here for a step in a module
  host), OQ-31 (Linux and Windows), OQ-32 (the contract checks).
