# libRoadRunner simulation service: feasibility design

- **Status:** Feasibility design, informative. Nothing in this document is implemented, and the
  build option `PRAENORA_WITH_ROADRUNNER` does not exist yet
  ([`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake)). The service table below is a
  draft: it is not part of ABI v0, and adopting it is a versioned change with its own ADR
  ([compatibility-policy.md](../specs/compatibility-policy.md)).
- **Decision record:** [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md) (Proposed)
  and the open question OQ-16 in [open-questions.md](../open-questions.md).
- **Sources:** upstream facts were checked on 2026-11-11 against libRoadRunner 2.11.0. They are
  cited as [R1] to [R32] and listed under Sources at the end. A claim that could not be checked is
  marked "(unverified)".

## Question and recommendation

**Question.** Can the runtime offer SBML simulation to modules as the runtime service
`praenora.simulation@0`, backed by libRoadRunner: load a model once and cache it, set parameters and
initial values, simulate a time course with selected outputs and return the results, with
numerical failures reported as results? What does it cost in build complexity, size, threading,
determinism and licensing, and how does it line up with the simulation configuration of
`ode-uq-spec`?

**Recommendation.** It is feasible as an optional part of the runtime:

- Build it only with `PRAENORA_WITH_ROADRUNNER=ON` (default OFF), linking libRoadRunner's static
  library PRIVATE into the runtime, behind the runtime's hidden symbol visibility. Default builds
  and the MVP are unchanged.
- Use libRoadRunner's C++ API inside the runtime and give modules only the C service table below.
  The C API keeps its last error in a process-wide variable, which rules it out for a service that
  several instances and threads may call.
- Give each session its own `rr::RoadRunner` object, keep a process-wide cache of compiled models
  keyed by the SHA-256 of the SBML text, and make every `simulate` call independent of the previous
  one: each starts from the model's original state.
- Record the engine's identity (libRoadRunner, LLVM and SUNDIALS versions, JIT settings) in the
  run's reproducibility section, so that a different engine gives a different fingerprint.
- Leave it out of any WebAssembly build of the runtime: its LLVM JIT has no WebAssembly path
  ([webassembly.md](webassembly.md)).
- Settle three items before implementation: the license review of the dependencies that would be
  linked statically (libSBML is LGPL-2.1, and the NLEQ solver sources show no license in their
  mirror), the macOS arm64 deployment target that the upstream binaries require, and the Windows
  linker version they require.

## Options compared

| Option | Summary | For | Against |
|---|---|---|---|
| S1. Compiled into the runtime (recommended) | `PRAENORA_WITH_ROADRUNNER=ON` links `roadrunner-static` PRIVATE into `praenora_runtime`; the service is registered when a `Runtime` is constructed. | One binary and no new loading mechanism; the third-party symbols stay hidden behind the runtime's export list. | Every runtime built with the option carries LLVM, SUNDIALS and libSBML; the upstream C++ archives alone are 160 to 477 MB (see LLVM and JIT footprint). |
| S2. A separate service library | A shared library holding the service, which the runtime loads when present. | The base runtime and the Python wheel stay small; packaging can leave the service out. | A second loading path (discovery, digests, versioning) to design and test. |
| S3. Each module bundles libRoadRunner | Possible today: nothing stops a module from linking it. | No runtime change. | Every such module carries the whole dependency set; the runtime cannot record which engine ran; ADR-0021 rejects it. |
| S4. Out-of-process simulation | A helper process that owns libRoadRunner. | A crash in the engine cannot end the run's process (OQ-5). | Process management and data transfer; out of scope for the MVP (ADR-0021). |

S1 is the simplest first step. S2 is the follow-up if distribution size becomes a problem (OQ-13).

## Integration design

### Where the service plugs in

- **Registry.** [`services.hpp`](../../src/runtime/instance/services.hpp) maps a service name and a
  major version to a table. With the option ON, the runtime registers one static table,
  `praenora.simulation` at major 0, before any instance uses the registry; lookups are then plain
  reads and thread-safe ([ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md)).
- **Lookup.** A module calls `get_service(context, "praenora.simulation", 19, 0)` through its host
  API, usually in `create` or `initialize`. It gets NULL from a runtime built without the option, and
  then fails with `PRAENORA_STATUS_UNSUPPORTED` and an error document that names the service. Every
  lookup is logged as the event `service.lookup` ([module-abi.md](../specs/module-abi.md#host-api)).
- **Lifetime.** The table, the model cache and LLVM's state live until the process ends. An
  unresponsive instance may still be inside a service call when its run is torn down; the runtime
  withdraws services from it for later lookups
  ([threading-and-ownership.md](../specs/threading-and-ownership.md#unresponsive-instances)) but
  cannot stop a call in progress, so the service must never be torn down under it.
- **Early failure (later).** A manifest field naming the services a module needs would let
  validation fail before any module is loaded. That is a PMM change
  ([manifest.md](../specs/manifest.md)).

### Draft service table (not part of ABI v0)

The table follows the conventions of [`abi.h`](../../include/praenora/sdk/abi.h) and the shape of
the test-only service in [`test_gate.h`](../../tests/support/test_gate.h): the common header, a
context pointer, then functions that take only fixed-width integers, `size_t`, pointers,
length-delimited UTF-8 and sinks.

```c
/* DRAFT: praenora.simulation@0, for discussion only. Not part of ABI v0 and not implemented. */
#include <praenora/sdk/abi.h>

#define PRAENORA_SIMULATION_SERVICE "praenora.simulation"
#define PRAENORA_SIMULATION_SERVICE_MAJOR 0u

/* A simulator bound to one model: opaque, allocated and freed by the service. */
typedef struct praenora_simulation_session praenora_simulation_session;

typedef struct praenora_simulation_v0 {
    praenora_api_header header; /* abi_major: the service major (0); abi_minor; struct_size of
                                   this table as compiled into the runtime; reserved: 0 */
    void* context;              /* service-owned; pass it to describe and open_session */

    /* Writes the service's identity: {"service", "version" {major, minor}, "engine" {name,
     * version, llvm, sundials, jit, jit_optimization}, "methods": [...]}. Thread-safe and free of
     * side effects. */
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);

    /* Opens a session for one SBML model. `sbml` is the document's text (UTF-8, not
     * NUL-terminated); `options_json` is a JSON object of load options ({} in this draft). On
     * success *out_session holds the session, and close_session must be called for it exactly once.
     * `host` is the calling instance's host API: the service logs through it and polls
     * cancel_requested() while it waits for the model cache. */
    praenora_status (PRAENORA_CALL *open_session)(void* context, const praenora_host_api* host,
                                                  praenora_bytes sbml, praenora_bytes options_json,
                                                  praenora_simulation_session** out_session,
                                                  praenora_sink* error_json);

    /* Runs one time course from the model's original state with the values of `request_json`.
     * Writes a result document to result_json: {"ok": true, ...} or, for a numerical failure,
     * {"ok": false, "error": {...}} with status OK. A request that cannot be run returns a status
     * other than OK and writes an error document to error_json. */
    praenora_status (PRAENORA_CALL *simulate)(praenora_simulation_session* session,
                                              const praenora_host_api* host,
                                              praenora_bytes request_json,
                                              praenora_sink* result_json,
                                              praenora_sink* error_json);

    /* Frees the session. Cannot fail. */
    void (PRAENORA_CALL *close_session)(praenora_simulation_session* session);
} praenora_simulation_v0;
```

Rules, each mirroring a rule of the module ABI ([module-abi.md](../specs/module-abi.md),
[threading-and-ownership.md](../specs/threading-and-ownership.md)):

- **Header and evolution.** The header carries the service's own major and minor version. Minor
  versions only append functions, and a module checks `header.struct_size` before it uses a function
  appended after the version it was built against, as it does for the host API. A batch function,
  the open part of OQ-16, would be such an addition.
- **Sinks.** In a service call the module is the caller: it passes sinks it owns, and the service
  calls `write` on them during the call. The sink copies the bytes before `write` returns, exactly as
  the runtime's sinks do for modules ([`output_sink.hpp`](../../src/runtime/loader/output_sink.hpp)),
  and a refused write ends the call with the sink's status.
- **Borrowed inputs.** `sbml`, `options_json` and `request_json` are borrowed for the call. The
  service copies what it keeps: the SBML text, for the cache.
- **Sessions.** `open_session` allocates and `close_session` frees, with the service's allocator.
  A module closes its sessions before its `destroy` returns, as it must stop its own threads.
- **Threading.** `describe` and `open_session` may be called from any thread at any time. A session
  serves one call at a time: a call on a session that is busy returns `PRAENORA_STATUS_INVALID_STATE`.
  Different sessions may be used from different module threads at once (subject to the spike's
  thread check).
- **Host API.** `host` is the calling instance's host API, valid during the call. The service logs
  through it, so its messages carry the step's attributes, polls `cancel_requested()` and returns
  `PRAENORA_STATUS_CANCELLED` when it turns nonzero, and never keeps the pointer after the call.
- **Errors.** Statuses are `PRAENORA_STATUS_OK` and the general codes 1 to 12 of
  [`status.h`](../../include/praenora/sdk/status.h); no new code is needed. Error documents have the
  `pmp-error` shape `{"code", "message", "details"}`, with `details.service` and, when
  libRoadRunner raised the failure, `details.engine_error`.
- **No exceptions.** No C++ exception or `longjmp` leaves a service function: every function
  catches everything, as the SDK's C++ adapter does
  ([`module.hpp`](../../include/praenora/sdk/module.hpp)).

### Request and result documents

A request carries numbers only. Expressions in `ode-uq-spec` documents are strings that must be
parsed as SBML Level 3 infix math and never executed (`ode-uq-spec` 0.0.0, `conventions.md`,
Security). The service evaluates none: a module that receives expression-valued settings resolves
them before it calls the service, or the request is refused with `PRAENORA_STATUS_UNSUPPORTED`.

```json
{
  "time_domain": {"start": 0, "end": 5},
  "initial_conditions": {"central": 100, "peripheral": 0},
  "parameter_values": {"k12": 0.35, "k21": 0.18, "ke": 0.12, "Vc": 4.0},
  "solver": {"method": "cvode", "stiffness": "stiff", "relative_tolerance": 1e-8,
             "absolute_tolerance": 1e-10, "max_steps": 100000},
  "output": {"save_at": "explicit_times", "times": [0, 0.5, 1, 2, 5],
             "quantities": ["central_concentration"], "include_states": true}
}
```

```json
{
  "ok": true,
  "columns": ["time", "central", "peripheral", "central_concentration"],
  "rows": [[0, 100, 0, 25], [0.5, 84.1, 13.9, 21.0]],
  "solver": {"method": "cvode", "stiff": true, "relative_tolerance": 1e-8,
             "absolute_tolerance": 1e-10, "maximum_num_steps": 100000}
}
```

The values above are illustrative, and the result is cut to its first two rows.

- `rows` has one row per output time and one value per column, the layout of libRoadRunner's result
  matrix, whose columns are the current selections [R10]. Numbers are written in the runtime's
  canonical JSON ([reproducibility.md](../specs/reproducibility.md#canonical-json)).
- JSON has no NaN or infinity, so a non-finite value makes the result
  `{"ok": false, "error": {"code": 11, ...}}` with `details.reason` `"nonfinite"`.
- `solver` echoes the settings actually used, after defaults, so a module can record them.
- A numerical failure (for example a CVODE error or too many steps) is a result with `"ok": false`
  and status OK, so that a module fitting many parameter sets can treat it as data rather than as a
  failure of its own call.

### Alignment with the ode-uq-spec simulation configuration

The request uses the field names of an `ode-uq-spec` 0.0.0 simulation configuration
(`docs/specification.md`, Simulation configuration, and `schemas/simulation.schema.json` in that
specification), so a module can pass the numeric parts of one through.

| `ode-uq-spec` field | In the draft request | libRoadRunner |
|---|---|---|
| `time_domain.start`, `end` | Same. | `simulate(start, stop, points)` or `simulate(times)` [R10]. `ode-uq-spec` lets initial values take effect at `start`; whether libRoadRunner applies initial assignments at a nonzero start time is unverified. |
| `initial_conditions` | Numbers only. | `init(x)` selections; the 2.11.0 release notes say `init(x)` can now override an existing initial assignment [R2], which the `ode-uq-spec` semantics require. |
| `parameter_values` | Numbers only. | `setValue(id, value)` [R10], or `init(x)` for symbols with initial assignments (spike item). |
| `inputs` | `constant` only, treated as a parameter value. | As above. `table` and `expression` inputs are `PRAENORA_STATUS_UNSUPPORTED` in this draft. |
| `solver.method` | libRoadRunner integrator names: `cvode`, `rk4`, `rk45`, `gillespie`. | `setIntegrator(name)` [R10]; the names are those of the solvers page [R17]. A module maps other method names, such as the `adaptive-rk` of the `ode-uq-spec` example. |
| `solver.stiffness` | `stiff`, `nonstiff`, `auto`. | CVODE's `stiff` setting [R17]. No automatic switching is documented, so `auto` provisionally selects `stiff` and the result's `solver` echo says so. |
| `solver.relative_tolerance`, `absolute_tolerance` | Same. | CVODE settings of the same names [R17]. |
| `solver.max_steps`, `max_step`, `min_step` | Same. | `maximum_num_steps`, `maximum_time_step`, `minimum_time_step` [R17]. |
| `solver.event_time_tolerance` | Refused (`PRAENORA_STATUS_UNSUPPORTED`). | No counterpart found among the CVODE settings [R17]. |
| `output.save_at` | `explicit_times` (with `times`), `regular_grid` (with `step`), `solver_steps`. `target_times` must be turned into `times` by the module, which has the target data. | `simulate(times)`, `simulate(start, stop, points)`, and the `variable_step_size` setting [R17] (its exact semantics are unverified). |
| `output.quantities`, `include_states` | Same. | `setSelections` [R10]: `time`, then the states when `include_states` is true, then the quantities. |
| `output.dense_output` | Refused when true. | No counterpart. |
| `events_enabled` | Refused when present. | Not mapped in this draft. |
| `checks.reject_nonfinite` | Always on (JSON cannot carry non-finite numbers). | Checked on the result matrix. |
| `checks.enforce_time_domain` | Always on: output times outside `[start, end]` are `PRAENORA_STATUS_INVALID_ARGUMENT`. | Checked before the run. |
| Stochastic runs | `gillespie` requires a `seed` in `solver`, an integer from 0 to 2^53 − 1 that the module derives from its step seed. | The Gillespie integrator has a `seed` setting, and a given seed regenerates the same random sequence [R18]. The default seed is unverified; the service never uses it. |

### Load-once caching

Compiling a model is the expensive part. The libRoadRunner 2.0 paper reports that with many models
"the run time is dominated by the compile time", that its LLJit engine is more than three times
faster than the older MCJit when loading and simulating over 1,100 BioModels models, and that LLJit
caches model object files in memory for fast reloading [R14]. The LoadSBMLOptions reference says the
LLVM model generator "maintains a hash table of currently running models" and can reuse the code
generated for a running instance of the same model [R16]; that page's descriptions of the
`recompile` and `readOnly` options appear transposed, so which flag controls the reuse is unverified.

The service adds its own cache on top, so that its behavior does not depend on that detail:

- **Key:** SHA-256 of the SBML bytes (the runtime already has an implementation,
  [`sha256.hpp`](../../src/runtime/core/sha256.hpp)), the canonical JSON of the load options and the
  engine identity.
- **Value:** a compiled template: a `rr::RoadRunner` loaded from the text, or its saved state.
  `saveStateS()` returns a state that `loadStateS()` restores [R10], and the paper reports that
  reloading a saved state is much faster than recompiling [R14]. A session is created from the
  template by copy construction (`RoadRunner(const RoadRunner&)` exists [R10]) or by `loadStateS()`;
  the spike decides which is faster and gives identical results.
- **Bounds:** a maximum number of entries and of bytes, least recently used first out; an entry is
  kept while a session uses it. A cache hit and a miss are logged at debug level.
- **Concurrency:** one mutex guards the map. A miss compiles outside the lock, with a per-key "in
  progress" marker so that two sessions of one model compile it once; a caller waiting for the
  marker polls `cancel_requested()`.

### Threading and instance pooling

- **One object per session.** A session owns one `rr::RoadRunner`, and the service serializes calls
  on it. This matches the runtime's model: a module instance's calls all run on its worker thread
  ([ADR-0006](../adr/ADR-0006-threading-model.md)). The class reference gives no general
  thread-safety guarantee for instances [R10].
- **Parallel sessions.** A module may run sessions on threads of its own (it must join them before
  `destroy` returns). libRoadRunner builds models in parallel itself: `RoadRunnerMap` loads models
  with a thread pool into a thread-safe map, entirely in C++ [R12][R13][R14]. That supports
  constructing separate instances concurrently; simulating separate instances concurrently is
  unverified and is a spike item under ThreadSanitizer.
- **Pool.** The cache's templates are the pool: sessions are cheap copies of a compiled model. A
  pool of ready instances per thread adds nothing while the runtime makes one module call at a time
  ([ADR-0011](../adr/ADR-0011-tick-based-synchronous-execution.md)); a future batch function could
  run its items on an internal pool, but would have to return them in request order.
- **JIT threads.** `LLJIT_NUM_THREADS` defaults to 0, meaning compilation runs on the calling thread
  [R15], so a model's compile time counts against the calling instance's deadline. Modules should
  open sessions in `create` or `initialize` (lifecycle timeout, 60 s by default), not in `invoke`.
- **Process-wide settings.** The JIT backend is chosen through `Config`, which applies globally
  [R15], and `rr::Logger` is configured through static functions [R19]. The service sets both once,
  before the first session, and never changes them afterwards; the C++ class reference describes
  LLVM's initialization as done "in a thread safe way" by `initLLVM()` [R10].

### Error mapping

| Condition | Outcome |
|---|---|
| `options_json` or `request_json` is not a JSON object, or has an unknown field or a wrong type | `PRAENORA_STATUS_INVALID_ARGUMENT`, with the JSON pointer in `details` |
| The SBML text is refused by the security checks (see Security) | `PRAENORA_STATUS_DATA_INVALID` |
| libRoadRunner cannot load or compile the model (an exception derived from `rr::Exception`, which derives from `std::exception` [R11]) | `open_session` returns `PRAENORA_STATUS_DATA_INVALID`; `details.engine_error` holds the exception's class and `what()` |
| An unknown symbol in `initial_conditions`, `parameter_values` or `quantities` (`setValue` raises for an invalid selection [R10]) | `PRAENORA_STATUS_INVALID_ARGUMENT`, naming the symbol |
| A method, setting or option this draft does not map | `PRAENORA_STATUS_UNSUPPORTED`, naming the field |
| Integration fails (for example `CVODEException` [R11]), the step limit is reached, or a value is not finite | Status OK; result `{"ok": false, "error": {"code": 11, "message", "details"}}` |
| `std::bad_alloc`, or the model, request or result exceeds the service's size limits | `PRAENORA_STATUS_RESOURCE_EXHAUSTED` |
| A sink refuses a write | The status `write` returned |
| `cancel_requested()` turned nonzero | `PRAENORA_STATUS_CANCELLED` |
| A call on a busy session | `PRAENORA_STATUS_INVALID_STATE` |
| Any other exception | `PRAENORA_STATUS_INTERNAL` |

Cancellation inside a run: `simulate` returns only when the time course is done, so the service
splits a long time course into segments and polls between them. Whether libRoadRunner continues
an integration across segments without reinitializing the integrator, with results identical to an
unsplit run, is unverified; until the spike shows it, the service polls only before and after the
run, and a long run without polls ends as `STEP_TIMEOUT` or `MODULE_UNRESPONSIVE`
([time-cancellation-timeouts.md](../specs/time-cancellation-timeouts.md)).

### C API or C++ API

| Aspect | C API (`rrc_api.h`) | C++ API (`rr::RoadRunner`) |
|---|---|---|
| Error reporting | Functions return `bool` or NULL; `hasError(void)` and `getLastError(void)` take no handle [R8], and the last error is the global `rrc::gLastError` [R9]. | Exceptions derived from `rr::Exception` [R11]. |
| Thread safety | The global last error races when two threads fail at once. | Per-object state, plus the process-wide `Config` and `Logger` [R15][R19]. |
| Memory | The client frees results (`freeRRCData`, `freeText`) [R8]. | `simulate()` returns a matrix owned by the object; pointers from `get…` functions must not be deleted [R10]. |
| Coverage | Instances, loading (with a force-recompile flag), time courses, selections, `setValue`, resets, integrator settings [R8]. | All of that, plus saved states (`saveStateS`, `loadStateS`), copies, `getCurrentSBML` and integrator objects [R10]. |
| Binary coupling | C linkage; generated by default (`BUILD_RR_CAPI` ON) [R6]. | Needs the compiler and standard library libRoadRunner was built with; the dependency repository asks that prebuilt binaries match the compiler in use [R20]. |

Recommendation: the C++ API, used only inside the runtime, which is compiled with the same
toolchain as libRoadRunner. Modules never see a libRoadRunner type: the service table is the only
interface, which keeps the module ABI free of C++
([ADR-0005](../adr/ADR-0005-ownership-and-allocation-across-the-abi.md)).

### Build integration: CMake and versions

- **Upstream build.** libRoadRunner 2.11 requires CMake 3.16 and C++17 and builds a shared
  `roadrunner` and a static `roadrunner-static` library [R6][R7]. It installs the CMake packages
  `roadrunner-config.cmake` (namespace `roadrunner::`) and `roadrunner-static-config.cmake`
  (namespace `roadrunner-static::`) under the library directory's `cmake` folder, a `roadrunner.pc`,
  and the helpers `ImportRoadrunnerAndDependencies.cmake` and `FindLLVM.cmake` [R6][R7].
- **Dependencies.** It finds zlib, bzip2, expat, libSBML (static, at least 5.20.5), rr-libstruct,
  CLAPACK, NLEQ1, NLEQ2, POCO (Foundation, Net and XML), SUNDIALS (at least 7.8.0) and LLVM [R6].
  The static target links all of them PUBLIC, and the shared target inherits that list [R7], so a
  consumer must be able to find every dependency package as well. They come from the
  `libroadrunner-deps` repository, which builds most of them with CMake from git submodules
  [R20][R21], and from a prebuilt LLVM; the installation guide's commands use LLVM 23.1.4 [R5].
- **In this project.** `PRAENORA_WITH_ROADRUNNER` (OFF) in
  [`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake); when ON,
  `find_package(roadrunner-static CONFIG REQUIRED)` from `CMAKE_PREFIX_PATH`, with the dependency
  packages found through the upstream helper. The service is an OBJECT library, with libRoadRunner
  linked PRIVATE, compiled into `praenora_runtime`, so third-party types never reach a public header
  ([ADR-0017](../adr/ADR-0017-dependency-management.md)). There is no FetchContent pin: building LLVM
  and the dependency set from source is far beyond the opt-in fetch mode, and a default build never
  uses the network.
