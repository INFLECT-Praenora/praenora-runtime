# ADR-0027: Simulation service v0 and the libRoadRunner provider

- Status: Proposed
- Date: 2026-11-16
- Supersedes: none
- Superseded by: none

## Context

[ADR-0021](ADR-0021-runtime-services-through-host-api.md) planned a simulation service,
`praenora.simulation@0`, backed by libRoadRunner, and left the shape of its API open (OQ-16: one
run or a batch, how results are encoded). [ADR-0026](ADR-0026-runtime-service-providers.md) decided
where such an engine lives: in a service provider that the runtime loads on demand, one engine per
process, reached by modules only through `get_service`.

The modules that will use the service differ in what they need from it:

- some run one time course now and then, and want a simple, self-describing request and result;
- estimation and uncertainty work runs the same model thousands of times with different parameters,
  where parsing and writing JSON for every run would cost more than the run, and where a run that
  diverges is an expected outcome, not an error of the module;
- some modules bring integrators of their own (for example for sensitivities or stochastic
  methods) and need the model's right-hand side as a function, not time courses.

The engine planned behind the service, libRoadRunner 2.11.0, compiles a model with LLVM into native
code, raises C++ exceptions for numerical failures, gives no thread-safety guarantee for one
instance, and offers two JIT backends, of which LLJit targets the host CPU and fuses floating-point
operations, so its results can differ between machines. Compiling module-supplied code into the
engine's model would need a hook the engine does not have.

## Decision

- **The service.** `praenora.simulation@0`, version 0.0, is a public C99 table declared in
  [`include/praenora/sdk/simulation.h`](../../include/praenora/sdk/simulation.h), installed with the
  module SDK (0.2.0), and specified in [simulation-service.md](../specs/simulation-service.md). It
  follows the conventions of the module ABI: fixed-width integers, pointers, length-delimited UTF-8
  and module-owned sinks; no exception crosses it.
- **Two levels of access.** Level A simulates: `simulate` takes one JSON request and writes one JSON
  result, and for many runs `prepare` fixes everything of a run but its numbers in a **plan**, which
  `run` and `run_batch` execute with module-owned `double` buffers, batches in order. Level B hands
  out a **kernel**, a table of C functions over the compiled model (`rhs`, `set_parameters`,
  `initial_state`, `quantities`), for modules with their own integrators. Both levels work on a
  **session**, one opened model.
- **Documents.** Requests and plans use the field names of the simulation configuration of
  `ode-uq-spec` 0.0.0 (`time_domain`, `initial_conditions`, `parameter_values`, `solver`, `output`),
  numbers only. Unknown fields are refused with `INVALID_ARGUMENT` and features the provider does
  not map with `UNSUPPORTED`, each with a JSON pointer: nothing is ignored. Results are canonical
  JSON.
- **Numerical failures are data.** A run that fails numerically (`integrator`, `step_limit`,
  `nonfinite`) does not fail the call: `simulate` returns OK with `{"ok": false, "error": ...}`,
  and `run` and `run_batch` return OK with the outcome `PRAENORA_SIMULATION_RUN_FAILED` for the run.
- **Objects and threads.** Sessions, plans and kernels are allocated and freed by the provider;
  plans and kernels keep the compiled model alive on their own; a module closes everything before
  its `destroy` returns. A session and a plan serve one call at a time (`INVALID_STATE` otherwise),
  distinct objects may be used concurrently, and a kernel by one thread at a time. Calls log what
  they can attribute to themselves through the calling instance's host API and poll its
  cancellation; kernel functions never do.
- **Parameters, initial values and events.** Parameter values take effect as the model's initial
  values (initial assignments are evaluated with them; then initial conditions replace state
  values). Level A applies the model's events; a kernel is the continuous right-hand side only, so
  `open_kernel` refuses a model with events and `describe_model` reports whether a model has any.
