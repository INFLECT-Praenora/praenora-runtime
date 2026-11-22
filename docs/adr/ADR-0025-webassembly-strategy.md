# ADR-0025: WebAssembly strategy: separate runtime models, design only for the MVP

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

WebAssembly could mean a runtime compiled to run in a browser or Node, or WebAssembly modules hosted by a native runtime. These have different requirements, and neither is needed for the MVP. Priority is open (OQ-17).

## Decision

- The MVP implements neither. A feasibility document describes both models: Model A, the runtime compiled with Emscripten with statically registered modules or side modules; Model B, the native runtime hosting WebAssembly modules through an embedded engine (for example Wasmtime or WAMR), with the JSON ABI mapped to linear memory, preemption through fuel or epoch interruption, and the component model as a later option.
- The MVP keeps seams that help either model: a loader interface separate from the engine, an inline-executor option when threads are unavailable, and no file I/O in the engine's core.
- Native and WebAssembly modules are expected to be different runtime models, not one ABI.
- The document lists threading constraints (SharedArrayBuffer, COOP and COEP), determinism, and a CMake toolchain preset sketch, and a spike plan with exit criteria.

### Builder notes (Proposed)

- The MVP has neither an inline-executor seam nor a loader interface yet. Every `ModuleInstance` starts a worker thread of its own (`InstanceWorker`, `src/runtime/instance/worker.hpp`), so there is no way to run module calls on the calling thread; and `LoadedModule` (`src/runtime/loader/loaded_module.hpp`) is built only from a `SharedLibrary`, which the engine, validation with loading and the contract checks open through `LoadedModule::load()`, so the runtime has no other source of a v0 table. The feasibility document records the state of each seam and recommends adding both as internal changes, with no ABI or format change, before either model is built ([docs/feasibility/webassembly.md](../feasibility/webassembly.md#the-mvps-seams-today)).

## Alternatives considered

- **Implement a WebAssembly loader now.** Premature without a priority decision.
- **Ignore WebAssembly.** Risks entrenching assumptions (threads, file I/O) that would be expensive to unwind.
- **One ABI for native and WebAssembly modules.** Hides real differences in memory, threading and preemption.

## Consequences

- No MVP cost beyond keeping seams clean.
- A later decision needs a spike and probably a new ADR superseding this one.

## References

- [docs/feasibility/webassembly.md](../feasibility/webassembly.md)
- [docs/open-questions.md](../open-questions.md)
- [WebAssembly specifications](https://webassembly.org/specs/)
