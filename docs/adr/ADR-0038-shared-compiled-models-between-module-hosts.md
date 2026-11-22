# ADR-0038: Shared compiled models between module hosts

- Status: Accepted (scope noted in Decision)
- Date: 2026-11-20
- Supersedes: none
- Superseded by: none

## Context

A step in a module host ([ADR-0037](ADR-0037-process-isolation-of-module-instances.md)) loads and
starts its own providers, so each step's module host compiles the models it opens, even when every
step of a run opens the same one. For libRoadRunner that is costly: compiling a model of 400
reactions takes about 2.8 s and one of 1500 reactions about 20 s, while loading the state that
`saveStateS('b')` wrote takes 0.04 s and 0.16 s, in a fresh process, with byte-identical results
([libroadrunner.md](../feasibility/libroadrunner.md), informative). The runtime's process outlives
its module hosts and talks to each of them, so it can hand a model one compiled to the others.

Results must not depend on it, a module host must not reach what another provider keeps, and what
a module host hands over is untrusted input to the runtime.

## Decision

Status split: sharing compiled models is Accepted, by the maintainer on 2026-11-22: **Opt-in, per
step**, **A store in the runtime's process**, **Events** and **Trust**. Proposed: **Provider host
callbacks**, which extend the provider interface that
[ADR-0026](ADR-0026-runtime-service-providers.md) keeps Proposed; **Through the channel**, part of
the channel that [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) keeps Proposed;
**roadrunner**, the provider's part, as [ADR-0027](ADR-0027-simulation-service-v0.md) keeps its
provider Proposed; and the builder notes. It resolves no open question: trust beyond the teams
that write the modules is OQ-19, and Windows is OQ-31.

- **Opt-in, per step.** The configuration's `isolation` object gains the field
  `share_compiled_models` (boolean, default `false`; `workflow-config` 0.1, changed in place),
  resolved field by field like `mode`. It applies only to a step in mode `process`: validation
  refuses it on a step in mode `in-process` (`CONFIG_INVALID`, at the field that set it), and the
  caller's override `in-process` turns it off for every step. The run record's
  `execution.isolation.<step>` always holds `mode` and `share_compiled_models` (`run-record` 0.2,
  changed in place); neither enters the fingerprint.
- **A store in the runtime's process**, process-wide and in memory only (never on disk): entries
  kept by namespace (the provider's name and its library's SHA-256) and key (at most 256 bytes);
  an entry at most 128 MiB (134217728 bytes); keys and entries together at most 512 MiB
  (536870912 bytes), the least recently used out first (a find and a save are uses).
- **Provider host callbacks.** `praenora_provider_host` gains `load_compiled` and
  `save_compiled`, appended after `log_enabled` (provider interface 0.0, no new version; providers
  check `struct_size`). Both are NULL in the runtime's own process, always, and in a module host
  whose step does not share.
- **Through the channel.** A module host's provider host sends store requests (a load, a save, and
  the notice of a save it refused) over the step's channel; the runtime's reader answers at any
  time, during the load and between calls too. A request names its provider by its index in the
  load, and the runtime takes the namespace from its own load request, never from the module host.
  The module host refuses an entry or a key over its limit itself and sends only a notice; the
  runtime's reader never allocates more for a save than the entry limit and the key limit together.
  A store request from a module host whose step does not share, for a provider the load does not
  have, or over a limit, breaks the protocol.
- **Events.** The runtime logs each request at debug on the requesting step's logger:
  `compiled_models.hit`, `compiled_models.miss`, `compiled_models.saved`, `compiled_models.refused`.
  Providers say at debug through the calling instance's host whether they compiled a model or
  loaded it from the store.
- **roadrunner** keeps a model's saved state (`saveStateS('b')` after compiling) under its existing
  cache key, behind its own cache, and loads it with `loadStateS()` instead of compiling, under its
  engine lock. The state is wrapped in an envelope (a magic, the engine's identity, the state's
  length and SHA-256), checked before libRoadRunner reads it; an entry that fails a check, or that
  `loadStateS()` refuses, is a warning and a compile. A loaded model is the compiled one, field by
  field. **test-simulation** keeps its canonical parsed model, so the store is tested in every
  build.
