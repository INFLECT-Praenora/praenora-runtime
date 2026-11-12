# ADR-0004: Lifecycle functions and documents of ABI v0

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

The ABI needs a call sequence that lets the host validate a module before use, create one instance per workflow step, run it tick by tick, and tear it down deterministically, even after failures.

## Decision

The v0 table has required functions `describe`, `create`, `initialize`, `invoke`, `finalize` and `destroy`, and optional `diagnostics` and `self_test`.
- `describe` is side-effect free and thread-safe; it returns identity JSON (name, version, abi, sdk_version, pmp_version, capabilities).
- `create` takes a create document (`pmp_version`, `step`, `capability`, `module`, validated `parameters`) and returns an instance handle.
- `initialize` takes an initialize document (`pmp_version`, `run {fingerprint, seed}`, `time {unit, start, period}`).
- `invoke` runs once per tick with an invocation document (`step`, `tick`, `logical_time`, `inputs` as envelopes; absent optional inputs omitted) and returns a result document (`outputs` with `data`, optional `diagnostics`).
- `finalize` is called only if `initialize` succeeded. `destroy` is called exactly once per successful `create` and cannot fail.
- `self_test` is used by contract checks only, never concurrently with other calls into the library.
- Host view of the instance state machine: unloaded, loaded, created, ready, busy, finalized, destroyed; plus failed and the terminal unresponsive state.
- Shutdown finalizes initialized instances, then destroys created instances, in reverse creation order. A library is unloaded only after its instances are destroyed and their threads joined. An unresponsive instance is never destroyed; its library is pinned.
- Every document carries `pmp_version` and has a schema in `spec/schemas/`.
- The adapter in `include/praenora/sdk/module.hpp` wraps the table in a `Module` base class; trampolines catch every exception (`sdk::Error` to its code, `std::bad_alloc` to `RESOURCE_EXHAUSTED`, others to `INTERNAL`).

### Builder notes (Proposed)

- The SDK offers `PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, getter_fn)`, which defines a non-exported getter returning the table (for in-process tests), and `PRAENORA_SDK_DEFINE_MODULE(ClassName)`, which defines that getter plus the single exported entry point.
- The getter has external linkage unless the expansion is placed in an unnamed namespace or prefixed with `static`; `PRAENORA_SDK_DEFINE_MODULE` places its getter in an unnamed namespace. The identity macros are read where the macro is expanded, so one test source can define several modules.
- The adapter parses no JSON, so `CreateContext` carries the host and the create document, and `InitializeContext` only the initialize document; modules read the step, parameters and seed themselves.
- The adapter's `create` returns `INVALID_ARGUMENT` for a NULL argument, and `UNSUPPORTED` for a host API of another ABI major or one too short for its fixed fields (`struct_size`, `abi_major`, `abi_minor`, `context`). The adapter needs none of the host callbacks itself: `Host` skips a callback that the host's `struct_size` does not cover (no-op, false or NULL). A module that cannot work without a callback throws `sdk::Error(PRAENORA_STATUS_UNSUPPORTED, ...)` from its constructor.
- The module object's destructor is the `destroy` call, and its `Host` is still valid there. Adapter tables always provide `diagnostics` (by default `{}`) and leave `self_test` NULL. `describe` is built from `PRAENORA_MODULE_NAME`, `PRAENORA_MODULE_VERSION`, `PRAENORA_MODULE_CAPABILITIES`, the ABI version of `abi.h` and the versions of `version.h`.
- An `sdk::Error` whose code is neither 1–99 nor at least 10000 becomes `INTERNAL`, and the error message names the rejected code. Error details that are not a JSON object are reported as `{"text": "<details>"}`. Strings are escaped like `praenora::sdk::detail::json_escape`, which also turns every byte that is not well-formed UTF-8 into U+FFFD, so the adapter's documents are always valid JSON.
- Host states (`ModuleInstance`): `loaded` → `created` → `ready` → `finalized` → `destroyed`, with `busy` during every call. A create, initialize or finalize that does not succeed leaves `failed`; an invocation leaves `ready` whatever its outcome, so every initialized instance is finalized. `destroy` is called at most once, only for an instance a create returned (even after that create's deadline), never after a failed create and never for an unresponsive instance. A create that returns OK without an instance is reported as `INTERNAL`. The instance's destructor finalizes a `ready` instance and destroys an existing one, each bounded by the lifecycle timeout and the cancellation grace, then joins the worker.
- Module log messages are routed to the step's logger with the attributes `run.fingerprint` (when the instance is given the run's fingerprint), `tick` (during an invocation only), `step` and `module` (`name@version`); levels below trace count as trace and above critical as critical.
- `pmp_version` in the ABI documents: the runtime writes its own PMP version into the create, initialize and invocation documents and into every envelope. A result may carry `pmp_version`, which the runtime does not read; the error document (`pmp-error`: `code`, `message`, optional `details`) has none; and `describe()` reports the module's own, of which only the form is checked: compatibility is decided from the manifest's `pmp_version` before loading, and the identity check does not compare the two.

## Alternatives considered

- **Single `run` call.** Cannot separate parameter validation, seeding and teardown, and gives no place to report lifecycle failures.
- **Constructor-style load with no `finalize`.** Loses a distinction between orderly shutdown and destruction after failure.
- **Streaming or callback-driven invocation.** More general, but complicates determinism and the MVP's synchronous tick model (ADR-0011).

## Consequences

- Lifecycle failures are attributable to a phase (`create` or `initialize`).
- The contract checks can exercise each call in isolation.
- Modules with no state still implement all required functions.
- Adding optional functions later needs only a minor ABI bump.

## References

- [docs/specs/module-abi.md](../specs/module-abi.md)
- [docs/specs/protocol.md](../specs/protocol.md)
- [docs/module-developer-guide.md](../module-developer-guide.md)
