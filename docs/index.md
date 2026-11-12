# Documentation index

## Repository-level documents

- [README.md](../README.md): overview, quick start per platform, project status.
- [AGENTS.md](../AGENTS.md): agent and contributor guide: commands, layout, conventions, rules.

## Guides

- [architecture.md](architecture.md): components, data flow, the life of a run and of a module
  instance, threads (Mermaid diagrams), and the security rules.
- [user-guide.md](user-guide.md): getting the tool, the CLI's commands, running the example
  workflow, the files a run writes, status, stopping, logging, troubleshooting.
- [configuration.md](configuration.md): the workflow configuration reference: every section and
  key, input sources, durations, paths, overrides, environment variables, validation.
- [developer-guide.md](developer-guide.md): dependencies, presets, building and installing,
  debugging modules, keeping libraries loaded, message tracing, logging, sanitizer builds, adding a
  module locally, conventions and the ADR process.
- [module-developer-guide.md](module-developer-guide.md): writing a module: the SDK, ABI rules, the
  C++ adapter, the manifest, `praenora_add_module()`, the test kit, test vectors, the contract
  checks, determinism and portability, versioning.
- [testing.md](testing.md): running the tests and their labels, sanitizer builds, golden files,
  triage by label and error code, the module acceptance gate, adding tests.
- [python.md](python.md): the Python API, its errors, threads and logging, building the bindings,
  the tests and the wheel.
- [platforms.md](platforms.md): macOS, Linux and Windows: dependencies, presets, run paths, DLL
  search, code signing, and what is verified where.

## Normative specifications

- [specs/module-abi.md](specs/module-abi.md): the module ABI v0.
- [specs/protocol.md](specs/protocol.md): the Praenora Module Protocol (PMP) 0.1: documents,
  envelope, module-ABI documents, catalog, execution semantics, CloudEvents mapping.
- [specs/manifest.md](specs/manifest.md): the Praenora Module Manifest (PMM) 0.1.
- [specs/workflow-definitions.md](specs/workflow-definitions.md): workflow definitions and
  capabilities, graph rules, the schedule.
- [specs/compatibility-policy.md](specs/compatibility-policy.md): versions and the compatibility
  rules.
- [specs/error-taxonomy.md](specs/error-taxonomy.md): every error code, its category, exit code
  and owner, and triage.
- [specs/threading-and-ownership.md](specs/threading-and-ownership.md): ownership across the ABI and
  the threads that call modules.
- [specs/time-cancellation-timeouts.md](specs/time-cancellation-timeouts.md): logical time,
  durations, deadlines, cancellation and stop requests.
- [specs/observability.md](specs/observability.md): logs, metrics, status snapshots, health, the
  status file and the message trace.
- [specs/reproducibility.md](specs/reproducibility.md): the run fingerprint, canonical JSON, seeds,
  the run record and the output files.
- [specs/starting-workflow.md](specs/starting-workflow.md): the starting workflow
  `pmp.continuous-forecast`, marked draft.

## Feasibility designs

- [feasibility/webassembly.md](feasibility/webassembly.md): running the runtime as WebAssembly, or
  hosting WebAssembly modules.
- [feasibility/libroadrunner.md](feasibility/libroadrunner.md): a simulation service backed by
  libRoadRunner.
- [feasibility/libantimony.md](feasibility/libantimony.md): a model conversion service backed by
  libAntimony.

These are designs only: nothing they propose exists in this version.

## Decisions and open questions

- [Architecture decision records](adr/README.md): index and status rules.
- [ADR template](adr/template.md)
- [Open-questions register](open-questions.md)