- **Engine identity.** `describe` reports the provider manifest's `engine`, which the run
  fingerprint records ([runtime-services.md](../specs/runtime-services.md#fingerprint)): results
  computed through the service are reproducible for the same engine identity.
- **Checks and a reference provider.** A parameterized suite of service checks tests any provider of
  the service against this specification; the fixture provider `test-simulation` implements every
  rule for a toy model format, so the service, the checks and modules that use it run in every
  build.
- **The libRoadRunner provider** (`roadrunner`) is built only with the build option
  `PRAENORA_WITH_ROADRUNNER` (OFF by default), never fetched: it links libRoadRunner 2.11.0's static
  library PRIVATE, with hidden visibility and an export list holding only the provider entry point,
  and uses libRoadRunner's C++ API (never its C API, whose last error is process-wide). Its
  `start()` sets libRoadRunner's process-wide configuration once: the MCJit backend (no host-CPU
  targeting, standard floating-point fusion) and logging routed or silenced. Its engine identity is
  `{"name": "libroadrunner", "version", "llvm", "jit": "mcjit", "llvm_optimization": "none"}` plus
  whatever else the build pins. Each session, plan and kernel owns its own `rr::RoadRunner`, copied
  from a compiled template held in a bounded, least-recently-used cache keyed by the SHA-256 of the
  model text, the load options and the engine identity; kernels use the copy's
  `rr::ExecutableModel`. It passes libRoadRunner only model text (starting with `<` after an
  optional byte order mark and white space), and refuses `<!DOCTYPE` and SBML documents that require
  a Level 3 package.
- **Deferred: compiling through the engine (level C).** Linking module-supplied code into the
  engine's compiled model is future work: it needs an upstream hook in libRoadRunner
  ([feasibility/libroadrunner.md](../feasibility/libroadrunner.md), OQ-28).

### Builder notes (Proposed)

- **Header.** Beyond the declarations of the table and the kernel, `simulation.h` defines
  `PRAENORA_SIMULATION_V0_BASE_SIZE` and `PRAENORA_SIMULATION_KERNEL_V0_SIZE` (the v0.0 sizes,
  frozen like `PRAENORA_HOST_API_V0_BASE_SIZE`) and the size limits as macros, and its comments
  settle the kernel's size handshake: the caller sets `struct_size` to its own size (at least the v0
  size), the provider fills the fields both structs have and sets `struct_size` to the smaller size.
  The header is compiled as C99 into the unit tests (`SimulationAbi.*`) and included by the install
  test's consumer module.
- **SDK C++ adapter.** `praenora::sdk::Host::api()` returns the host API itself, so a module written
  with the adapter can pass it to service functions; C++ only, no ABI change. The SDK has no C++
  wrapper of the service in this version.
- **Host.** `host` MAY be NULL (no logging, no cancellation), for code that has no host (tests,
  helper libraries) or that wants no cancellation; a module's own threads can pass the instance's
  host API, whose callbacks are thread-safe. A call checks its arguments, then takes its session or
  plan, then polls `cancel_requested()`, then reads its document; it holds its object from then
  until its work on it is done, also while it logs or polls. Busy objects are refused at once,
  without waiting. A call on a plan does not hold the plan's session.
- **Defaults.** Empty option bytes count as `{}` (`open_session`, `open_kernel`); the first of
  `describe`'s `model_formats` and `methods` is the default; a kernel without a `parameters` or
  `quantities` list gets all of `describe_model`'s, in its order, and `[]` gets none; a new kernel
  has the model's original parameter values.
- **Error details.** Every error document has `details.service`; document problems add
  `details.pointer` (`""` for the whole document), unknown or repeated ids `details.id`, refused
  model texts `details.reason` `"model"`, engine failures `details.engine_error` `{"type",
  "message"}`, refused counts the counts. A call that fails hands out nothing: out-pointers are set
  to NULL, and so is the kernel's `model`; `close_kernel` sets `model` to NULL.
- **Limits.** Model text 64 MiB; each JSON document 16 MiB, checked before parsing; one run's output
  at most 2^22 values (rows times columns), in `simulate` and `prepare`. A batch's buffers are the
  module's: only its products must fit in 64 bits.
