# Documentation index

## Repository-level documents

- [README.md](../README.md): overview, quick start per platform, project status.
- [AGENTS.md](../AGENTS.md): agent and contributor guide: commands, layout, conventions, rules.
- [LICENSE](../LICENSE): the MIT license of this repository's files
  ([ADR-0040](adr/ADR-0040-mit-license.md)).

## Guides

- [architecture.md](architecture.md): components, data flow, the life of a run and of a module
  instance, threads (Mermaid diagrams), module hosts, service providers, Python modules and the
  Python core, the control API and the OTLP export, and the security rules.
- [user-guide.md](user-guide.md): getting the tool, the CLI's commands, running the example
  workflow, process isolation, service providers, Python steps and the Python environment in an
  installation, the files a run writes, status, stopping, watching and running over HTTP
  (`run --listen`, `serve`, `status --server`), logging and the OTLP export, troubleshooting.
- [configuration.md](configuration.md): the workflow configuration reference: every section and
  key, input sources, durations, paths, isolation (module hosts, shared compiled models, the
  restricted sandbox profile), overrides, environment variables (the `OTEL_*` variables of the
  OTLP export too), validation.
- [developer-guide.md](developer-guide.md): dependencies (cpp-httplib among them), presets, the
  dependency and Python environment bundles, building, installing and archiving, the providers,
  the Python environment and providers implemented in Python, debugging modules (in module hosts
  too), keeping libraries loaded, message tracing, logging, sanitizer builds, adding a module
  locally, conventions and the ADR process.
- [module-developer-guide.md](module-developer-guide.md): writing a module: the SDK, ABI rules, the
  C++ adapter, the manifest, `praenora_add_module()`, the services a module requires, the test kit,
  modules in Python (the class, a complete example, `praenora_add_python_module()`, the sources'
  digest, child processes, the Python test kit), test vectors, the contract checks, a module in a
  module host and under the restricted sandbox profile, determinism and portability, versioning.
- [testing.md](testing.md): running the tests and their labels, the rules of the tests that open
  local listeners, module hosts and nested sandboxes, the tests of the providers and of the Python
  environment, sanitizer builds, golden files, triage by label and error code, the module
  acceptance gate, adding tests.
- [python.md](python.md): the Python API, process isolation (Python steps run in module hosts),
  its errors, threads, logging and the OTLP export, building the bindings, the tests and the wheel
  (which holds `praenora.sdk`, but no module host and no Python environment).
- [platforms.md](platforms.md): macOS, Linux and Windows: dependencies (json-schema-validator and
  cpp-httplib from source), presets, run paths (the Python core's too), DLL search, long paths on
  Windows, code signing, the module host and the restricted sandbox profile, the control API's
  platform differences, and what is verified where.
- [releasing.md](releasing.md): the GitHub Actions workflow that builds and tests every pull
  request against the pinned dependency bundles and Python environment bundles, what its runs
  verified, its jobs and runners, raising each pin, the required checks and repository settings,
  and the release procedure
  ([ADR-0041](adr/ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md),
  [ADR-0045](adr/ADR-0045-python-environment-pinned-in-ci-and-installation.md)).

## Normative specifications

- [specs/module-abi.md](specs/module-abi.md): the module ABI v0.
- [specs/protocol.md](specs/protocol.md): the Praenora Module Protocol (PMP) 0.1: documents,
  envelope, module-ABI documents, catalog, execution semantics, CloudEvents mapping.
- [specs/manifest.md](specs/manifest.md): the Praenora Module Manifest (PMM) 0.2, with the
  services a module requires.
- [specs/runtime-services.md](specs/runtime-services.md): runtime services and service providers:
  the provider manifest, the provider interface (internal), discovery, resolution and loading.
- [specs/simulation-service.md](specs/simulation-service.md): the simulation service
  `praenora.simulation@0`: sessions, simulate, plans and batches, kernels, threading, limits, and
  the service checks.
- [specs/model-conversion-service.md](specs/model-conversion-service.md): the model conversion
  service `praenora.model-conversion@0`: describe, convert, its options, rules and report,
  threading, cancellation, limits, the antimony provider (Antimony and SBML with libAntimony), the
  test provider and the service checks.
- [specs/python-service.md](specs/python-service.md): the Python service `praenora.python@0` and
  Python modules: the directory digest of Python sources, the package `praenora.sdk` and its test
  kit, the service's table, Python modules, the Python core and its interpreter, output and child
  processes, the provider `python`, the providers that run Python and those implemented in Python,
  and the process's environment.
- [specs/workflow-definitions.md](specs/workflow-definitions.md): workflow definitions and
  capabilities, graph rules, the schedule.
- [specs/compatibility-policy.md](specs/compatibility-policy.md): versions and the compatibility
  rules.
- [specs/error-taxonomy.md](specs/error-taxonomy.md): every error code, its category, exit code
  and owner, and triage.
- [specs/threading-and-ownership.md](specs/threading-and-ownership.md): ownership across the ABI and
  the threads that call modules.
- [specs/process-isolation.md](specs/process-isolation.md): process isolation of module instances:
  the configuration's isolation settings, the module host and where it is found, the channel
  (internal), loading, runs and validation, calls and the host API, service providers in module
  hosts, shared compiled models and their trust, the restricted sandbox profile, crashes,
  unresponsive calls, ending, determinism, stops, platforms and limits.
- [specs/time-cancellation-timeouts.md](specs/time-cancellation-timeouts.md): logical time,
  durations, deadlines, cancellation and stop requests.
- [specs/observability.md](specs/observability.md): logs, metrics, status snapshots, health, the
  status file, the OTLP export of metrics and logs, the Prometheus text of the metrics, and the
  message trace.
- [specs/control-api.md](specs/control-api.md): the control and status API over HTTP, its
  security rules first (listeners, tokens, scopes, the Host check), addresses, run and serve mode
  and their command lines, routes, run entries and request bodies, HTTP statuses, the OpenAPI
  document (`spec/openapi/`), the Prometheus text and limits.
- [specs/reproducibility.md](specs/reproducibility.md): the run fingerprint, canonical JSON, seeds,
  the run record and the output files.
- [specs/starting-workflow.md](specs/starting-workflow.md): the starting workflow
  `pmp.continuous-forecast`, marked draft.

## Feasibility designs

- [feasibility/webassembly.md](feasibility/webassembly.md): running the runtime as WebAssembly, or
  hosting WebAssembly modules.
- [feasibility/libroadrunner.md](feasibility/libroadrunner.md): the simulation service backed by
  libRoadRunner: the options compared, upstream facts about libRoadRunner 2.11.0, the alignment
  with `ode-uq-spec`, platforms and licensing, and compiling module code through libRoadRunner
  (level C, future work).
- [feasibility/libantimony.md](feasibility/libantimony.md): the model conversion service backed by
  libAntimony: the options compared, upstream facts about libAntimony 3.3.0 and the libraries it
  links, what the build corrected in the first design, the alignment with `ode-uq-spec`, platforms
  and licensing, and future work (hierarchical SBML, CellML, WebAssembly).

The WebAssembly document is a design only: nothing it proposes exists in this version. Levels A and
B of the simulation service are built ([specs/simulation-service.md](specs/simulation-service.md));
level C is not. The model conversion service is built
([specs/model-conversion-service.md](specs/model-conversion-service.md)); the future work of its
feasibility document is not.

## Decisions and open questions

- [Architecture decision records](adr/README.md): index and status rules.
- [ADR template](adr/template.md)
- [Open-questions register](open-questions.md)
