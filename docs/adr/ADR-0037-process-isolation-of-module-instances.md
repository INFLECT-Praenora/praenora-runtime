# ADR-0037: Process isolation of module instances

- Status: Accepted (scope noted in Decision)
- Date: 2026-11-19
- Supersedes: none (amends [ADR-0006](ADR-0006-threading-model.md))
- Superseded by: none

## Context

Modules are arbitrary native code that, until now, run in the runtime's process
([ADR-0006](ADR-0006-threading-model.md)): a segmentation fault, an `abort()` or an `exit()` in a
module ends the runtime, and a call that never returns and never polls `cancel_requested()` can only
be abandoned. An abandoned call keeps its thread running, its library pinned and its host context
alive for the rest of the process, and every later call or load of that library that would have to
wait for it is refused ([ADR-0030](ADR-0030-library-gate-across-runs.md)). Heavy engines
(libRoadRunner, libAntimony) make both more likely: an LLVM fatal error ends the process, and a
long translation does not poll. OQ-5 asked whether module instances should run in processes of
their own.

Modules must not change: ABI v0, the SDK, the protocol and every document a module sees stay as
they are, and results must not depend on where a module runs.

## Decision

Status split: the answer to OQ-5 is Accepted, by the maintainer on 2026-11-22; it resolves OQ-5.
That answer is: a module instance can run in a module host, a process of its own that serves
exactly one instance (the first sentence of **Isolation at the level of the module instance**);
**Crashes become data**; **A call that never returns is killed**; **Chosen per step**; **Providers
with their module**; and **Amendment of ADR-0006**. Proposed: how the runtime starts a module host
(the rest of that first bullet), **Where it is**, **The channel**, **A code seam in the loader**,
**No module host is left behind**, **A module host that cannot be used**, **Platforms** (OQ-31),
and the builder notes.

- **Isolation at the level of the module instance.** A module instance can run in a process of its
  own, a *module host*: the executable `praenora-module-host`, which holds the runtime's own code
  (it links the runtime's objects) and serves exactly one instance. The runtime starts it with
  `posix_spawn()` (never `fork()` of its own process), with a private channel (a socket pair) as its
  descriptor 3, `/dev/null` as standard input, the runtime's standard output and error, and a
  process group of its own.
- **Where it is.** `<prefix>/libexec/praenora/praenora-module-host` in an installation, the same
  place relative to the runtime library in a build tree; the runtime finds it relative to its own
  binary, never through an environment variable.
- **The channel** is an internal protocol of one runtime build: framed messages with per-kind
  limits, checked before anything is allocated; a handshake refuses a module host of another
  runtime version or channel protocol. Every frame from a module host is untrusted.
- **A code seam in the loader.** A loaded module holds its code behind an internal interface: its
  function table in the runtime's process, or its module host. The module instance keeps its state
  machine, deadlines, stop flags, logging and lookup recording either way, and the contract checks
  keep running modules in the runtime's process (OQ-32). The runtime's process never opens the
  library of a module that runs in a module host; the module host loads it with the loader's own
  checks, codes and messages.
- **Crashes become data.** A module host that ends (a signal, any exit) or breaks the protocol
  during a load or a call gives the new outcome *crashed*: error `MODULE_CRASHED` (706) with the
  phase and the signal, exit code or reason `protocol` (`ended` when the system cannot say how), and
  the new step state `crashed`; the instance is never called again.
- **A call that never returns is killed.** After its deadline and the cancellation grace, the
  runtime kills the module host's process group and waits for it: `MODULE_UNRESPONSIVE` with
  `details.terminated`, and nothing is detached, pinned, kept or left abandoned, so the library can
  be used again at once.
- **No module host is left behind.** The module host ends when its channel ends (the runtime is
  gone); on Linux it also asks for `SIGKILL` on its parent's death. The runtime waits for every
  module host it starts, and kills what the module host left in its process group.
- **A module host that cannot be used** fails the load with `MODULE_HOST_UNAVAILABLE` (503), with
  `details.reason` `missing`, `spawn`, `handshake` or `build`, and `sandbox` for a restricted
  sandbox profile that cannot be applied
  ([ADR-0039](ADR-0039-restricted-sandbox-profile-for-module-hosts.md)).
- **Chosen per step.** The workflow configuration's `isolation.mode` (`in-process`, the default, or
  `process`), at `run.isolation` for every step and at `steps.<id>.isolation` for one, resolved
  field by field; a caller may set every step's mode for one run (`WorkflowOptions::isolation`,
  `praenora run --isolation`, Python's `isolation=`). The engine starts one module host per step in
  process mode, never shared; `validate --load` loads such a step's module in a module host too.
  The mode is not part of the run fingerprint, since results do not depend on it; the run record's
  `execution.isolation` lists every step's.
