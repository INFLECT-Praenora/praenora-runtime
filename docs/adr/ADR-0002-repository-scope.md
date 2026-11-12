# ADR-0002: Repository scope: runtime, SDK and protocol here; modules in separate repositories

- Status: Accepted
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Praenora takes in target data, runs a workflow of scientific modules on every update, and keeps predictions current. The science is implemented by separately versioned module repositories, each owned by a different team, which must ship on their own schedules without recompiling the runtime.

## Decision

This repository contains the integration layer only:
- the runtime library (`Praenora::Runtime`, shared library `praenora_runtime`) and the `praenora` command-line tool;
- the module SDK (`Praenora::ModuleSDK`): the C ABI, an optional C++ adapter, a CMake helper and a test kit;
- the Praenora Module Protocol (PMP) and Praenora Module Manifest (PMM): schemas, catalog, and normative documents;
- the Python bindings (`praenora.runtime`);
- two example modules and an example workflow, used to prove the integration;
- tests, documentation and feasibility designs.

Modules live in their own repositories and are built against the installed SDK. The runtime never links against module targets; it discovers manifests and loads libraries at run time. Automated cloning or building of module repositories is out of scope for the MVP.

## Alternatives considered

- **Monorepo with all modules.** Simplifies integration builds but couples release schedules and ownership, which the project must avoid.
- **Modules as static libraries linked into the runtime.** Requires recompiling the runtime for every module change.
- **Separate repositories for runtime, SDK and protocol.** Cleaner ownership, but the three evolve together in the MVP and would need synchronized releases.

## Consequences

- Module teams depend only on the SDK and the protocol documents.
- The runtime must work with modules it has never seen, so nothing in it may hard-code module names, step counts or topology.
- Integration of real modules is a later concern (see OQ-14 in the register).

## References

- [docs/architecture.md](../architecture.md)
- [docs/module-developer-guide.md](../module-developer-guide.md)
- [docs/open-questions.md](../open-questions.md)