- **Requests.** `checks` is accepted with `true` values only (both checks are always on);
  `inputs`, `events_enabled`, `solver.event_time_tolerance`, `output.save_at` `"target_times"`
  and `output.dense_output: true` are `UNSUPPORTED`. The solver settings are `method`,
  `stiffness`, `relative_tolerance`, `absolute_tolerance`, `max_steps`, `max_step`, `min_step` and
  `seed` (required by stochastic methods, never defaulted); a known setting the chosen method does
  not map is `UNSUPPORTED`, an unknown name `INVALID_ARGUMENT`. Output times are strictly increasing
  within the time domain; a regular grid has the times `start + i * step` for
  `i <= floor((end - start) / step + 1e-9)`, the last at most `end`; `solver_steps` exists only in
  `simulate`. `times` and `step` belong to their `save_at` only.
- **Results.** Columns are `time`, the states (with `include_states`, default true) and the
  requested quantities; the time column holds the output times exactly. `solver` reports the
  settings used, after defaults, under the request's names rather than the engine's. A failed
  result holds only `ok` and `error`; failure documents MAY add details (the test provider adds the
  column and time of the first value that is not finite).
- **Plans.** A plan's inputs are `parameters`, then `initial_conditions` (arrays of ids); an id
  appears once among them; `parameter_values` is unknown in a plan. `prepare` writes the plan's
  `inputs`, `columns`, `rows` and `solver` before it hands the plan out, so a refused write leaves
  no plan.
- **Runs.** Counts must match exactly; input values must be finite (`INVALID_ARGUMENT` with
  `details.index`), in a batch all of them before any set runs. A failed run's output is quiet NaN;
  its failure document is the `error` of a failed `simulate` result; `failure_json` may be NULL but
  not a sink without `write`. After a status other than OK, outcomes and outputs are unspecified. A
  batch writes no failure documents and polls before each set. The same settings and values give the
  same doubles in `simulate`, `run` and `run_batch`, on any thread and in any process of a build.
- **Evolution.** A later minor may append functions and kernel fields and add optional document
  fields and members; it never changes what exists.
- **Parameter values as initial values** (both levels): a request's `parameter_values`, a plan's
  parameter inputs and a kernel's `set_parameters` replace the model's values before anything is
  computed from them, so the model's initial assignments are evaluated with them; then a request's
  `initial_conditions` (or a plan's initial-condition inputs) replace state values at `start`. This
  matches what an SBML engine does when initial values are set and the model is reset without
  restoring its original values. The test format `test-decay` lets a state's initial value name a
  parameter, so the rule is checked in every build. The initial assignments are not evaluated again
  with the initial conditions: a state whose initial value is defined from another state keeps the
  value computed from that state's original value when a request sets only that state (setting
  current values after the reset, not initial values).
