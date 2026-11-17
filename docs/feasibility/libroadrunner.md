# libRoadRunner simulation service: upstream facts and future work

- **Status:** Informative. Levels A and B of the simulation service, simulating and calling the
  compiled model, are specified in [simulation-service.md](../specs/simulation-service.md),
  [`simulation.h`](../../include/praenora/sdk/simulation.h) and
  [ADR-0027](../adr/ADR-0027-simulation-service-v0.md), and built: the service provider
  `roadrunner` ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md),
  [runtime-services.md](../specs/runtime-services.md)), built only with the build option
  `PRAENORA_WITH_ROADRUNNER=ON` ([`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake)).
  Level C, compiling module code through libRoadRunner, is future work: neither specified nor
  built ([Level C](#level-c-compiling-through-roadrunner-future-work), OQ-28). Where this document
  and the specifications differ, the specifications hold.
- **Decision records:** [ADR-0026](../adr/ADR-0026-runtime-service-providers.md) and
  [ADR-0027](../adr/ADR-0027-simulation-service-v0.md) (Proposed), which amend
  [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md); the open questions OQ-16, OQ-27
  and OQ-28 in [open-questions.md](../open-questions.md).
- **Sources:** facts from upstream documentation were checked on 2026-11-11 against libRoadRunner
  2.11.0 and are cited as [R1] to [R32]. Facts about the 2.11.0 release binaries and source were
  checked on 2026-11-16, when the provider was built, and are cited as [R33] to [R35]; [R36], an
  upstream issue, was not read in this build (unverified). All are listed under
  [Sources](#sources). A claim that could not be checked is marked "(unverified)".

## What this document holds

It began as the feasibility design of a libRoadRunner-backed simulation service. The service is now
specified and built, so this document keeps what the specifications do not hold: the options that
were compared, facts about libRoadRunner 2.11.0 and its ecosystem (each with where it was checked),
the alignment with `ode-uq-spec`, platforms, licensing, the WebAssembly outlook, the design of
level C as far as it is known, and the risks and checks that remain.

What the specifications own, and where to find it:

- The service table, sessions, plans, batches and kernels:
  [`simulation.h`](../../include/praenora/sdk/simulation.h) and
  [simulation-service.md](../specs/simulation-service.md).
- Requests, results and numerical failures as data:
  [simulation-service.md](../specs/simulation-service.md#level-a-simulate).
- Compiled-model caching, objects and threads in the provider:
  [simulation-service.md](../specs/simulation-service.md#the-roadrunner-provider) and ADR-0027's
  builder notes.
- Errors and how engine failures map to them:
  [simulation-service.md](../specs/simulation-service.md#errors) and its roadrunner section.
- Providers, their discovery, loading and lifetime, and the engine in the run fingerprint:
  [runtime-services.md](../specs/runtime-services.md) and
  [ADR-0026](../adr/ADR-0026-runtime-service-providers.md).
- libRoadRunner's C++ API rather than its C API, the MCJit backend and the rejected C++ tier:
  [ADR-0027](../adr/ADR-0027-simulation-service-v0.md).
- Building the provider: [platforms.md](../platforms.md#the-roadrunner-provider) and
  [developer-guide.md](../developer-guide.md#the-roadrunner-provider).

## Options compared

| Option | Summary | Outcome |
|---|---|---|
| S1. Compiled into the runtime | `PRAENORA_WITH_ROADRUNNER=ON` links `roadrunner-static` PRIVATE into `praenora_runtime`. | Rejected ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md)): every runtime built with the option would carry and load LLVM, SUNDIALS and libSBML, also when no run needs them, and the runtime's build would depend on the engine's toolchain. The upstream C++ archives alone are 160 to 477 MB ([LLVM and JIT footprint](#llvm-and-jit-footprint)). |
| S2. A service provider | A separately built library, shipped with the runtime, that the runtime loads only when a run needs its services: one engine per process. | Chosen and built ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md)): the provider `roadrunner`. |
| S3. Each module bundles libRoadRunner | Possible: nothing stops a module from linking it. | Not supported: every such module carries the whole dependency set, gives up ABI v0's freedom of compilers, and the runtime cannot record which engine ran. A module that links the shared library brings the LLVM it exports into the process, beside any other LLVM there [R36]. |
| S4. Out-of-process simulation | A helper process that owns libRoadRunner. | Not needed yet. It would isolate crashes (a crash in the engine would not end the run's process; OQ-5), at the cost of serializing every call. |
| S5. Modules link libRoadRunner's C++ API | An optional C++ tier beside the C service table. | Rejected for now ([ADR-0027](../adr/ADR-0027-simulation-service-v0.md)): the installed `rr/llvm` headers do not compile ([Build and link](#build-and-link)), libRoadRunner's classes are not exported on Windows (unverified here), and modules would be pinned to one exact build of the engine. |

## libRoadRunner 2.11.0: checked facts

Building and testing the provider on macOS arm64, with the upstream release binaries of
libRoadRunner 2.11.0 [R33] and LLVM 13.0.1 [R34], and reading libRoadRunner 2.11.0's source [R35],
settled the following. The binaries hold SUNDIALS 7.9.0, libSBML 5.21.2 and POCO 1.14.2.

### Build and link

- **LLVM 13.0.1.** The 2.11.0 binaries are built with LLVM 13.0.1: the shared `libroadrunner.dylib`
  holds the string "LLVM version 13.0.1", the static library's CMake package names 51 LLVM
  libraries in a directory `llvm-13.x-binaries` of upstream's build machine, and the matching LLVM
  archive's `llvm-config --version` prints 13.0.1 [R33][R34]. The installation guide's commands use
  a prebuilt LLVM 23.1.4 [R5]; the release binaries do not.
- **LLVM inside the shared library, exported.** `libroadrunner.dylib` links LLVM statically (it
  depends only on libSystem and libc++) and exports LLVM's symbols: of its 46,603 exported symbols
  (`nm -gU`), about 19,100 are in the namespace `llvm` by their mangled names (functions and data,
  vtables and type information, thunks), and 123 more are LLVM's C API (`LLVM...`) [R33]. A process
  that loads it beside another LLVM holds two copies of LLVM [R36]. This is why the provider links
  the static library, `roadrunner-static`, behind an export list that keeps every LLVM symbol
  inside the provider.
- **No code-generation hook.** The model generator, `rrllvm::LLVMModelGenerator`, makes executable
  models from SBML documents (`createModel`, `regenerateModel`) [R33]; both generate a model's LLVM
  module and add it to the JIT in one function, with no point in between where other code could be
  linked in [R35].
- **Incomplete `rr/llvm` headers.** The installation holds 49 headers in `include/rr/llvm`; 28 of
  them include `LLVMIncludes.h`, `rrCompiler.h` or `SymbolForest.h`, which are not installed, so
  `rr/llvm/LLVMModelGenerator.h`, for example, does not compile on its own [R33]. This is one
  reason the C++ tier (S5) is rejected.
- **Packaging.** `roadrunner-static` links into a `MODULE` library behind an export list without a
  warning once three packaging defects are repaired on the imported targets: its link interface
  names LLVM's libraries by the absolute paths of upstream's build machine; several packages list
  `m`, `dl` and `pthread`, which are part of libSystem and make the macOS linker warn about
  duplicate libraries; and zlib's package must be asked for its static component. Its package files
  do not find their dependencies either, so the build finds each package the static library links
  (libRoadRunner's `ImportRoadrunnerAndDependencies.cmake` also finds packages the static library
  does not use). The provider library needs only libSystem and libc++ and exports only the provider
  entry point; its size is under [LLVM and JIT footprint](#llvm-and-jit-footprint).
- **macOS 14.** The macOS arm64 binaries, shared and static, are built for macOS 14.0 (SDK 14.5),
  so a build that links them needs a deployment target of at least 14.0 [R33].

### JIT backends and configuration

- **MCJit and LLJit.** MCJit is created with LLVM's `EngineBuilder` defaults: no CPU name and no CPU
  features, so code for a generic CPU of the process's architecture, and LLVM's default
  floating-point fusion. LLJit targets the host CPU (`JITTargetMachineBuilder::detectHost`) with
  fast floating-point fusion [R35]. The same model can therefore give other results on another CPU
  with LLJit; the provider pins MCJit
  ([ADR-0027](../adr/ADR-0027-simulation-service-v0.md)). MCJit is the default backend; LLJit
  compiles faster and offers optimization levels [R14][R15].
- **`LLJIT_NUM_THREADS`** defaults to 1 in 2.11.0 [R35]; the documentation says 0 [R15].
- **Process-wide state.** `rr::Config` reads a configuration file on its first use
  (`ROADRUNNER_CONFIG`, `roadrunner.conf` or `.roadrunner.conf` in the home directory, and beside
  the library or one level up) and keeps its values in a static array read without a lock;
  `rr::Logger` writes through POCO, at the error level by default, to a stream that can be replaced,
  and is configured through static functions [R19]; making, copying and destroying models touches
  unguarded counters [R35]. The provider sets every configuration key once, when it starts, and
  makes, copies and destroys libRoadRunner objects one at a time.
- **The C API** keeps its last error in a process-wide variable [R8][R9], one reason the provider
  uses the C++ API.

### Model texts

- **Text or path.** The `RoadRunner` constructor and `load()` accept "a URI, local path, or document
  contents" [R10]. libRoadRunner reads a string as document text only when its own test,
  `rr::SBMLReader::is_sbml`, says so; any other string is read as a file path or a URI, with POCO's
  HTTP stream factory registered [R35]. The test looks for "sbml" after a `<`, but when the text
  holds `<`, then "?", "xml", "?", `>` and `<` in that order (an XML declaration, or a comment or
  processing instruction that holds them), only after that last `<`: a valid SBML document without
  an XML declaration, followed by such a comment, is read as a URI. The provider applies that test
  itself, after its own checks, and passes only text that passes it.
- **Packages.** libRoadRunner flattens a text that declares the `comp` package (its version 1
  namespace), which reads the external documents the model names, by URL, URN or file [R30][R35];
  the packages qual, spatial and multi make it throw `std::domain_error` [R35]. The provider refuses
  every Level 3 document that declares `comp` or requires a package.
- **SBML Level 2.** libSBML lists the namespace of its own Level 2 layout annotation
  (`http://projects.eml.org/bcb/sbml/level2`) as required in every Level 2 document it reads, so a
  rule that refuses required packages refuses every Level 2 document unless it is limited to
  Level 3, where packages exist. libRoadRunner computes a Level 2 `stoichiometryMath` of constant
  values once, when it loads the model, so the parameters it uses keep their first values in the
  stoichiometry; the provider does not offer them as parameters.
- **SBML Level 1.** A Level 1 Version 2 decay model opens, its ids come from the `name` attributes,
  and it simulates to the exact solution; the provider accepts Level 1.
- **Models it cannot simulate.** `RoadRunner::simulate` refuses a flux-balance model: a document
  with the `fbc` package, its `required` attribute set, whose reactions have no kinetic law with
  math (`RoadRunnerImpl::isCompleteFBC`) [R35]. The provider applies the same test when it opens the
  model, so such a model is refused at `open_session` rather than in every run.
- **Load failures** are exceptions derived from `std::exception` of several types:
  `rrllvm::LLVMException` (a `std::runtime_error`), `std::domain_error` and others, not always an
  `rr::Exception` [R11][R35].

### Runs, resets and events

- **Original state.** `ExecutableModel::reset(SelectionRecord::ALL)` restores the model as loaded,
  initial values included; a reset without `SBML_INITIALIZE` evaluates the initial assignments with
  the current initial values [R35]. The C API's `resetToOrigin` "discards any initial values set"
  [R8].
- **Pending events survive a reset.** Neither reset clears the queue of event assignments still
  pending (the assignments of delayed events); `LLVMExecutableModel::resetEvents()` is empty, and no
  API clears the queue; applying the pending assignments at time +infinity empties it [R35].
- **Events at the start.** `RoadRunner::simulate` sets the model's time to the start before it
  restarts the integrator there, so the triggers are compared at the start with their own values
  there, and an event whose trigger is true at the start never fires. Only `RoadRunner::reset`,
  which restarts the integrator at time 0 with the model before it, queues such events, with delays
  counted from 0 [R35]. So, with plain libRoadRunner: a freshly loaded model simulated once from 0
  gets them right; a second run after `ExecutableModel::reset` never fires them; a copy, whose
  constructor queues them, fires them twice; `RoadRunner::reset` between runs alternates; and a run
  from a later start counts their delays from 0. The provider empties the queue before each run and
  restarts the integrator at the run's start itself
  ([simulation-service.md](../specs/simulation-service.md#the-roadrunner-provider)). These are
  upstream behaviors worth reporting to libRoadRunner.
- **Copies.** `RoadRunner(const RoadRunner&)` saves the model's state and loads it into a new
  object, reloading the generated code without compiling again; copies of one compiled model give
  the same doubles.
- **Tolerances.** CVODE's default tolerances in libRoadRunner are 1e-6 (relative) and 1e-12
  (absolute), and its default step limit 20000; a single absolute tolerance is scaled by each
  state's initial value (or compartment size), a vector of one value per state is used as given.
- **Step limit.** `maximum_num_steps` is CVODE's limit per call of `CVode`: per output interval in
  a run with output times, one step per call in variable-step mode, where it never ends a run.
- **Failures.** CVODE errors are `rr::IntegratorException`, a `std::runtime_error` (the installed
  `Integrator.h` [R33]), whose text names the CVODE flag (`CV_TOO_MUCH_WORK` for the step limit).
- **Variable-step output.** In variable-step mode `simulate` gives a row at the start, one after
  each step and one at the end, interpolated, up to the integrator's `max_output_rows`.
- **Randomness.** Each model has its own random number generator, seeded from `rr::Config` when it
  is made or copied (-1 means from the clock); libRoadRunner draws from it the order of simultaneous
  events of equal priority. `RoadRunner::setSeed` writes the process-wide configuration, which is
  not safe across threads; `ExecutableModel::setRandomSeed` seeds one model [R35].
- **Cancellation.** An integrator listener's `onTimeStep` is called after each output interval of
  a run (after each step in variable-step mode), after CVODE has returned, so a run can be stopped
  between output times.
- **Threads.** The class reference gives no general thread-safety guarantee [R10], and
  `RoadRunnerMap` builds models with a thread pool [R12][R13]. Distinct libRoadRunner objects
  simulate concurrently in the provider's service checks, also under ThreadSanitizer, which
  instruments only the provider's own code.

Not settled by one machine: whether MCJit's code, and so the results, are the same on every CPU of
one architecture.

## Alignment with the ode-uq-spec simulation configuration

Requests use the field names of an `ode-uq-spec` 0.0.0 simulation configuration
(`docs/specification.md`, Simulation configuration, and `schemas/simulation.schema.json` in that
specification), numbers only, so a module can pass the numeric parts of one through. Expressions in
`ode-uq-spec` documents are strings of SBML Level 3 infix math that must never be executed
(`conventions.md`, Security); the service evaluates none. The second column summarizes
[simulation-service.md](../specs/simulation-service.md#requests), which is normative.

| `ode-uq-spec` field | `praenora.simulation@0`, and the provider `roadrunner` | libRoadRunner 2.11.0 |
|---|---|---|
| `time_domain.start`, `end` | Same; the run starts at `start` from the initial state. | `simulate` with explicit times [R10]; the provider restarts the integrator at `start` ([Runs, resets and events](#runs-resets-and-events)). |
| `initial_conditions` | Numbers only; they replace state values at `start`, after the parameter values took effect. | The provider sets the state vector after the reset. |
| `parameter_values` | Numbers only; they take effect as initial values, so initial assignments are evaluated with them. | Initial values, then a reset without SBML initialization. The 2.11.0 release notes say `init(x)` can override an existing initial assignment [R2]. |
| `inputs` | `UNSUPPORTED`; a module turns constant inputs into `parameter_values`. | Not used. |
| `solver.method` | One of the provider's methods: `cvode` for `roadrunner`. | Integrators `cvode` (the default), `rk4`, `rk45` and `gillespie` [R17]. |
| `solver.stiffness` | `stiff` or `nonstiff`; `auto` is `UNSUPPORTED` for `cvode`. | CVODE's `stiff` setting [R17]; CVODE does not switch by itself. |
| `solver.relative_tolerance`, `absolute_tolerance` | Same. | CVODE settings of the same names [R17]; the provider passes one absolute tolerance per state. |
| `solver.max_steps`, `max_step`, `min_step` | Same; `max_steps` counts CVODE's steps per output interval. | `maximum_num_steps`, `maximum_time_step`, `minimum_time_step` [R17]. |
| `solver.seed` | Required by stochastic methods, never defaulted; `UNSUPPORTED` for `cvode`. | The Gillespie integrator's `seed` regenerates the same random sequence [R18]. |
| `solver.event_time_tolerance` | `UNSUPPORTED`. | No counterpart among the CVODE settings [R17]. |
| `output.save_at` | `explicit_times`, `regular_grid`, and `solver_steps` in `simulate` only; `target_times` is `UNSUPPORTED` (a module turns it into `explicit_times`). | `simulate` with times; variable-step mode for `solver_steps`. |
| `output.quantities`, `include_states` | Same. | `setSelections` [R10]. |
| `output.dense_output` | `true` is `UNSUPPORTED`. | No counterpart. |
| `events_enabled` | `UNSUPPORTED` when present; level A applies the model's events. | Events are applied while it integrates. |
| `checks.reject_nonfinite`, `enforce_time_domain` | Always on; `false` is `UNSUPPORTED`. | Checked by the provider. |

## Platforms

| Platform | Project presets | libRoadRunner 2.11.0 binaries [R3][R4] | Notes |
|---|---|---|---|
| macOS arm64 | `macos-arm64-*` | `macos-14-arm64` archives; `macosx_14_0_arm64` wheels | Built and tested ([platforms.md](../platforms.md#the-roadrunner-provider)). Built for macOS 14.0 [R33]: a build with the provider needs a deployment target of at least 14.0 instead of the project default 11.0. Building libRoadRunner from source for 11.0 is unverified. A hardened-runtime host needs a JIT entitlement ([Security](#security)). |
| Linux x86-64 | `linux-x64-*` | `manylinux_2_28-x86_64` and `ubuntu-24-x86_64` archives; `manylinux_2_28` wheels | Not built. The provider's export list is a version script there. The manylinux 2.28 tag implies glibc 2.28 or newer (unverified). The README notes that the Linux binaries depend on ncurses [R1]. |
| Windows x86-64 | `windows-x64-*` | `windows-msvc18-x86_64` archives; `win_amd64` wheels | Not built. Microsoft's rule: the final link must use build tools at least as new as the newest that built any input [R29]. If `msvc18` denotes Visual Studio 2026 (unverified), linking the static archives needs its build tools, not the Visual Studio 2022 of the project presets; otherwise build libRoadRunner from source with Visual Studio 2022. |
| macOS x86-64 | none | `macos-15-x86_64` archives and wheels | Not a project platform. |
| Linux arm64, Windows arm64 | none | none listed | Building from source should be possible on platforms LLVM supports (unverified). |

How the engine is built and distributed (upstream binaries or source builds, per platform) is
OQ-27.

### LLVM and JIT footprint

- libRoadRunner uses LLVM to generate code by default [R1]. `LLJIT_OPTIMIZATION_LEVEL` defaults to
  none, which gave two times faster model loads; aggressive optimization makes faster code but
  compiles longer [R15]. The provider loads models without LLVM optimization passes.
- Compiling dominates when many models are loaded, and LLJit was more than three times faster than
  MCJit at loading and simulating over 1,100 BioModels models [R14]; the provider pins MCJit for
  reproducibility and caches compiled models instead.
- Size: the 2.11.0 release archives of the C++ library are about 160 MB (macOS arm64, Release),
  167 MB (macOS x86-64), 238 MB (manylinux 2.28 x86-64) and 477 MB (Windows x86-64, Release), and
  the Windows Debug archive is about 1.3 GB [R3]; the Python wheels are 24 to 51 MB [R4]. The
  provider library, with libRoadRunner, LLVM, SUNDIALS and libSBML linked in, is about 48 MB in a
  Debug build on macOS arm64, about 37 MB stripped (`strip -x`).
- A test process that loads the provider, compiles a small model and runs it a few times takes
  about 1 s on the machine where this was checked. Memory per compiled model, and compile time
  against model size, are not measured.

### WebAssembly outlook

- **Runtime compiled to WebAssembly** ([webassembly.md](webassembly.md), Model A). libRoadRunner
  generates native machine code at run time. WebAssembly code is immutable and not observable at
  run time, and call targets are fixed when a module is loaded [R32], so a JIT inside a WebAssembly
  module cannot run what it generates. No upstream WebAssembly build of libRoadRunner was found, and
  whether it has a backend that needs no code generation (the build has a legacy C option,
  `BUILD_LEGACY_C` [R6]) is unverified. In Model A no provider of the service exists: validation
  refuses a module that requires it (`SERVICE_UNAVAILABLE`), and a module that declares it optional
  gets NULL from `get_service`.
- **WebAssembly modules in a native runtime** (Model B). The provider stays native in the host, and
  its service could be offered to WebAssembly modules as host imports that mirror the table of
  `simulation.h`.

### Licensing facts

These are facts about upstream projects, gathered for the license decision (OQ-1) and for
distribution (OQ-13, OQ-27). This project states no license of its own. libRoadRunner's
dependencies come from its `libroadrunner-deps` repository, most of them as git submodules
[R20][R21].

| Component | License as stated upstream |
|---|---|
| libRoadRunner | Apache License 2.0 [R1][R22][R4] |
| LLVM | Apache 2.0 License with LLVM exceptions; portions embedded in object code by compilation may be redistributed without complying with sections 4(a), 4(b) and 4(d) [R23] |
| libSBML | LGPL 2.1 (the repository page does not say "or later") [R24] |
| SUNDIALS | BSD 3-clause [R25] |
| POCO | Boost Software License 1.0 [R26] |
| NLEQ1, NLEQ2 | No license shown in the mirror repository [R27]; the terms must be established before distribution |
| zlib, bzip2, expat, CLAPACK, rr-libstruct | Not checked (unverified) |

libRoadRunner links libSBML statically [R6], and so does the provider, through `roadrunner-static`.
A distributed binary that contains a statically linked LGPL-2.1 library must, among other
conditions, give users the means to relink it with a modified library, or use a shared-library
mechanism instead (LGPL-2.1, section 6) [R28]. Distributing the provider therefore needs either a
dynamically linked libSBML or relinking materials, and a legal review either way (OQ-27).

## Determinism, threading and security

### Determinism

- Results depend on the libRoadRunner, LLVM, SUNDIALS and libSBML builds, on the JIT backend and
  optimization level, and on the integrator settings. The provider pins the process-wide settings,
  echoes the settings each run used, and reports its engine identity, which the run fingerprint
  records ([simulation-service.md](../specs/simulation-service.md#reproducibility),
  [runtime-services.md](../specs/runtime-services.md#fingerprint)). libRoadRunner 2.11.0 changed
  how the absolute tolerance vector is scaled [R2]: a new engine version can change results, and
  then changes the fingerprint.
- The same request gives the same bytes in two fresh processes of one build on one machine (checked
  by the provider's tests). Across CPUs of one architecture this is not verified
  ([libRoadRunner 2.11.0: checked facts](#libroadrunner-2110-checked-facts)); across platforms,
  results agree within a tolerance only, as for modules.

### Threading

What the provider does is specified in
[simulation-service.md](../specs/simulation-service.md#the-roadrunner-provider) (compiled models
and threads); the upstream facts it rests on are under
[JIT backends and configuration](#jit-backends-and-configuration) and
[Runs, resets and events](#runs-resets-and-events).

### Security

- **Only document text.** libRoadRunner reads a string that its own test does not take for a
  document as a path or a URI ([Model texts](#model-texts)), and links POCO's networking library
  [R6][R7]. The provider passes it only text that passes that test, so libRoadRunner never reads a
  path or a URI on behalf of a model.
- **`ode-uq-spec` rules for SBML.** `ode-uq-spec` 0.0.0 forbids XML document type declarations in
  model sources, because they enable entity-expansion attacks (`conventions.md`, Security), and
  forbids SBML sources that require a Level 3 package, noting that this excludes the `comp` package
  (`specification.md`, Model specification). The provider refuses `<!DOCTYPE` before any XML parser
  sees the text, Level 3 documents that require a package, and every declaration of `comp`
  ([simulation-service.md](../specs/simulation-service.md#the-roadrunner-provider)).
- **Generated code.** The JIT turns model mathematics into native code that runs in the host
  process. The input is SBML math, not program text, but a defect in code generation affects the
  process. Size limits on the model, the request and the result bound the work and the memory.
- **macOS hardened runtime.** Apple's Hardened Runtime disallows memory that is both writable and
  executable unless the app has the `com.apple.security.cs.allow-jit` entitlement and uses
  `MAP_JIT`; without it, JIT-dependent code may crash [R31]. A hardened-runtime host that loads the
  provider needs `com.apple.security.cs.allow-jit` or, if LLVM's MCJit memory manager does not use
  `MAP_JIT` (unverified), `com.apple.security.cs.allow-unsigned-executable-memory`, in addition to
  the library-validation note of
  [ADR-0019](../adr/ADR-0019-platform-loading-rpath-dll-search.md). Neither is verified with such a
  host (OQ-27).

## Level C, compiling through roadrunner (future work)

Nothing in this section is specified or built. It records the design as far as it is known, for
OQ-28.

**Aim.** A module supplies code that is compiled together with the model, so that module code and
model code inline into each other instead of meeting through the kernel's function pointers
(level B).

**Design as far as it is known.**

- The module supplies LLVM bitcode, which the provider links into the model's LLVM module after
  libRoadRunner generates it and before optimization, so the optimizer sees both.
- Module bitcode may call only Praenora-defined, inlinable wrappers around the model: its
  right-hand side, its parameters and its quantities. No other symbol of the engine or the model is
  available to it, so modules do not depend on the internals of the generated code.
- Module bitcode is compiled with `-ffp-contract=off` (no floating-point contraction) and within a
  bitcode version ceiling: the engine's LLVM 13 reader must read it.
- The digest of the module's bitcode enters the run fingerprint, beside the engine identity.
- A provider would offer it as a later minor of `praenora.simulation` or as a separate service;
  which one is decided when the hook below exists.

**Why it is blocked.**

- libRoadRunner 2.11.0 has no hook between generating a model's code and adding it to the JIT
  ([Build and link](#build-and-link)): an upstream change is needed.
- The shared library exports the LLVM inside it ([Build and link](#build-and-link)), so a provider
  that reads module bitcode with the engine's LLVM can use only the static library, behind an export
  list, as the provider `roadrunner` does; a hook has to be reachable there.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| MCJit code may differ between CPUs of one architecture | Equal fingerprints with results that are not byte-identical | Check on a second machine of the same architecture; until then, [reproducibility.md](../specs/reproducibility.md) states it as not verified. |
| Linux and Windows builds are not done | The provider exists on macOS arm64 only | Build and test with the upstream archives or with source builds (OQ-27). |
| Static linking of LGPL-2.1 libSBML, and NLEQ sources without a stated license | Distribution blocked or obligations unmet | License review before the provider is distributed (OQ-1, OQ-13, OQ-27). |
| Hardened-runtime hosts | Code generation fails without a JIT entitlement | Name the entitlement in the host's signing; verify with such a host (OQ-27). |
| Engine warnings are dropped (libRoadRunner's logger at the error level) | Warnings about a model never reach the logs | Revisit the level when a module needs them ([simulation-service.md](../specs/simulation-service.md#the-roadrunner-provider), Logging). |
| Another LLVM in the process (for example libRoadRunner's Python package imported beside the Python bindings) | Clashes between the two copies (unverified) | The provider keeps its LLVM behind its export list; a process that loads both is not tested. |

## Remaining checks

1. **CPU independence.** Run the provider's two-process test on a second machine of the same
   architecture and compare the bytes. *Exit:* identical bytes, or the difference recorded and the
   reproducibility claim narrowed.
2. **Other platforms.** Build the provider on Linux x86-64 and Windows x86-64 and run its tests.
   *Exit:* the same tests pass, and the Windows linker and the Linux export list are confirmed or
   corrected here.
3. **License review.** *Exit:* a written answer on linking libSBML and on the NLEQ terms, recorded
   in an ADR before the provider is distributed.
4. **Hardened runtime.** Load the provider in a host signed with the hardened runtime. *Exit:* the
   entitlement it needs is named in [platforms.md](../platforms.md).
5. **Level C.** When libRoadRunner offers a hook between code generation and the JIT, write the
   specification and an ADR.

## Sources

Pages R1 to R32 were read on 2026-11-11; R33 to R35 were checked on 2026-11-16; R36 was not read
in this build (unverified). The `ode-uq-spec` 0.0.0 rules cited by name above were read in that
specification's own repository (`docs/conventions.md`, `docs/specification.md`,
`schemas/simulation.schema.json`); no public URL is cited for it.

| Id | Source | Supports |
|---|---|---|
| R1 | [sys-bio/roadrunner, repository page](https://github.com/sys-bio/roadrunner) | C/C++ library for SBML simulation; Apache 2.0; LLVM code generation by default; C/C++, Python and Julia bindings; binaries for Windows, macOS and Linux; Linux binaries depend on ncurses; dependencies from `libroadrunner-deps`; a C++17 compiler. |
| R2 | [sys-bio/roadrunner, releases](https://github.com/sys-bio/roadrunner/releases) | 2.11.0 is the latest release; its notes: Python 3.12 to 3.15 bindings, updated SUNDIALS, `init(x)` can override an existing initial assignment, the absolute tolerance vector scaled differently. |
| R3 | [GitHub API: release v2.11.0 of sys-bio/roadrunner](https://api.github.com/repos/sys-bio/roadrunner/releases/tags/v2.11.0) | Publication date 2026-10-21; asset names and sizes per platform, including `windows-msvc18-x86_64` and `macos-14-arm64`. |
| R4 | [libroadrunner on PyPI](https://pypi.org/project/libroadrunner/#files) | 2.11.0 of 2026-10-21; wheels for `win_amd64`, `manylinux_2_28_x86_64`, `macosx_15_0_x86_64`, `macosx_14_0_arm64`; sizes 24 to 51 MB; Apache 2.0. |
| R5 | [Installing RoadRunner](https://libroadrunner.readthedocs.io/en/latest/Installation/installation.html) | Source build with `libroadrunner-deps` and prebuilt LLVM 23.1.4; `LLVM_INSTALL_PREFIX`, `RR_DEPENDENCIES_INSTALL_PREFIX`. |
| R6 | [roadrunner `CMakeLists.txt` (develop)](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/CMakeLists.txt) | CMake 3.16, C++17; options `BUILD_LLVM`, `BUILD_RR_CAPI` (ON), `BUILD_LEGACY_C`; dependency packages and floors (libSBML 5.20.5 static, SUNDIALS 7.8.0, POCO Net); installed helper modules. |
| R7 | [roadrunner `source/CMakeLists.txt` (develop)](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/source/CMakeLists.txt) | `roadrunner` (shared) and `roadrunner-static` targets; exported packages and namespaces; PUBLIC link interface; LLJit and MCJit sources; `RoadRunnerMap` and a thread pool. |
| R8 | [roadrunner `wrappers/C/rrc_api.h`](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/wrappers/C/rrc_api.h) | C API: `RRHandle`, loading with a force-recompile flag, time-course functions, `setValue`, the reset family including `resetToOrigin`, `hasError(void)`, `getLastError(void)`, client-freed results. |
| R9 | [roadrunner `wrappers/C/rrc_utilities.cpp`](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/wrappers/C/rrc_utilities.cpp) | The C API's last error is the global `char* gLastError` in namespace `rrc`, with no mutex in that file. |
| R10 | [`rr::RoadRunner` class reference](https://libroadrunner.readthedocs.io/en/latest/C%2B%2BAPIReference/rrRoadRunner.html) | Constructors (URI, path or contents; copy), `simulate` overloads and borrowed results, `setValue`, `setSelections`, integrators, `saveStateS`/`loadStateS`, ownership of returned pointers, `initLLVM()`. |
| R11 | [roadrunner `source/rrException.h`](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/source/rrException.h) | `rr::Exception` derives from `std::exception`; `CoreException`, `CVODEException` and the other subclasses. |
| R12 | [`RoadRunnerMap` class reference](https://libroadrunner.readthedocs.io/en/latest/C%2B%2BAPIReference/RoadRunnerMap.html) | Parallel building of models with a thread pool; default of one thread. |
| R13 | [Parallel RoadRunnerMap](https://libroadrunner.readthedocs.io/en/latest/parallel/roadrunner_map/roadrunner_map_index.html) | The map is thread-safe and builds models with a thread pool; compilation cost "can be prohibitory" for many models. |
| R14 | [libRoadRunner 2.0: a high performance SBML simulation and analysis library](https://pmc.ncbi.nlm.nih.gov/articles/PMC9825722/) | LLJit on LLVM 13's ORC JIT v2, more than three times faster than MCJit; compile time dominates for many models; in-memory caching of object files; saved states reload faster than recompiling; parallel building in C++; C++, C and Python APIs. |
| R15 | [Jit Engines](https://libroadrunner.readthedocs.io/en/latest/tutorial/jit_engines.html) | MCJit default, LLJit optional, set globally through `Config`; `LLJIT_OPTIMIZATION_LEVEL` default none; `LLJIT_NUM_THREADS` documented as 0, compiling on the calling thread. |
| R17 | [Roadrunner Solvers](https://libroadrunner.readthedocs.io/en/latest/tutorial/solvers.html) | Integrators `cvode`, `gillespie`, `rk4`, `rk45`; CVODE is the default; CVODE setting names. |
| R18 | [Stochastic Simulation](https://sys-bio.github.io/roadrunner/docs-build/stochastic.html) | The Gillespie integrator has a `seed` setting; a given seed regenerates the same random sequence. |
| R19 | [`rr::Logger` class reference](https://libroadrunner.readthedocs.io/en/latest/C%2B%2BAPIReference/rrLogger.html) | Logging is configured through static functions. |
| R20 | [sys-bio/libroadrunner-deps](https://github.com/sys-bio/libroadrunner-deps) | Dependency repository built with CMake; prebuilt binaries must match the compiler. |
| R21 | [libroadrunner-deps `.gitmodules`](https://raw.githubusercontent.com/sys-bio/libroadrunner-deps/develop/.gitmodules) | The dependencies are git submodules (bzip2, zlib, NLEQ1, NLEQ2, POCO, rr-libstruct, SUNDIALS, libSBML, expat and others). |
| R22 | [roadrunner `LICENSE.txt`](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/LICENSE.txt) | Apache License 2.0. |
| R23 | [LLVM Developer Policy](https://llvm.org/docs/DeveloperPolicy.html) | "Apache 2.0 License with LLVM exceptions" and the exception for code embedded by compilation. |
| R24 | [sbmlteam/libsbml](https://github.com/sbmlteam/libsbml) | LGPL 2.1; release 5.21.0; Xerces, Expat or libxml2; Linux, Windows, macOS. |
| R25 | [LLNL/sundials](https://github.com/LLNL/sundials) | BSD 3-clause; version 7.9.0 of September 2026. |
| R26 | [pocoproject/poco](https://github.com/pocoproject/poco) | Boost Software License 1.0. |
| R27 | [sys-bio/nleq2](https://github.com/sys-bio/nleq2) | No license file or license text on the repository page. |
| R28 | [GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) | Section 6: conditions for distributing a work linked with the library, including relinking means or a shared-library mechanism. |
| R29 | [C++ binary compatibility 2015-2026](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017) | Binaries from the v140 to v145 build tools can be combined, but the linker must be at least as new as the newest build tools used for any input. |
| R30 | [libSBML `ExternalModelDefinition` class reference](https://sbml.org/software/libsbml/5.18.0/docs/formatted/cpp-api/class_external_model_definition.html) | The `comp` package's `source` attribute may be a URL, URN or file location of another SBML document. |
| R31 | [Allow execution of JIT-compiled code entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit) (read through the page's documentation data) | The Hardened Runtime disallows writable and executable memory unless the app has the entitlement and uses `MAP_JIT`; JIT-dependent code may otherwise crash. |
| R32 | [WebAssembly: Security](https://webassembly.org/docs/security/) | Compiled code is immutable and not observable at run time; call targets are functions declared at load time. |
| R33 | `roadrunner-2.11.0-macos-14-arm64-Release.zip` of the [release v2.11.0 of sys-bio/roadrunner](https://github.com/sys-bio/roadrunner/releases/tag/v2.11.0), unpacked | Its headers (`include/rr`, `include/rr/llvm`), CMake package files (`lib/cmake`), static library and `libroadrunner.dylib` (`nm`, `otool`, `strings`): LLVM 13.0.1 inside and exported, macOS 14.0, `IntegratorException`, `LLVMModelGenerator`'s entry points, the headers that are not installed. |
| R34 | `llvm-13.x-macos-14-arm64-Release.zip` of the [release v13.1 of sys-bio/llvm-13.x](https://github.com/sys-bio/llvm-13.x/releases/tag/v13.1), unpacked | `llvm-config --version`: 13.0.1. |
| R35 | [libRoadRunner source at the tag v2.11.0](https://github.com/sys-bio/roadrunner/tree/v2.11.0) | `rrRoadRunner.cpp` (constructor, `load`, `reset`, `simulate`, `isCompleteFBC`, copies), `rrSBMLReader.cpp` (`is_sbml`, paths and URIs, `comp` flattening), `rrConfig.cpp` (defaults, configuration files), the MCJit and LLJit sources, `LLVMModelGenerator.cpp`, `LLVMExecutableModel.cpp` (reset, pending events), `Random.cpp`, the CVODE integrator and the integrator listener. |
| R36 | [sys-bio/roadrunner issue 1259](https://github.com/sys-bio/roadrunner/issues/1259) (not read in this build) | Two copies of LLVM in one process, from the LLVM that the shared library exports (unverified). |