- **Version floor.** libRoadRunner 2.11.0, the current release (2026-10-21) [R3][R4]. The service
  reports the version it was built with in `describe` and the run record.

### LLVM and JIT footprint

- libRoadRunner uses LLVM to generate code by default [R1]. It offers two JIT backends: MCJit, the
  default, and LLJit, which compiles faster and offers optimization levels [R15].
  `LLJIT_OPTIMIZATION_LEVEL` defaults to none, which gave two times faster model loads; aggressive
  optimization makes faster code but compiles longer [R15].
- Size: the 2.11.0 release archives of the C++ library are about 160 MB (macOS arm64, Release),
  167 MB (macOS x86-64), 238 MB (manylinux 2.28 x86-64) and 477 MB (Windows x86-64, Release), and the
  Windows Debug archive is about 1.3 GB [R3]; the Python wheels are 24 to 51 MB [R4]. How much a
  static link adds to `praenora_runtime` after dead code elimination and stripping is unverified and
  is measured by the spike.
- Memory per compiled model and compile time per model size are unverified; the jit-engines page
  relates compile time to the optimization level only [R15].

### Platform matrix

| Platform | Project presets | libRoadRunner 2.11.0 binaries [R3][R4] | Notes |
|---|---|---|---|
| Linux x86-64 | `linux-x64-*` | `manylinux_2_28-x86_64` and `ubuntu-24-x86_64` archives; `manylinux_2_28` wheels | The manylinux 2.28 tag implies glibc 2.28 or newer (unverified). The README notes that the Linux binaries depend on ncurses [R1]. |
| macOS arm64 | `macos-arm64-*` | `macos-14-arm64` archives; `macosx_14_0_arm64` wheels | Built for macOS 14: a runtime that links them needs a deployment target of at least 14.0 instead of the project default 11.0, or the linker warns about every such library, as [AGENTS.md](../../AGENTS.md) describes for newer Homebrew libraries. Building libRoadRunner from source for 11.0 is unverified. Hardened-runtime hosts need the JIT entitlement (see Security). |
| Windows x86-64 | `windows-x64-*` | `windows-msvc18-x86_64` archives; `win_amd64` wheels | Microsoft's rule: the final link must use build tools at least as new as the newest that built any input [R29]. If `msvc18` denotes Visual Studio 2026 (unverified), linking the static archives needs its build tools, not the Visual Studio 2022 of the project presets; otherwise build libRoadRunner from source with Visual Studio 2022. |
| macOS x86-64 | none | `macos-15-x86_64` archives and wheels | Not a project platform. |
| Linux arm64, Windows arm64 | none | none listed | Building from source should be possible on platforms LLVM supports (unverified). |

