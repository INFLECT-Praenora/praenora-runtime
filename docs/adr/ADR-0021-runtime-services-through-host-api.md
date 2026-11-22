# ADR-0021: Runtime services through the host API; libRoadRunner simulation service deferred

- Status: Proposed
- Date: 2026-11-10
- Supersedes: none
- Superseded by: none

## Context

Some modules will need shared facilities, for example simulating a model from a standard model description, which are heavy dependencies not suited to each module bundling its own. The project wants a runtime-service mechanism and a designed, but not built, libRoadRunner service.

## Decision

- The host keeps a registry mapping a service name and major version to a function table, exposed through `host_api.get_service(context, name, name_size, service_abi_major)`, which returns NULL if unavailable. Service tables are C structs with the same header convention as module tables. Each use is logged in the trace.
- The MVP registers no production services. Tests register a test-only service, `test.gate`, which also drives the unresponsive-module test deterministically.
- Planned services (feasibility documents only): `praenora.simulation@0` backed by libRoadRunner (load and cache models, set parameters, simulate, return results; one engine instance per thread or a pool; build option `PRAENORA_WITH_ROADRUNNER`, OFF and absent from the MVP) and `praenora.model-conversion@0` backed by libAntimony (process-global state serialized behind a mutex).
- The shape of the simulation API (batch or single run, result encoding) is open (OQ-16).

### Builder notes (Proposed)

- `ServiceRegistry` maps a service name and major version to a table. `add()` refuses an empty name, a NULL table and a name and major that are already registered (`INVALID_ARGUMENT`); services are registered before instances use the registry, after which lookups are plain reads and thread-safe.
- Each `get_service()` call is logged at debug level on the step's module logger (event `service.lookup`, with the service, major and whether it was provided).
- Once an instance is unresponsive, its `get_service()` returns NULL: the runtime may tear the registry down while the abandoned module still runs.
- The same holds for the instances the engine keeps after an unresponsive one of the same library (`features.concurrency` `"module"`; ADR-0011): `ModuleInstance::withdraw_services()` clears their registry pointer, under the lock that lookups take, before they are kept, so a module thread that asks for a service once the run, and with it the registry, is gone gets NULL instead of reaching freed memory.
- Later: [ADR-0030](ADR-0030-library-gate-across-runs.md) keeps those instances whenever a call into them is refused because an abandoned call may still be running in their library, whichever run abandoned it; their services are withdrawn the same way, also by `ModuleInstance`'s destructor (event `instance.kept`).
- The test-only `test.gate` service (major 0) is the C struct `praenora_test_gate_v0` in `tests/support/test_gate.h`: the common table header, a context pointer and `is_open(context)`.
- The message trace (`trace/messages.jsonl`) holds envelopes and result diagnostics only, so runtime-service lookups do not appear in it: in this version the Decision's "logged in the trace" is met by the `service.lookup` log event.
- Later: [ADR-0032](ADR-0032-service-lookups-in-the-message-trace.md) puts the lookups in the message trace when tracing is on: one record per service a call into the instance looked up, with the number of lookups. The `service.lookup` log event stays.

## Alternatives considered

- **Each module bundles its own simulator.** Duplicates a large dependency and complicates deployment.
- **A built-in simulation module.** Couples the runtime to one engine.
- **Out-of-process services.** Better isolation but beyond the MVP.

## Consequences

- The mechanism is tested now and real services can be added as minor ABI evolution.
- Services add shared state, which later needs determinism rules per service.

## References

- [ADR-0026](ADR-0026-runtime-service-providers.md): service providers, which offer the planned
  simulation service instead of the runtime linking libRoadRunner.
- [ADR-0027](ADR-0027-simulation-service-v0.md): the simulation service `praenora.simulation@0`,
  whose shape answers OQ-16.
- [ADR-0035](ADR-0035-model-conversion-service-v0.md): the model conversion service
  `praenora.model-conversion@0`, which the provider `antimony` offers instead of the runtime linking
  libAntimony.
- [docs/specs/module-abi.md](../specs/module-abi.md)
- [docs/feasibility/libroadrunner.md](../feasibility/libroadrunner.md)
- [docs/feasibility/libantimony.md](../feasibility/libantimony.md)
- [docs/open-questions.md](../open-questions.md)