- **Providers with their module.** The service providers of a step's module are loaded and started
  in the step's module host, with the runtime's provider loader, before the module: a service's
  tables and the engine behind them (a simulation kernel is function pointers into it) live in the
  process of the module that calls them, so an engine's crash ends only that step and a long engine
  call ends with its module host. The runtime's process loads only the providers of its own steps.
  Lookups, the message trace, the providers' log records and the fingerprint are those of the
  runtime's process.
- **Platforms.** POSIX. On Windows process isolation is not available in this version: validation
  refuses a step in process mode (`UNSUPPORTED`), the interface refuses (`MODULE_HOST_UNAVAILABLE`,
  reason `spawn`) and the module host is not built (OQ-31).
- **Amendment of ADR-0006.** Its "modules run in-process" becomes the default, `in-process`, no
  longer the only way. An instance in a module host keeps ADR-0006's rules there: one thread makes
  its calls, one at a time, and its host callbacks are thread-safe; its crash ends only its module
  host, and a call of it that never returns is killed instead of abandoned. OQ-5 is resolved.

The specification is [process-isolation.md](../specs/process-isolation.md).

### Builder notes (Proposed)

- The seam is `ModuleCode` (`src/runtime/loader/module_code.hpp`), owned by `LoadedModule`: the
  in-process implementation calls the function table; `ChildModule`
  (`src/runtime/isolation/child_module.hpp`) forwards each call over the channel. The instance's
  host context implements a second interface, `InstanceHost`, through which the runtime's side of
  the channel hands over what the module does in the module host (log records, lookups) as the
  same calls on the in-process host API would be handled, and asks whether the call in flight has
  a cancellation. `LoadedModule::api()` stays the function table, valid only for a module loaded
  in the runtime's process (the contract checks use it); every function is NULL for one in a
  module host.
- The limits of a call's sinks travel with each call, taken from the instance's own limit
  (`InstanceOptions::max_output_bytes`), instead of once with the load. A module therefore meets
  exactly the instance's limit, in either place, whatever the module host was started with.
- The lowest log level the instance's logger records travels with each call, not separately when
  it changes: a change between calls reaches the module host with the next call, and until then
  the module host filters with the old level (the runtime filters again what it receives).
- Cancellation is passed on by the thread that waits for the call's result: it looks every 5 ms
  with a timed condition wait, since whoever raises the flag (a signal handler) may not be able to
  wake it; the instance's `request_cancel()` stays lock-free and async-signal-safe. A stop
  requested before the call starts travels with the call, so the module sees it from its first
  poll, as in the runtime's process.
- A crash noticed between calls (a module thread ended the process) is reported by the next call,
  with that call as the phase. A module host whose channel ends while its process keeps running
  (it closed its descriptor 3) is killed after five seconds and reported with reason `protocol`.
  A module host whose exit status the system cannot give (taken by someone else) is reported with
  reason `ended`.
- Processes a module starts must not hide its crash or outlive it. The module host makes its
  descriptor 3 close-on-exec first thing (the spawn passes it on without), so no program the
  module runs inherits the channel; a request that waits also looks at the module host's process
  every 5 ms, and a process that has ended is a crash after 100 ms (time for the reader to take
  what it sent) even while a forked child holds the channel open; and when any child ends, the
  runtime sees it with `waitid(WNOWAIT)`, kills what is left in its process group while the
  unreaped child still holds the group's number, and only then reaps it.