### WebAssembly outlook

- **Runtime compiled to WebAssembly** ([webassembly.md](webassembly.md), Model A). libRoadRunner
  generates native machine code at run time. WebAssembly code is immutable and not observable at
  run time, and call targets are fixed when a module is loaded [R32], so a JIT inside a WebAssembly
  module cannot run what it generates. No upstream WebAssembly build of libRoadRunner was found, and
  whether it has a backend that needs no code generation (the build has a legacy C option,
  `BUILD_LEGACY_C` [R6]) is unverified. In Model A the service is absent: `get_service` returns NULL.
- **WebAssembly modules in a native runtime** (Model B). The service stays native in the host and
  could be offered to WebAssembly modules as host imports that mirror the table above.

### Licensing facts

These are facts about upstream projects, gathered for the license decision (OQ-1) and for
distribution (OQ-13). This project states no license of its own.

| Component | License as stated upstream |
|---|---|
| libRoadRunner | Apache License 2.0 [R1][R22][R4] |
| LLVM | Apache 2.0 License with LLVM exceptions; portions embedded in object code by compilation may be redistributed without complying with sections 4(a), 4(b) and 4(d) [R23] |
| libSBML | LGPL 2.1 (the repository page does not say "or later") [R24] |
| SUNDIALS | BSD 3-clause [R25] |
| POCO | Boost Software License 1.0 [R26] |
| NLEQ1, NLEQ2 | No license shown in the mirror repository [R27]; the terms must be established before distribution |
| zlib, bzip2, expat, CLAPACK, rr-libstruct | Not checked (unverified) |

