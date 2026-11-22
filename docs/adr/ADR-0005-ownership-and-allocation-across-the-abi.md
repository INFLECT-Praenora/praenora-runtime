# ADR-0005: Ownership and allocation across the ABI: borrowed inputs, host-owned sinks

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Modules and the host may use different allocators and runtimes. Any allocation that crosses the boundary risks mismatched `free` calls, especially on Windows where each DLL can have its own heap.

## Decision

- **Inputs are borrowed.** `praenora_bytes` and the host pointer belong to the host. A module copies anything it needs beyond the call (the host pointer is valid from `create` until `destroy` returns).
- **Outputs go through sinks.** A module calls `sink->write` zero or more times; bytes are appended and the host copies them before `write` returns. `write` returns a non-OK status if the host refuses the data (for example a size limit), and the module should return that status.
- **Instances are module-owned.** `create` allocates and `destroy` frees with the module's own allocator.
- **Nothing else crosses.** No `malloc`/`free` pairing across the boundary, no `FILE*`, no C++ objects, no exceptions, no `longjmp`.
- **Static data.** The function table and `describe` data are static and live while the library is loaded.

### Builder notes (Proposed)

- A module result is limited to 256 MiB by default; when a call's sink writes exceed that, the sink refuses them with `PRAENORA_STATUS_RESOURCE_EXHAUSTED`.
- The SDK macro naming for module definition is recorded in ADR-0004; it does not change these ownership rules. `PRAENORA_SDK_DEFINE_MODULE_GETTER` produces a non-exported getter so in-process tests can drive a table without `dlopen`, and `PRAENORA_SDK_DEFINE_MODULE` adds the exported entry point.
- The C++ adapter writes every output to its sink in pieces of 4096 bytes (`praenora::sdk::detail::kSinkChunkSize`), only the last one shorter, so hosts see large outputs arrive in several writes. The fixture `fx-echo` relies on this to test concatenation.
- The adapter writes error documents through a fixed buffer, without allocating, so an out-of-memory failure is still reported. When a sink refuses a write, the adapter returns the sink's status unchanged; for a refused result it also writes an error document with that code.
- A zero-length input or log message may come with a NULL data pointer, and both sides accept that. A NULL data pointer with a nonzero size is `INVALID_ARGUMENT` in the adapter.
- The host's sinks (`OutputSink`) copy every write. The size limit (`max_output_bytes`, 256 MiB by default) applies to each of a call's sinks, the error sink included; once a sink refuses a write it refuses every later one, so a refused document never continues with a gap. `describe()` output is limited to 1 MiB.
- The host owns each call's input document for as long as the call runs, even past the caller's wait, so a module that is still running after its caller gave up (unresponsive) never reads freed input.

## Alternatives considered

- **Module-allocated output buffers with a host-called release function.** Works, but adds a call and a lifetime rule per buffer.
- **Host-allocated output buffers sized by the module.** Needs a two-call protocol or a guess at the size.
- **Shared allocator contract (everyone uses `malloc`).** Fragile across runtimes and Windows CRTs.

## Consequences

- No allocator coupling between the host and modules.
- Each output costs one copy, which is acceptable for the MVP's JSON payloads.
- Modules have simple rules to follow and the contract checks can test sink refusal.

## References

- [docs/specs/threading-and-ownership.md](../specs/threading-and-ownership.md)
- [docs/specs/module-abi.md](../specs/module-abi.md)