- **Kernel results.** `rhs`, `quantities` and `initial_state` depend only on their arguments and the
  kernel's current parameters; no call changes what a later call computes except `set_parameters`,
  which also brings values derived from parameters (initial assignments, compartment sizes) up to
  date. A kernel function returns `INTERNAL` when the engine fails unexpectedly (an engine kernel
  catches the engine's exceptions); outputs are then unspecified.
- **Events, stricter than optional.** `describe_model` MUST report `"events": true|false`. Level A
  applies a model's events as the engine defines them; this version cannot turn them off
  (`events_enabled` stays `UNSUPPORTED`). A kernel cannot apply events (an engine's right-hand side,
  such as libRoadRunner's `getStateVectorRate`, is the continuous part only), so `open_kernel` MUST
  refuse a model with events with `UNSUPPORTED` and `details.reason` `"events"`, rather than give a
  module silently wrong trajectories. `test-decay` can declare events (`{"time", "state",
  "value"}`) that `test-simulation` does not simulate: its `simulate` and `prepare` refuse such a
  model with `UNSUPPORTED`, reason `"events"`, a property of the toy format only.
- **`max_steps` and `step_limit`.** `max_steps` is the method's limit on solver steps in the
  method's own scope, which the provider documents: CVODE limits the steps per output interval, and
  libRoadRunner integrates one output interval at a time. A run ends with reason `step_limit` only
  when the provider can tell the limit ended it (an engine may report it only in an error text);
  otherwise the reason is `integrator`.
- **Engine logging.** What an engine logs through its own process-wide logger cannot be attributed
  to the calling instance, so the provider routes it to its own logger or silences it, once, when
  it starts; only records a call can attribute to itself go through that call's host.
- **Setting values.** A value of a mapped setting that the method cannot honor (for example
  `stiffness` `"auto"` for a method that does not switch by itself) is `UNSUPPORTED`; a provider
  never substitutes a value silently.
- **Output times.** A regular grid whose times do not strictly increase (a step below the
  precision of the start time) is `INVALID_ARGUMENT` at `/output/step`. A `solver_steps` run that
  reaches the output limit while it runs is stopped with `RESOURCE_EXHAUSTED`.
- **Smaller rules.** A call holds its object until its work on it is done (it may release it before
  writing its error document); a call with no work (a batch of no sets) need not poll; a refused
  input value's `details.index` is its index in `values`, across a batch's sets; a call ended by a
  refusing sink returns that sink's status, whatever its value.
- **Test provider.** `test-simulation` is C++17 with nlohmann/json
  (`praenora_add_fixture_provider()` gained `LINK_LIBRARIES`; `provider_fixture.h` is usable from
  C++). Its format `test-decay` lists states and parameters in id order, quantities as the states
  and `total`, lets a state's initial value name a parameter, can declare events, and solves each
  state's decay exactly with the method `analytic`, which maps no solver setting.
- **Service checks.** `tests/contract/simulation_service_checks.hpp` declares the parameter set
  `SimulationCase` (provider manifest, model format and text, states, parameters and their values,
  quantities, initial state and optionally the initial state as a function of the parameters,
  solver settings, exact solution, derivative and quantities, failing parameter values, end time,
  tolerances, and optionally a model with events) and the suite `SimulationService`, whose 32
  checks are in `simulation_service_checks.cpp`; each provider instantiates the suite in a source
  file of its own (`TestSimulation` with the parameter sets `TwoStates`,
  `ThreeStatesSomeParameters` and `StateFromParameter`, whose initial state is a parameter). The
  checks require the same doubles from `simulate`, `run` and `run_batch` and the same failure
  reason from `run` and `simulate`; the case's model has no events.
- **Fixture module.** `simulating-module` (capability `test.echo@0.1.0` of the test catalog,
  `REQUIRES_SERVICES praenora.simulation@0.0`) gets the table in `create`, opens a session and
  prepares its plan in `initialize`, closes the session at once (the plan outlives it), optionally
  keeps a kernel's `dy/dt` at the start, runs the plan in `invoke` with the values of its input or
  parameters, and ships test vectors (plans, a kernel, a numerical failure) that the contract checks
  run through `test-simulation`.

- **The roadrunner provider: build.** `PRAENORA_WITH_ROADRUNNER` (OFF) in
  `cmake/PraenoraOptions.cmake`; `cmake/PraenoraRoadrunner.cmake` finds `roadrunner-static` and only
  the packages its link interface names (zlib's static component, expat, libSBML, rr-libstruct,
  CLAPACK, NLEQ1, NLEQ2, POCO Foundation, Net and XML, SUNDIALS), searched first beside
  `roadrunner-static`'s package files, not the whole set of libRoadRunner's
  `ImportRoadrunnerAndDependencies.cmake` (bzip2, Threads, LibLZMA and the shared packages are
  not needed). LLVM comes from `LLVM_INSTALL_PREFIX` (else an `llvm-config` on `CMAKE_PREFIX_PATH`,
  searched again at each configuration, never cached), whose `llvm-config --version` names it.
  Each missing piece is a `FATAL_ERROR` that names it. Versions: libRoadRunner 2.11.x only (the
  provider relies on its internals: configuration keys, how reset and `simulate` treat events), with
  a status line when it is not 2.11.0, the version checked; LLVM 13, which libRoadRunner 2.11 is
  built against (another LLVM's libraries of the same names would link but not work); anything else
  is a `FATAL_ERROR` naming both versions.
  Three repairs are made on the imported targets, never in the installation: the absolute LLVM
  library paths of upstream's build machine in `roadrunner-static`'s link interface are replaced
  by the libraries of the same name in the LLVM installation (a missing one is a `FATAL_ERROR`);
  on macOS the system libraries `m`, `dl` and `pthread` (all part of libSystem) are removed from
  the link interfaces, so the linker does not warn about duplicate libraries; zlib is asked for its
  static component. The provider's version is the runtime's; its engine object adds the SUNDIALS
  and libSBML versions to the fields above, and the build checks libRoadRunner's headers against
  the version the manifest names. In the build tree the provider goes where the build tree's
  runtime looks for the providers of its installation, so the tool and the runtime find it there
  without a provider path; it is installed to `<libdir>/praenora/providers/roadrunner`. A build
  tree configured again without the option removes that directory (only below
  `CMAKE_BINARY_DIR`), so it does not keep finding the provider.
- **The roadrunner provider: models.** The format is `sbml`, Levels 1, 2 and 3 (Level 1 was
  decided by a probe: a Level 1 Version 2 decay model opens, its ids come from the `name`
  attributes, and it simulates to the exact solution, so it is accepted). Text checks before any
  parser: a first `<` after an optional byte order mark and white space, no NUL, no `<!DOCTYPE`
  (any case), and libRoadRunner's own test of document text, `rr::SBMLReader::is_sbml` (a copy of
  it missed texts such as a document followed by a comment that holds "?xml?" and a `<`, which
  libRoadRunner then opened as a URI). Then libSBML reads it: any error is a refusal (an unknown
  required package is one); in Level 3 only (packages exist there only; libSBML lists the
  namespaces of its own Level 2 annotations as required), a required package libSBML knows and any
  declaration of `comp`; and a flux-balance model that libRoadRunner's `simulate` would refuse
  (the `fbc` package with its required attribute set, no kinetic law with math: libRoadRunner's
  own test), so that such a model is refused at `open_session` rather than every run failing. Ids:
  states are libRoadRunner's state vector with conserved-moiety analysis off (rate-rule variables,
  then floating species, amounts); parameters are global parameters without assignment rule, rate
  rule or initial assignment, and not used by an initial assignment of a species reference or a
  Level 2 `stoichiometryMath` (libRoadRunner computes such stoichiometries once, at load);
  quantities are the states, the other floating species' amounts, `[<species>]` concentrations,
  reaction rates by reaction id and assignment-rule parameters, in that order. Boundary species and
  compartment sizes are neither parameters nor quantities in this version, so a module can neither
  set nor read them; adding them is future work for a later minor (pharmacokinetic models often
  take boundary species as inputs).
- **The roadrunner provider: runs.** One method, `cvode`. Settings: `stiffness` stiff (default) or
  nonstiff, `"auto"` `UNSUPPORTED`; tolerances default to CVODE's in libRoadRunner (1e-6, 1e-12);
  the absolute tolerance is passed as one value per state, which libRoadRunner uses as given (a
  single value it would scale by each state's initial value); `max_steps` is CVODE's per output
  interval (default 20000, at most 2^31 - 1); `max_step` and `min_step`; `seed` and
  `event_time_tolerance` `UNSUPPORTED`. Each run: libRoadRunner's reset with SBML initialization,
  the random generator seeded with 0, parameter values as initial values, a reset without SBML
  initialization, initial conditions as current state values, the settings applied in full,
  `simulate` with explicit times (a first output time after `start` gets `start` before it, whose
  row is dropped; a single output time at `start` gets `end` after it). `solver_steps` runs use
  libRoadRunner's variable-step mode with its row limit set one above the run value limit. Before
  each run the queue of pending event assignments is emptied (libRoadRunner's reset keeps it, and
  its copy constructor queues events that fire at time 0): they are applied at time +infinity with
  the trigger states there, so nothing is queued again, before the reset. Then the integrator is
  restarted at `start` with the model before `start`, so the triggers are evaluated there against
  their initial values: libRoadRunner queues events that fire at the start only in
  `RoadRunner::reset`, at time 0, never in `simulate`, so without this a later run, or a run from a
  later start, would miss them or apply them twice. When the queue cannot be emptied (an event
  cascade beyond libRoadRunner's limit, a delay that is not a number), a fresh copy is drained the
  same way, and if that fails too the run returns `INTERNAL`. After every `solver_steps` run, also
  one ended by the row limit (libRoadRunner's variable-step mode keeps an event or switch found at
  a step's end for the next call, and a restart does not clear it), and after a run that ended
  with an exception, the object is replaced by a fresh copy. A CVODE
  failure is `step_limit` when its text names `CV_TOO_MUCH_WORK`, otherwise `integrator`; every
  other exception during the time course is `integrator`; all carry `engine_error`. Runs poll
  cancellation after each output interval through libRoadRunner's integrator listener.
- **The roadrunner provider: objects and threads.** A cache of up to 16 compiled models, keyed by
  the SHA-256 of the engine object, the options and the model bytes, least recently used first
  out; compiling outside the cache's lock with an in-progress entry that waiters poll. Sessions (for
  `simulate`, made on the first call), plans and kernels each own a copy of the compiled model's
  `rr::RoadRunner`. Making, copying and destroying `rr::RoadRunner` objects, and compiling, take
  one process-wide lock (libRoadRunner keeps unguarded counters and global caches there), which
  calls wait for while polling cancellation; its cost: `close_*` (without polling) and the first
  `simulate` of a session, `prepare` and `open_kernel` may wait for another call's compilation of
  any model. Runs and kernel functions take no lock.
- **The roadrunner provider: process-wide settings.** At `start()`, every key of `rr::Config` is
  set to libRoadRunner 2.11.0's built-in default (the build fails when a libRoadRunner has keys
  this list does not name), so a user's `roadrunner.conf` changes nothing, except `RANDOM_SEED` 0;
  `LLVM_BACKEND` MCJit. Load options: MCJit, no optimization passes, no conserved moieties,
  mutable initial conditions, LLVM's symbol cache (libRoadRunner's default). libRoadRunner's logger
  is set to the error level and writes, through a stream, to the provider's logger one level lower
  than its own (errors as warnings, critical and fatal as errors), since libRoadRunner logs an error
  for each CVODE failure that the service reports as data: the engine's records are context, the
  service's results and error documents are what count. Colour sequences are removed, and a failed
  write drops only its own line.

## Alternatives considered

- **JSON only.** Simple, but each run of a fit would parse a request and write a result; plans
  and batches move only numbers.
- **Batches only.** A single run is then a batch of one, but a module that simulates once, or
  wants a self-describing result, loses the JSON call.
- **Numerical failures as errors.** Every failed parameter set would fail the module's call, and
  modules would have to tell their own errors from the engine's.
- **A C++ interface in which modules link libRoadRunner.** Rejected for now: libRoadRunner's
  installed headers for its LLVM parts are incomplete, its classes are not exported on Windows, and
  modules would have to match the engine's exact build, breaking ABI v0's promise that a module may
  use any compiler.
- **LLJit.** Faster to compile, but it targets the host CPU and fuses floating-point operations, so
  results could differ between machines with the same fingerprint.
- **Level C now.** It needs an upstream hook to link module code into the model before
  optimization; deferred.

## Consequences

- Modules simulate models without linking an engine, from a single run to large batches and their
  own integrators, and a fit can treat diverging parameter sets as data.
- Every provider of the service must implement the same rules (busy detection, cancellation, limits,
  error documents); the service checks test them, and the test provider shows them without an
  engine.
- The run fingerprint changes with the engine, and results are reproducible for one engine identity.
- The libRoadRunner provider is built only with `PRAENORA_WITH_ROADRUNNER` and an installed
  libRoadRunner 2.11 with the LLVM it was built with (checked on macOS arm64 with the upstream
  binaries of 2.11.0 and LLVM 13.0.1); other builds have only the test provider, and modules that
  require the service cannot run there (`SERVICE_UNAVAILABLE`). Its library is large (on macOS
  arm64, about 48 MB in a Debug build, about 37 MB stripped), which is why it is a provider of its
  own.
- This answers the shape question of OQ-16 (single run, plan, batch, kernel).

## References

- [simulation-service.md](../specs/simulation-service.md): the normative specification.
- [ADR-0021](ADR-0021-runtime-services-through-host-api.md): runtime services through the host API.
- [ADR-0026](ADR-0026-runtime-service-providers.md): service providers.
- [runtime-services.md](../specs/runtime-services.md): providers, resolution and the fingerprint.
- [feasibility/libroadrunner.md](../feasibility/libroadrunner.md): the libRoadRunner facts, the
  alignment with `ode-uq-spec` and level C as future work.
- [open-questions.md](../open-questions.md): OQ-16 (the shape of the service), OQ-27 (building
  and distributing the engine) and OQ-28 (level C).
