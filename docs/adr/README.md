# Architecture decision records

This directory holds the architecture decision records (ADRs) of `praenora-runtime`. Each ADR is its own document with a unique label, named `ADR-NNNN-<kebab-title>.md`. Start from [template.md](template.md). Labels are never reused.

## Statuses

| Status | Meaning |
|---|---|
| Proposed | A provisional decision. The project proceeds on it, and it remains open to maintainer review. |
| Accepted | A settled decision, either required by the project or confirmed by review. |
| Superseded | Replaced by a later ADR. The text is kept; the header names the replacement. |
| Deprecated | No longer applies, with no replacement. |

Some ADRs have a split status, for example "Accepted (shape) / Proposed (details)". Their metadata says `Status: Accepted (scope noted in Decision)` and the Decision section marks which parts are Proposed.

## Process

- **Writing an ADR.** Copy the template, take the next free label, fill every section, and add a row to the index below.
- **Superseding an ADR.** Write a new ADR that states what it replaces, set `Supersedes:` in the new header, set the old header's status to Superseded and its `Superseded by:` to the new label. Do not delete or rewrite the old decision.
- **Proposed decisions** stay open to maintainer review. Reviewing one means either changing its status to Accepted or superseding it.
- **Builder notes.** Calls made while building that do not change what gets built are appended as a "Builder notes" subsection under Decision, with status Proposed.
- **Open questions.** Questions that remain unresolved are tracked in [../open-questions.md](../open-questions.md) and link to the relevant ADRs.

## Index

| Label | Title | Status | Date |
|---|---|---|---|
| [ADR-0001](ADR-0001-record-architecture-decisions.md) | Record architecture decisions | Accepted | 2026-11-10 |
| [ADR-0002](ADR-0002-repository-scope.md) | Repository scope: runtime, SDK and protocol here; modules in separate repositories | Accepted | 2026-11-10 |
| [ADR-0003](ADR-0003-native-module-abi-v0.md) | Native module ABI v0: single entry point returning a versioned function table | Accepted (shape) | 2026-11-10 |
| [ADR-0004](ADR-0004-lifecycle-functions-and-documents.md) | Lifecycle functions and documents of ABI v0 | Proposed | 2026-11-10 |
| [ADR-0005](ADR-0005-ownership-and-allocation-across-the-abi.md) | Ownership and allocation across the ABI: borrowed inputs, host-owned sinks | Proposed | 2026-11-10 |
| [ADR-0006](ADR-0006-threading-model.md) | Threading: per-instance worker threads, serialized calls, thread-safe host callbacks | Proposed | 2026-11-10 |
| [ADR-0007](ADR-0007-json-messages-for-v0.md) | JSON messages for v0; nlohmann/json; binary encodings deferred | Accepted | 2026-11-10 |
| [ADR-0008](ADR-0008-json-schema-draft-07-embedded-schemas.md) | JSON Schema draft-07 for all Praenora schemas; schemas embedded in the runtime | Proposed | 2026-11-10 |
| [ADR-0009](ADR-0009-module-manifests.md) | Module manifests as separately discoverable documents; identity cross-check | Accepted (discovery) / Proposed (details) | 2026-11-10 |
| [ADR-0010](ADR-0010-workflow-definitions-as-catalog-data.md) | Workflow definitions as versioned catalog data; YAML config binds steps to modules | Accepted | 2026-11-10 |
| [ADR-0011](ADR-0011-tick-based-synchronous-execution.md) | Tick-based synchronous execution; deterministic schedule; feedback by delayed connections | Proposed | 2026-11-10 |
| [ADR-0012](ADR-0012-time-timeouts-and-cancellation.md) | Time representation, timeouts and cooperative cancellation | Proposed | 2026-11-10 |
| [ADR-0013](ADR-0013-error-taxonomy-and-status-codes.md) | Error taxonomy and status codes | Proposed | 2026-11-10 |
| [ADR-0014](ADR-0014-versioning-and-compatibility-policy.md) | Versioning and compatibility policy | Accepted (SemVer, independent ABI) / Proposed (rules) | 2026-11-10 |
| [ADR-0015](ADR-0015-observability.md) | Observability: spdlog, OpenTelemetry-aligned logs and metrics, status snapshots and status file | Proposed | 2026-11-10 |
| [ADR-0016](ADR-0016-reproducibility.md) | Reproducibility: run fingerprint, run record, seed derivation, determinism contract | Proposed | 2026-11-10 |
| [ADR-0017](ADR-0017-dependency-management.md) | Dependency management: find_package first, opt-in pinned FetchContent | Accepted | 2026-11-10 |
| [ADR-0018](ADR-0018-python-bindings-with-swig.md) | Python bindings with SWIG over a narrow facade; scikit-build-core; namespace package | Proposed | 2026-11-10 |
| [ADR-0019](ADR-0019-platform-loading-rpath-dll-search.md) | Platform loading, RPATH and Windows DLL search policy | Proposed | 2026-11-10 |
| [ADR-0020](ADR-0020-message-envelope-and-asyncapi.md) | Message envelope modeled on CloudEvents; AsyncAPI generated from workflow definitions | Proposed | 2026-11-10 |
| [ADR-0021](ADR-0021-runtime-services-through-host-api.md) | Runtime services through the host API; libRoadRunner simulation service deferred | Proposed | 2026-11-10 |
| [ADR-0022](ADR-0022-target-data-intake.md) | Target data intake through runtime-provided replay and document sources | Proposed | 2026-11-10 |
| [ADR-0023](ADR-0023-public-cpp-api.md) | Public C++ API: exceptions, pimpl, no third-party types in public headers | Proposed | 2026-11-10 |
| [ADR-0024](ADR-0024-contract-checks-and-test-vectors.md) | Contract checks and test vectors as the module acceptance gate | Proposed | 2026-11-10 |
| [ADR-0025](ADR-0025-webassembly-strategy.md) | WebAssembly strategy: separate runtime models, design only for the MVP | Proposed | 2026-11-10 |