libRoadRunner links libSBML statically [R6]. A distributed binary that contains a statically linked
LGPL-2.1 library must, among other conditions, give users the means to relink it with a modified
library, or use a shared-library mechanism instead (LGPL-2.1, section 6) [R28]. Option S1 therefore
needs either a dynamically linked libSBML or relinking materials, and a legal review either way.

## Determinism, threading and security

### Determinism

- **What must stay fixed.** Results depend on the libRoadRunner, LLVM and SUNDIALS builds, on the
  JIT backend and optimization level, and on the integrator settings. The service fixes the
  process-wide settings once, echoes the effective solver settings in each result, and reports the
  engine identity from `describe`. The reproducibility section gains an entry for each registered
  production service, so that the fingerprint changes with the engine; that is a change to the run
  record format, with its own ADR ([reproducibility.md](../specs/reproducibility.md#run-fingerprint)).
- **Independent calls.** Each `simulate` starts from the model's original state and applies only
  what its request sets, so its result does not depend on earlier calls on the session. The C API
  documents `resetToOrigin`, which "discards any initial values set" [R8]; which C++ call restores
  exactly the original state, parameters included, is a spike item.
- **Randomness.** Only the `gillespie` method is stochastic, and it requires an explicit seed
  derived from the step seed ([reproducibility.md](../specs/reproducibility.md#seeds)); the default
  seed is never used.
- **Guarantee.** The repository's guarantee, byte-identical outputs for the same fingerprint on the
  same platform with the same runtime build, extends to the service when its build is part of that
  runtime build. Across platforms, results agree within a tolerance only, as for modules. The spike
  checks byte identity across processes on one platform.

### Threading

See Threading and instance pooling above. In short: sessions are single-threaded, the cache is
mutex-guarded, process-wide engine settings are written once before any session exists, and
concurrent simulation of distinct sessions waits for the spike's ThreadSanitizer run (the project's
`*-tsan` presets exist for this).

### Security

- **Only document text.** The `RoadRunner` constructor and `load()` accept "a URI, local path, or
  document contents" [R10], and libRoadRunner links POCO's networking library [R6][R7]. The service
  passes libRoadRunner only text that starts, after an optional byte order mark and white space,
  with `<`, so the text is never taken as a path or a URI. Whether a URI would be fetched over the
  network is unverified; the runtime performs no network access, and this rule keeps it that way.
- **`ode-uq-spec` rules for SBML.** `ode-uq-spec` 0.0.0 forbids XML document type declarations in
  model sources, because they enable entity-expansion attacks (`conventions.md`, Security), and
  forbids SBML sources that require a Level 3 package, noting that this excludes the `comp` package
  (`specification.md`, Model specification). The service refuses SBML text containing `<!DOCTYPE`
  before any XML parser sees it, and refuses documents that declare a required package. The `comp`
  package lets a document reference other documents by URL, URN or file location [R30]; whether
  libRoadRunner resolves such references is unverified, and the rule above refuses them in any case.
- **Generated code.** The JIT turns model mathematics into native code that runs in the host
  process. The input is SBML math, not program text, but a defect in code generation affects the
  process. Size limits on the model, the request and the result bound the work and the memory.
- **macOS hardened runtime.** Apple's Hardened Runtime disallows memory that is both writable and
  executable unless the app has the `com.apple.security.cs.allow-jit` entitlement and uses `MAP_JIT`;
  without it, JIT-dependent code may crash [R31]. A hardened-runtime host that links the service
  needs that entitlement, in addition to the library-validation note of
  [ADR-0019](../adr/ADR-0019-platform-loading-rpath-dll-search.md). Whether LLVM's JIT in
  libRoadRunner uses `MAP_JIT` is unverified.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Static linking of LGPL-2.1 libSBML, and NLEQ sources without a stated license | Distribution blocked or obligations unmet | License review before implementation; dynamic libSBML or relinking materials (OQ-1, OQ-13). |
| Upstream binaries pin macOS 14 and a newer Windows linker | Builds with the option cannot use the project's macOS 11.0 default or Visual Studio 2022 | A separate preset for builds with the service, or source builds of libRoadRunner with the project's toolchains. |
| Size of LLVM and the dependency set | Larger runtime binary and Python wheel | Option OFF by default; option S2 if size matters. |
| Two LLVM copies in one process (for example the service and libRoadRunner's own Python package imported together) | Symbol clashes or crashes (unverified) | Hidden visibility and the runtime's export list; a test that imports both. |
| Thread safety of concurrent simulations on distinct objects is undocumented | Data races in modules that simulate on several threads | Spike under ThreadSanitizer; until then, the service serializes all simulations behind one lock. |
| Compile time of large models | Deadlines exceeded in `invoke`, unresponsive instances | Open sessions in `create` or `initialize`; cache; LLJit without optimization. |
| `ode-uq-spec` semantics not matched exactly (start time, initial assignments, `auto` stiffness) | Results differ from other consumers of the same configuration | Spike checks against analytic solutions; unmapped fields are refused, never ignored. |
| Upstream releases change numerical results (2.11.0 changed how the absolute tolerance vector is scaled [R2]) | Equal fingerprints for runs whose engines differ, unless the engine is recorded | Engine identity in the reproducibility section. |

## Spike plan

Each step has an exit criterion; the spike stops at the first that fails and records why.

1. **Link.** Configure a scratch build with `PRAENORA_WITH_ROADRUNNER=ON` against the 2.11.0 macOS
   arm64 Release archive and link `roadrunner-static` into a scratch OBJECT library.
   *Exit:* the tree builds warning-free, `ctest --preset dev` passes unchanged, the runtime exports
   exactly the symbols it exports today, and the size increase is recorded.
2. **Skeleton.** Implement the draft table with a small self-contained SBML model of exponential
   decay. *Exit:* a fixture module gets the service, opens a session and simulates; results match the
   analytic solution within a relative tolerance of 1e-9.
3. **Cache.** Open 50 sessions of one model, then of 10 models. *Exit:* first and cached open times
   are recorded, a cached open is at least ten times faster than a compile (a target to confirm or
   revise), and copy construction and `loadStateS` are compared for speed and identical results.
4. **Threads.** Eight module threads, each with its own session, simulate concurrently under a
   ThreadSanitizer preset (`macos-arm64-tsan`, `linux-x64-tsan`). *Exit:* no ThreadSanitizer
   report, and results identical to a sequential run.
5. **Determinism.** Run the same request in two fresh processes, and on one session after other
   requests. *Exit:* byte-identical result documents; the reset call that restores the original state
   is identified.
6. **Cancellation.** A long time course with a short deadline. *Exit:* the call returns
   `PRAENORA_STATUS_CANCELLED` within the cancellation grace, and segmented runs equal unsegmented
   ones, or the segmentation is dropped and the limitation documented.
7. **`ode-uq-spec` alignment.** Translate the numeric parts of the `ode-uq-spec` example simulation
   configuration into a request. *Exit:* every field maps or is refused with a JSON pointer, and a
   nonzero `time_domain.start` gives the semantics `ode-uq-spec` requires.
8. **Other platforms.** Repeat steps 1, 2 and 5 on Linux x86-64 and Windows x86-64. *Exit:* the same
   tests pass, and the Windows linker and macOS deployment-target requirements are confirmed or
   corrected in this document.
9. **License review.** *Exit:* a written answer on libSBML linking and on the NLEQ terms, recorded in
   an ADR before any implementation is merged.

## Sources

Every page was read on 2026-11-11. The `ode-uq-spec` 0.0.0 rules cited by name above were read in
that specification's own repository (`docs/conventions.md`, `docs/specification.md`,
`schemas/simulation.schema.json`); no public URL is cited for it.

| Id | Source | Supports |
|---|---|---|
| R1 | [sys-bio/roadrunner, repository page](https://github.com/sys-bio/roadrunner) | C/C++ library for SBML simulation; Apache 2.0; LLVM code generation by default; C/C++, Python and Julia bindings; binaries for Windows, macOS and Linux; Linux binaries depend on ncurses; dependencies from `libroadrunner-deps`; a C++17 compiler. |
| R2 | [sys-bio/roadrunner, releases](https://github.com/sys-bio/roadrunner/releases) | 2.11.0 is the latest release; its notes: Python 3.12 to 3.15 bindings, updated SUNDIALS, `init(x)` can override an existing initial assignment. |
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
| R15 | [Jit Engines](https://libroadrunner.readthedocs.io/en/latest/tutorial/jit_engines.html) | MCJit default, LLJit optional, set globally through `Config`; `LLJIT_OPTIMIZATION_LEVEL` default none; `LLJIT_NUM_THREADS` 0 compiles on the calling thread. |
| R16 | [SBML Compile Options (LoadSBMLOptions)](https://libroadrunner.readthedocs.io/en/latest/PythonAPIReference/cls_LoadSBMLOptions.html) | The LLVM model generator keeps a table of running models and can reuse their generated code. |
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