- A failed write of a request (the load or a call) ends the connection as the end of the stream
  does: the module host gets five seconds to end, and its status is the crash's.
- A call whose grace ends while its module host's crash is being recorded is reported as crashed,
  not as unresponsive.
- The module host checks that the manifest file is the one the runtime read (its SHA-256), and
  refuses a changed one with `MANIFEST_INVALID`. The runtime checks the `describe()` document it
  receives again against the schema and the manifest; a module host whose answer fails that broke
  the protocol.
- The module host serves one instance: a second `create` (another instance over the same loaded
  module) is refused in the runtime, before anything is sent, and the first instance keeps the
  module host; calls into a module in a module host take an exclusive lease on a gate of its own,
  shared with nothing.
- After `destroy`, the runtime sends the request to exit and waits during the instance's lifecycle
  timeout, without limit when it has none (`timeouts.lifecycle: none`, as for the `destroy`
  itself); the module host flushes its standard streams (`fflush(NULL)`) and ends with `_Exit(0)`.
  A module thread that holds the lock of standard output makes the flush block; the runtime then
  kills the module host when the wait ends. A module host whose instance never existed is ended
  when its loaded module goes away, with a wait of 5 seconds. The handshake waits 30 seconds.
- Killing signals the module host's process first and then its process group: signalling a group
  is not one atomic step, and a process that sees a member of its group die first could still exit
  on its own.