- **Trust.** A module host whose step shares can give every other sharing step of the process an
  entry of its choosing; for roadrunner that is native code. The envelope finds corruption and
  truncation, not an entry made on purpose. This is documented in
  [process-isolation.md](../specs/process-isolation.md#shared-compiled-models) and
  [configuration.md](../configuration.md#shared-compiled-models), and sharing is off by default.

The specification is [process-isolation.md](../specs/process-isolation.md#shared-compiled-models).

### Builder notes (Proposed)

- The store is `CompiledModelStore` (`src/runtime/isolation/compiled_model_store.hpp`), one per
  process (`compiled_model_store()`, never destroyed). Its limits are constructor arguments, so its
  unit tests use small ones; the process's has the limits above. A save under a key replaces its
  entry; what is evicted or replaced is freed outside the store's lock. The total counts each
  entry's key and bytes, so many empty entries are bounded too.
- Channel frames: `store_load` (24: provider index, key), `store_save` (25: provider index, key
  size, key, entry), `store_refused` (26: provider index, key size, entry size, the key's first 256
  bytes) from the module host, and `store_reply` (11: found, entry) from the runtime; limits
  4 + 256, 4 + 4 + 256 + 128 MiB, 4 + 8 + 8 + 256 and 1 + 128 MiB. The channel protocol number
  stays 1. Store requests have their own ids, counted by the module host; a reply that no request
  waits for ends the module host (the runtime broke the protocol).
- The reader of every frame grows its payload buffer by doubling up to the announced length, never
  beyond it, so a large save takes no more than its announced length. `Channel::write_parts()`
  sends a frame's fields, key and entry without copying them into one string, and the runtime keeps
  a saved entry by taking the frame's payload over.
- The runtime's reader answers a store request itself, and holds the channel's writer while it
  writes: a module host that stopped reading would hold both for good. So the answer is written
  within a wait (`ChildSettings::store_reply_wait`, 30 s); a module host that does not take it
  breaks the protocol and is killed.
- No write of the runtime's side waits for good on a module host that stops reading. A bounded
  write (`Channel::write_parts_until()`, and `write_parts_while()`, which waits in slices of 5 ms
  and asks between them whether to go on) waits for room with `poll()` until the end of its slice
  or its deadline and then sends only what the stream has room for, so no `sendmsg` waits; the
  socket's send timeout (`SO_SNDTIMEO`), set under the writer's lock to one slice and back to none
  for an unbounded write, is a second bound. (The send timeout alone bounds each wait for room on
  macOS, not a whole `sendmsg`, so a module host that keeps taking a few bytes could stretch it:
  [ADR-0039](ADR-0039-restricted-sandbox-profile-for-module-hosts.md)'s builder notes.) Both are
  send-side only, so the descriptor stays blocking and a read on it, by another thread at the same
  time, is undisturbed; a first version switched the descriptor
  to non-blocking while it wrote (macOS ignores `MSG_DONTWAIT` on a stream socket pair), which only
  the reading thread could safely do, and which a read entering `recv()` meanwhile saw as a broken
  channel. The other choice, a runtime end that is non-blocking for good with `poll()` in every
  read, would have changed every read for one property of the writes. A bounded write that gives
  up within its frame leaves the channel `broken()`: every later write fails at once.
  - The handshake and the load request are taken within the handshake's timeout (a stop ends the
    load's write too); a module host that does not take its load breaks the protocol.
  - A call's frame is written in slices that ask `InstanceHost::call_stopped()`: a stop before the
    module host has taken the call kills the module host (it has not begun the call) and the call
    returns `CANCELLED`, so a run ends `RUN_STOPPED` at once. The call's deadline does not end the
    write (no cancellation can reach a module whose call has not arrived): the instance's kill at
    the end of the grace does, and the call is `MODULE_UNRESPONSIVE` with `terminated`.
  - A cancel frame is tried for one poll (5 ms); one that got no turn is tried again at the next
    poll, one cut short is given up (the grace's kill follows).
  - `end()` writes the request to exit within its wait, and kills the module host when the wait has
    passed; a module host that breaks the protocol while it is being ended (an answer of the store
    it did not take) is killed at once by `connection_ended()`, which used to leave a module host
    that was exiting alone.
- A failed save of a compiled model is a warning through the calling instance's host, in roadrunner
  and test-simulation ("the compiled model is not kept in the shared store"); roadrunner catches
  whatever `saveStateS()` throws, so a compile that succeeded never fails for it.
- `CompiledModelStore::save()` allocates everything (the key, the entry's list node, the index's
  node) before it changes the store, so a `std::bad_alloc` leaves the store as it was.
- A key over 256 bytes is refused like an entry over 128 MiB: the module host sends a notice, and
  the runtime logs `compiled_models.refused` (with `key_size`, `size` and the key's first 256
  bytes), so a provider whose keys are too long is seen in the log. A load under such a key returns
  0 in the module host without asking, and logs nothing. A notice that refuses an entry within the
  limits, or whose key is not the size it says, breaks the protocol.
- The events' attributes: `module`, `pid`, `provider` (the name), `key`, and `size` (hit, saved,
  refused), `evicted` (saved), `key_size` (refused).
- The provider loader takes a `CompiledModelAccess` (`provider_loader.hpp`), which the module host
  gives every provider of a sharing step; the provider host that `start()` gets has the callbacks
  when the load that starts the provider has one, NULL otherwise. A load waits for its answer on a
  condition variable that the module host's reader thread signals.
- `validate --load` gives a module host the sharing of the step it loads for, so a provider's
  `start()` sees the store as in a run; it loads a module once for each set of isolation settings
  of its steps in module hosts, so a sharing step and a non-sharing step of one module are both
  checked ([ADR-0037](ADR-0037-process-isolation-of-module-instances.md)).
- The runtime's end of the channel of a module host whose step does not share refuses the store's
  frames (`store_load`, `store_save`, `store_refused`) at their header
  (`Channel::refuse_kinds()`), before any of their payload is read: the frame limits are one for
  every module host, so a save from such a module host made the runtime read up to 128 MiB before
  the check of its fields refused it. The fields' own check stays.
- The override `process` keeps each step's sharing as the file says, with the pointer of the field
  that set it; a run field set to `true` with a step in mode `in-process` refuses that step, once
  for each such step, at the run's field.
- roadrunner's envelope: `praenora-roadrunner-state` and a new line, the identity's size (u64,
  little-endian) and the identity (the manifest's engine object and the load options, the same
  string its cache key hashes), the state's size (u64) and its SHA-256 (32 bytes), then the state.
  Under one engine lock: the text's checks, the store's load, `loadStateS()` on a default-made
  `rr::RoadRunner` (or the compile), `saveStateS('b')` and the store's save. Whatever
  `loadStateS()` throws (also `bad_alloc`) is "does not load"; a state that cannot be saved is not
  kept. One function derives the compiled model's ids, parameter indices, quantities and events
  from either object. Its engine code exposes `state_envelope()`, `envelope_payload()`,
  `compile_model()`, `load_compiled_model()` and `CompiledModel::saved_state()`, which a test
  executable of its own (`praenora_roadrunner_engine_tests`, label `contract`, roadrunner builds
  only) compiles from the provider's sources with the provider's definitions, to compare a loaded
  model with the compiled one field by field.
- test-simulation's entry is `{"text", "model"}` (the model's text and its canonical JSON) under
  `test-decay:` and the FNV-1a (64-bit) hash of the text; a session uses an entry only when its
  text is the session's, so a hash collision costs a compile, never a wrong model. It logs
  "test-simulation: compiled the model" in every process, so a step in the runtime's process and
  one in a module host that does not share log the same records.
- Test fixtures: the provider `test-store` (`tests/fixtures/providers/test-store/`) drives the
  store as its `test.echo` input says, from a call, from its `start()`
  (`PRAENORA_TEST_STORE_AT_START`) and from a thread of its own between calls
  (`PRAENORA_TEST_STORE_LATER`); the fake module host breaks the store's rules in eight modes,
  one of which stops reading after its request, and two more load the module as a module host does
  (with the loader, in the fake's own process) and then stop reading. The
  store belongs to the test's process, so the tests use keys and model texts no earlier test of the
  process used, and pass in one process and repeated.

## Alternatives considered

- **A store on disk**, shared across runtime processes: compiled code would outlive the process
  and its trust, and need permissions, cleanup and a format with versions; in memory only, the
  store ends with the runtime's process.
- **Module hosts handing models to each other directly**: each would need channels to every other,
  and a namespace no module host could pick for itself would have no keeper. The runtime already
  holds each channel and each load request.
- **Namespaces named by the module host** (the provider name in each request): a module host could
  then read or write another provider's entries; the index into the runtime's own load request
  cannot name anything the load does not hold.
- **Sending an oversized entry and refusing it in the runtime**: the runtime would have to read
  (and so allocate) what it refuses; refusing it in the module host sends only a notice.
- **Trusting `loadStateS()`'s own checks**: it checks only a magic number and a data version, then
  trusts every length in the state; an envelope with the state's length and SHA-256 finds
  corruption and truncation before libRoadRunner reads any of it.
- **On by default**: sharing couples the trust of the steps' modules, so a configuration chooses
  it.

## Consequences

- A run whose sharing steps open one model compiles it once per runtime process, and later runs in
  the same process find it too.
- The runtime's process holds up to 512 MiB more memory for the store.
- A sharing step trusts every other sharing step's modules with code it will run.
- Log records of sharing steps depend on the store's content (who compiled, who loaded), as
  provider start records and provider caches already depend on the process's history; outputs,
  trace and fingerprint do not.
- Open: Windows (no process isolation in this version): OQ-31. Linux, not compiled where this was
  built, is compiled and tested on x86-64 by CI since 2026-11-22, the sharing tests included
  ([releasing.md](../releasing.md#what-the-runs-verified)).

## References

- [docs/specs/process-isolation.md](../specs/process-isolation.md#shared-compiled-models)
- [docs/specs/runtime-services.md](../specs/runtime-services.md#provider-interface)
- [docs/specs/simulation-service.md](../specs/simulation-service.md#the-roadrunner-provider)
- [docs/feasibility/libroadrunner.md](../feasibility/libroadrunner.md)
- [ADR-0037](ADR-0037-process-isolation-of-module-instances.md): process isolation of module
  instances
- [ADR-0026](ADR-0026-runtime-service-providers.md): runtime service providers
- [ADR-0039](ADR-0039-restricted-sandbox-profile-for-module-hosts.md): the restricted sandbox
  profile, under which a sharing module host loads the store's entries from its channel too
- [docs/open-questions.md](../open-questions.md) (OQ-19, OQ-31)