- The module host's calls all run on one thread, made as the runtime makes an instance's worker
  thread (a `std::thread` with the platform's default stack), not on the main thread, whose stack
  is larger: a module has the same stack in either place. Its load (static initializers,
  `describe()`, the providers' `start()`) runs on a thread of its own, which that thread starts
  and waits for, with a main thread's stack: the soft limit of `RLIMIT_STACK`, at least 1 MiB and
  at most 1 GiB in whole pages, or 8 MiB when the limit is unlimited or cannot be read. In the
  runtime's process a load runs on its caller's thread, the main thread of the tool and of Python;
  on macOS a thread's default stack is 512 KiB, so a static initializer that needs more crashed
  only in a module host before. A reader thread takes the runtime's frames, raises the
  cancellation flag of a call, and ends the process on the request to exit or at the end of the
  channel.
- An executable that exists but cannot be run is reported with `details.reason` `spawn` ("is not
  executable", with the system's text).
- The digest check of "the library changed after validation" is one check with one message in
  both places: the engine loads a module in its own process with `LoadedModule::load()` and
  validation's SHA-256, which refuses a changed library before any of its code runs, as a module
  host does; the words follow the provider loader's.
- `ChildSettings` and `describe_crash()` are in `src/runtime/isolation/child_settings.hpp`, so the
  loader and the engine depend on nothing else of the module host's side.
- The install destination is computed as `${CMAKE_INSTALL_LIBDIR}/praenora/../../libexec/praenora`,
  so the executable stays where the runtime looks for it: `libexec/praenora` for the usual `lib`.
  With a `CMAKE_INSTALL_LIBDIR` of more than one component (Debian's multiarch directories) it lands
  in `lib/libexec/praenora` (not verified).
- A missing module host is tested through an internal hook,
  `set_module_host_directory_for_testing()`, never an environment variable.
- The fixture module `fx-hang` gets the parameter `without_gate: "block"`: a module host offers no
  built-in services, so it blocks for good without the test service `test.gate`.
- The engine maps the outcome *crashed* to `MODULE_CRASHED`, keeps `details.terminated` of an
  unresponsive call in a module host, and skips the teardown of a crashed instance; metrics count a
  crashed call as failed, and a crashed step makes the run's health `unhealthy`.
- On Linux the parent-death signal is sent when the thread that started the child ends, not the
  process: there the runtime starts module hosts from one thread of its own that lasts as long as
  the process, so a module host does not die with a short-lived thread of the caller. Elsewhere
  (macOS) no such thread exists: `posix_spawn()` runs on the thread that loads the step's module.
- Linux code (`PR_SET_PDEATHSIG` with the check of `getppid()`, `MSG_NOSIGNAL`, bounded sends
  with `MSG_DONTWAIT`, closing the descriptors above 3 with `close_range`, `/proc/self/fd` or a
  loop) is written with care but not compiled in this version; Windows has the refusing interface
  only, not compiled either. Members and helpers that one platform does not use are declared only
  where they are used (the channel's send chunk on macOS, its system-text helper on POSIX), so
  that Clang's `-Wunused-private-field` and MSVC's `/W4` find none.
- The configuration's `isolation` object has the fields `mode`, `share_compiled_models`
  ([ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md)) and `sandbox`
  ([ADR-0039](ADR-0039-restricted-sandbox-profile-for-module-hosts.md)); its schema admits no
  other. The resolved mode keeps where it came from (the JSON pointer of the field that
  set it, or the caller's override), so validation's refusals are located at that field; a mode
  that the caller's override set has no field, and its refusal no location, and says so in words
  that fit every front end ("the run's isolation override": `--isolation`,
  `WorkflowOptions::isolation` or Python's `isolation=`).
- The load request carries the step's providers, each with its manifest's path and SHA-256 (as
  the runtime read it), its library's path and validation's SHA-256, the services the step gets
  from it and the lowest level the runtime's logger of it records; the module host reads each
  manifest again and refuses one that changed (`MANIFEST_INVALID`), then loads and starts the
  providers in the order of the run's services, then the module. A load error names the provider
  by its index in the request, and the runtime locates it in that provider's manifest from what it
  sent: a module host never names a file. Only the step's services are loaded from a provider (in
  the runtime's process, only those of its own steps), and only they are served.
- A provider's log records cross the channel as a frame of their own: the provider's index, the
  level, the record's attributes (the JSON array `[[key, value], ...]`, in their order) and the
  message. In the module host each provider gets logs of their own at the level the runtime's
  logger records, whose one sink takes each event apart again (`decode_logged_event()`) and sends
  it; the runtime checks the frame and logs it on the logger it gave the provider with the same
  attributes. So the records are those of the runtime's process, `provider.loaded` with its
  `library_sha256` included, and carry no process id. A message over 1 MiB is cut, and attributes
  over 64 KiB as JSON are left out, at limits no provider reaches.
- Each module host starts its own instance of each of its providers: `provider.loaded`, and what a
  provider logs as it starts, come once for each module host, and a `start()` that failed in one
  module host is tried again in the next (the runtime's process reports its first failure again).
  A provider of a step in a module host starts when the step loads, not when the run starts; its
  load error fails the step's load and names the step, where the runtime's own providers fail the
  run naming none.
- `validate --load` makes the same split: the providers of the steps in mode `in-process` in this
  process, reported for the first such step that uses one; those of a step in mode `process` in its
  module host, before its module, reported for that step.
- The check that the runtime's process loads no provider that only steps in module hosts use goes
  through a hook of the process-wide provider registry, `opened_providers_for_testing()` (every
  provider whose library opened in the process, whatever became of it), never an environment
  variable.
- The built-in services of a `Runtime` (test services such as `test.gate`) stay out of module
  hosts: they are objects of the runtime's process. The module developer guide says so.
- The first to see the end of a module host decides how it ended: a protocol violation found by the
  thread that reads the load's answer kills the module host, and the reader, which then sees the
  end of the stream, no longer records the kill (`SIGKILL`) as the crash; the crash's reason stays
  `protocol`. A thread that comes second waits until the first has said how it ended.
- A provider that ends its module host is that module host's crash, with the phase `load` during
  the load (its `start()` included) or the call that was running; the fixture provider
  `test-start-crashes` ends its process in `start()`, and only module hosts load it. The fake
  module host of the tests answers a load with a provider's record, or a load error, that names a
  provider the load did not have, which the runtime refuses as a protocol violation.
- The tests of providers in module hosts pass when one process runs them all, and repeated, as the
  other provider tests do: a comparison with the runtime's process leaves out each provider's
  records of its start (up to its `provider.loaded` event), which that process has only for a
  provider no earlier test started, and the tests check them in the module host's log; a start
  failure that the runtime's process reports again is compared by the message of the first; a
  test of what a load opens in the runtime's process compares `opened_providers_for_testing()`
  before and after it, and the one that needs two providers not open yet is skipped, saying so,
  when they are.
- The fixture provider `test-log-levels` says at warn whether `log_enabled()` reports info as
  recorded, so the level the module host gives a provider's logs is seen, not only the records the
  runtime keeps; `test-echo-crashes` ends its process in `echo()`, at once or after the call
  returned, from a thread of its own, for a provider that crashes during a call and between calls.
  The thread waits for a file the test makes after the call returned
  (`PRAENORA_TEST_ECHO_CRASH_AFTER`), and the test makes its next call only once the runtime has
  logged the crash (`module_host.crashed`): the crash is then between calls, and its record says so,
  whatever the machine's speed. (A fixed delay let a slow machine see the crash during the call, or
  a next call that began while the runtime was recording the crash.)
- A long translation of the provider `antimony` in a module host is ended with its module host when
  its deadline and grace pass, and the next step's conversion does not wait for it
  ([model-conversion-service.md](../specs/model-conversion-service.md#the-antimony-provider)). The
  test uses a generated chain of 6000 reactions, which takes libAntimony about 19 s.
- The run record's `execution.isolation` is required in the `run-record` 0.2 schema, changed in
  place, and lists every step with every field resolved; `execution.overrides.isolation` holds the
  caller's override by its name, as the seed and the ticks are listed.
- The module host's events go to the step's logger, `module.<step>`, beside the instance's other
  events; a step's `log_level` filters them as it filters the module's records.
- A module host that crashes while it loads the module makes its step `crashed`, although the step
  has no instance yet: the step's state in the status snapshot says what ended it, as for a crash
  during a call.
- A module host that crashes in the teardown's `destroy` is recorded as one that crashes in its
  `finalize`: the step's last error `MODULE_CRASHED` with `details.phase` `destroy` and how it
  ended, the warning `teardown.destroy_failed`, the step marked failed (shown `crashed`); the
  run's ending stays as it was and its health is `unhealthy`. In the runtime's process a `destroy`
  cannot crash (the process would end). A `destroy` that does not return within its deadline plus
  the cancellation grace is recorded as such a `finalize` is, in both places: the step's last
  error `MODULE_UNRESPONSIVE` with `details.phase` `destroy` (and `details.terminated` in a module
  host, which is killed) and the warning `teardown.destroy_failed`, since the step's state alone
  named no call and the run record named nothing. Every error the teardown records, a
  `finalize`'s too, names its call in `details.phase`, as a run failure's details do. A `destroy`
  that returns after its deadline, within the grace, still sets no last error: it has freed its
  instance, and the warning `call.deadline` says it was late (such a `finalize` is
  `STEP_TIMEOUT`).
- A stop reaches a module host's start as it reaches a call: `ChildSettings::stop_flag`, the run's
  stop flag, is looked at every 5 ms, during the handshake (waiting until the channel is readable,
  so a frame is never cut by a short read) and during the load. Once it is raised, the module host
  is killed and waited for (event `module_host.terminated`, also during the handshake, where no
  `module_host.started` came before it: a module host is announced as started only once it has
  answered the handshake), and the load fails with `CANCELLED`, which the engine turns into a run
  that ends as stopped; a stop raised before the start starts no module host. No load gets a timeout
  of its own, as in the runtime's process: a static initializer that never returns holds a run until
  it is stopped.
- `validate --load` loads each module once for each place its steps run (in this process for the
  first step in mode `in-process`; in a module host for the first step in mode `process` of each
  set of isolation settings, the sandbox profile and whether it shares compiled models, so that a
  load that fails only under one step's settings, such as the restricted profile's, is reported
  for that step; with the first step's settings only, it went unreported until the run), and in
  this process, too, with validation's SHA-256 checked before any of the library's code runs
  (`LoadedModule::load()` with the expected digest), as a run does: "the library changed after
  validation" has one check and one message in both places, where it used to be compared after the
  load ("changed during validation").
- The front ends take the isolation modes exactly as the configuration writes them (`in-process`,
  `process`), from one name table that the tool and the Python extension share; log levels and
  formats are taken in any case, but these names are the configuration's.
- The Python wheel holds no module host: in a wheel installation a step in process mode fails to
  load with `MODULE_HOST_UNAVAILABLE`, `details.reason` `missing`; the package in a build tree uses
  the build tree's runtime library, and finds its module host beside it.
- The performance probe takes `--isolation in-process|process`, and the perf test
  `perf.process_isolation` (POSIX only) runs it with every step in a module host; the install test
  runs a step in the installation's module host with the installed tool, before and after the
  installation is moved, and checks that the module host is installed and executable.
- Tests that make module hosts crash share `OwnSignals` (`tests/support/scoped_environment.hpp`),
  so that the integration tests can use it; the fixture module `fx-crash` gets `skip_invocations`,
  for a crash in a later tick, and `fail_invoke`, an invoke that fails so that a run asks the
  instance for its diagnostics (the only way a run calls diagnostics), and `fx-hang-at-load` blocks
  for good in a static initializer, for a stop during a load. A run-level crash is tested in each
  call (create, initialize, invoke, finalize, destroy, diagnostics), and in each way (signal,
  abort, exit) in invoke: a crash in the teardown leaves the run's ending as it was, the step
  `crashed` and the run's health unhealthy. A test of Ctrl+C makes its process lead a process
  group of its own (and returns to its group at the end) and signals that group, as a terminal
  does, and checks that the run ends as stopped, its call cancelled in the module host, not
  crashed.
- The CLI tests' driver checks values in a JSON file too (`EXPECT_FILE_JSON_PATH`,
  `EXPECT_FILE_JSON_VALUE`, on `EXPECT_FILE`), so that `cli.run.isolation_process` checks that the
  run record says each step ran in a module host.
- Every deadline of the isolation and instance code is a clock reading plus a wait made with
  saturating arithmetic (`deadline_after()`, `src/runtime/core/deadline.hpp`): a wait beyond the
  clock's range gives its last instant, never an overflow. Without a lifecycle timeout the engine
  passes the longest duration to the module host's end, and adding it to the clock overflowed
  (undefined behaviour; in practice a deadline long past), so every module host of such a step was
  killed at the end of the run instead of being asked to exit, with a misleading warning, and what
  it had buffered on standard output was lost. A deadline at the clock's last instant is no
  deadline: the channel's write and the wait for the exit then wait without limit, as the
  `destroy` before them does.
- A module host ignores `SIGTTOU` and `SIGTTIN` (`SIG_IGN`), first thing, in its own process only:
  as a process group of its own it is a background group of the runtime's terminal, and a write to
  the terminal under `stty tostop`, a `tcsetattr()` by the module or a library, or a read from it
  would otherwise stop it, and the call with it, which the same module makes unhindered in the
  runtime's process. The processes a module starts inherit the ignored signals. The test sends
  both signals by hand to a module host before its calls (before, macOS's `waitid()` reported the
  stopped module host as ended, and the runtime killed it).
- macOS's `waitid(WEXITED | WNOHANG | WNOWAIT)` reports a stopped child as well (`CLD_STOPPED`),
  which the runtime took for an end: it killed a stopped module host and reported a crash by
  `SIGKILL`. A child whose report is `CLD_STOPPED`, `CLD_TRAPPED` or `CLD_CONTINUED` has not ended
  now; a call into a stopped module host waits as for a module that does not return, and its
  deadline and grace end it. Found by the test of the terminal's stop signals; tested with a
  stopped and continued child (`ChildProcess.AStoppedChildHasNotEnded`).
- A module host's stdio buffers are its own: what a module writes through `stdout` reaches the
  runtime's standard output when the module host flushes it, at the latest at its requested exit,
  and is lost when it is killed or crashes. The specification and the module developer guide say
  so, and advise flushing, standard error, or the host API's log; nothing in the runtime changes
  it (a module host cannot flush on behalf of a process that has ended).
- Tests whose timeouts are short bound only the call they are about: a call in a module host under
  ThreadSanitizer can take far longer than the same call in a plain build, so a short lifecycle
  timeout, which bounds `create` and `initialize` too, made a step fail in `create`. The fixture
  module `converting-module` gets the parameter `convert_in` (`initialize`, the default, or
  `invoke`), so that the test of a long `antimony` translation runs it in the first invoke, under
  a short invoke deadline, while the lifecycle timeout keeps its default; the test of a module host
  that does not exit after `destroy` waits 1 s instead of 200 ms; and the margins of the tests of
  a crash as the grace ends (a grace of 1.5 s, with the crash's record held 4 s) and of stops and
  deadlines (5 s, with the module sleeping 20 s) are wider.

## Alternatives considered

- **Keep modules in the runtime's process** ([ADR-0006](ADR-0006-threading-model.md)): no crash is
  contained, and an abandoned call holds its library for the rest of the process.
- **`fork()` the runtime's process** for each instance: the child would inherit every thread's
  locks in an unknown state, every descriptor and the runtime's whole memory; `posix_spawn()` of a
  small executable avoids all three.
- **One module host per library or per run**, shared by several instances: fewer processes, but a
  crash would take several steps down, and the thread-affinity and concurrency rules of a library
  would have to be enforced across instances in one host. One host per instance keeps the failure
  of a step to that step.
- **A public wire protocol** (for example JSON over pipes) for module hosts: a public format would
  need its own versions and compatibility rules; an internal protocol of one build changes freely
  and is refused across builds.
- **Killing only the call's thread** in the runtime's process: no portable way exists, and a thread
  killed inside a library leaves its locks and memory broken.

## Consequences

- A crash in a module that runs in a module host fails its step and is reported, with how the
  module host ended; the runtime and the other steps go on to their teardown.
- A call that never returns no longer leaves anything behind in the runtime's process.
- Each call costs a round trip over the channel and a copy of its documents; each instance costs a
  process and its start.
- The contract checks still run modules in the runtime's process.
- Results do not depend on the mode: a run in module hosts gives the outputs, message trace and
  fingerprint of a run in the runtime's process.
- An engine's crash in a step in process mode ends only that step, and a long engine call ends with
  its module host. Each module host loads and starts its own providers, so each pays an engine's
  start, and compiles its own models unless its step shares them with other steps' module hosts
  ([ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md)).
- Open: process isolation on Windows, and a profile of their own on Linux and Windows (OQ-31); the
  contract checks in a module host (OQ-32). Process isolation on Linux, then still to be verified,
  is verified on x86-64 by CI since 2026-11-22
  ([releasing.md](../releasing.md#what-the-runs-verified)).

## References

- [docs/specs/process-isolation.md](../specs/process-isolation.md)
- [docs/specs/threading-and-ownership.md](../specs/threading-and-ownership.md)
- [docs/specs/error-taxonomy.md](../specs/error-taxonomy.md)
- [ADR-0006](ADR-0006-threading-model.md) (amended), [ADR-0003](ADR-0003-native-module-abi-v0.md),
  [ADR-0024](ADR-0024-contract-checks-and-test-vectors.md),
  [ADR-0026](ADR-0026-runtime-service-providers.md),
  [ADR-0030](ADR-0030-library-gate-across-runs.md),
  [ADR-0035](ADR-0035-model-conversion-service-v0.md): each has a Later note on this decision
- [ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md): shared compiled models
  between module hosts
- [ADR-0039](ADR-0039-restricted-sandbox-profile-for-module-hosts.md): a restricted sandbox
  profile for module hosts
- [docs/open-questions.md](../open-questions.md) (OQ-5, resolved here; OQ-13, OQ-19, OQ-30, OQ-31,
  OQ-32)
