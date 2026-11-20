# Simulation service `praenora.simulation@0`

- **Version:** service `praenora.simulation` 0.0 (`PRAENORA_SIMULATION_SERVICE_MAJOR` 0,
  `PRAENORA_SIMULATION_SERVICE_MINOR` 0), declared in
  [`simulation.h`](../../include/praenora/sdk/simulation.h) of the module SDK 0.2.0
- **Status:** Normative. Proposed ([ADR-0027](../adr/ADR-0027-simulation-service-v0.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The simulation service lets a module simulate a model with an engine that the runtime owns. It is a
runtime service offered by service providers ([runtime-services.md](runtime-services.md)): a module
declares that it requires it, the runtime finds and loads a provider that offers it, and the module
calls the provider's engine through the C table this document specifies. The module links no engine
and may be built with any compiler.

The service gives two levels of access to one compiled model:

- **Level A, simulate.** `simulate` runs one time course described by a JSON request and writes a
  JSON result. For many runs that differ only in their numbers, `prepare` fixes everything else in a
  **plan**, and `run` and `run_batch` take the numbers and fill the module's `double` buffers.
- **Level B, the compiled model.** `open_kernel` hands out a **kernel**: C functions for the model's
  right-hand side, initial state, parameters and quantities, for modules that integrate the model
  with integrators of their own.

Compiling module code into the engine's model (a level C) is not part of this version
([ADR-0027](../adr/ADR-0027-simulation-service-v0.md)).

In this repository the fixture provider `test-simulation` offers the service for a toy model format
([The test provider](#the-test-provider-test-simulation)), and the provider `roadrunner`, built with
the option `PRAENORA_WITH_ROADRUNNER`, offers it for SBML models with libRoadRunner as its engine
([The roadrunner provider](#the-roadrunner-provider)). Every provider of the service follows this
document, and the service checks ([Service checks](#service-checks)) test that it does.

## Getting the service

A module declares the service in its manifest
([module-developer-guide.md](../module-developer-guide.md#requiring-services)):

```cmake
praenora_add_module(simulating_module NAME simulating-module VERSION 0.1.0
    CAPABILITIES example.estimate@0.1.0 SOURCES src/module.cpp
    REQUIRES_SERVICES praenora.simulation@0.0)
```

and gets the table from its host API, usually once, in `create`:

```c
const praenora_simulation_v0* simulation = (const praenora_simulation_v0*)host->get_service(
    host->context, PRAENORA_SIMULATION_SERVICE, 19, PRAENORA_SIMULATION_SERVICE_MAJOR);
```

A module MUST use the table only when it is not NULL, `header.abi_major` is 0, `header.reserved` is
0 and `header.struct_size` is at least `PRAENORA_SIMULATION_V0_BASE_SIZE`. `header.abi_minor` is the
minor the provider implements: a later minor only appends functions to the table (and fields to the
documents, see [Evolution](#evolution)), and a module uses an appended function only when
`struct_size` covers it. The table lives until the process ends.

## The table

The header is authoritative; this is its declaration, without comments:

```c
#define PRAENORA_SIMULATION_SERVICE "praenora.simulation"
#define PRAENORA_SIMULATION_SERVICE_MAJOR 0u
#define PRAENORA_SIMULATION_SERVICE_MINOR 0u
#define PRAENORA_SIMULATION_RUN_OK 0
#define PRAENORA_SIMULATION_RUN_FAILED 1

typedef struct praenora_simulation_session praenora_simulation_session;
typedef struct praenora_simulation_plan praenora_simulation_plan;

typedef struct praenora_simulation_kernel {
    uint32_t struct_size;
    uint32_t reserved;
    void* model;
    uint64_t state_count;
    uint64_t parameter_count;
    uint64_t quantity_count;
    praenora_status (PRAENORA_CALL *rhs)(void* model, double t, const double* y, double* dydt);
    praenora_status (PRAENORA_CALL *set_parameters)(void* model, const double* values);
    praenora_status (PRAENORA_CALL *initial_state)(void* model, double* y);
    praenora_status (PRAENORA_CALL *quantities)(void* model, double t, const double* y,
                                                double* out);
} praenora_simulation_kernel;

typedef struct praenora_simulation_v0 {
    praenora_api_header header;
    void* context;
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);
    praenora_status (PRAENORA_CALL *open_session)(void* context, const praenora_host_api* host,
        praenora_bytes model, praenora_bytes options_json,
        praenora_simulation_session** out_session, praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *describe_model)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_sink* out_json, praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *simulate)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_bytes request_json, praenora_sink* result_json,
        praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *prepare)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_bytes plan_json,
        praenora_simulation_plan** out_plan, praenora_sink* info_json, praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *run)(praenora_simulation_plan* plan,
        const praenora_host_api* host, const double* values, uint64_t value_count, double* out,
        uint64_t out_count, int32_t* outcome, praenora_sink* failure_json,
        praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *run_batch)(praenora_simulation_plan* plan,
        const praenora_host_api* host, const double* values, uint64_t value_count,
        uint64_t set_count, double* out, uint64_t out_count, int32_t* outcomes,
        praenora_sink* error_json);
    praenora_status (PRAENORA_CALL *open_kernel)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_bytes options_json,
        praenora_simulation_kernel* out_kernel, praenora_sink* error_json);
    void (PRAENORA_CALL *close_kernel)(praenora_simulation_kernel* kernel);
    void (PRAENORA_CALL *close_plan)(praenora_simulation_plan* plan);
    void (PRAENORA_CALL *close_session)(praenora_simulation_session* session);
} praenora_simulation_v0;
```

The header also defines `PRAENORA_SIMULATION_V0_BASE_SIZE` and `PRAENORA_SIMULATION_KERNEL_V0_SIZE`
(the sizes of the v0.0 table and kernel, frozen at that layout) and the size limits of
[Size limits](#size-limits). It is C99, depends only on `abi.h`, `status.h`, `<stddef.h>` and
`<stdint.h>`, and is installed with the SDK. Every function of the table is required.

## Conventions

The conventions of the module ABI apply ([module-abi.md](module-abi.md#conventions)): only
fixed-width integers, `size_t`, pointers, length-delimited UTF-8 and sinks cross the table, and no
exception or `longjmp` leaves a function of the provider.

- **Borrowed inputs.** `praenora_bytes` and `double` arrays are borrowed for the call; the provider
  copies what it keeps. Text is UTF-8 and not NUL-terminated. A pointer MAY be NULL only when the
  number of elements it points to is 0; otherwise the call returns `INVALID_ARGUMENT`.
- **Sinks** belong to the module. The provider writes to them only while the call runs. Every sink
  argument is required: a NULL sink, or one whose `write` is NULL, makes the call return
  `INVALID_ARGUMENT` (and when that sink is `error_json`, nothing is written). Only `failure_json`
  of `run` MAY be NULL. When a sink refuses a write, the call stops and returns the status `write`
  returned, after writing an error document to `error_json` when the refusing sink is another one;
  a session or plan the call would have handed out is not created.
- **Documents** the provider writes are canonical JSON: compact, object keys sorted
  ([reproducibility.md](reproducibility.md#canonical-json)). Documents it reads are strict JSON
  objects of numbers, strings, booleans, arrays and objects; every value the service uses is a JSON
  number, never an expression to evaluate.
- **Statuses** are `PRAENORA_STATUS_OK` and the general codes 1 to 12 of
  [`status.h`](../../include/praenora/sdk/status.h), except that a call ended by a refusing sink
  returns the status that sink's `write` returned, whatever its value.
- **Error documents.** A call that returns a status other than OK writes one document of the
  `pmp-error` shape to `error_json`: `{"code": <the status>, "message": <text>, "details": {...}}`.
  `details` holds `"service": "praenora.simulation"` and, where they apply:
  - `pointer`: the JSON pointer of the problem in the document the call read (`""` for the whole
    document);
  - `id`: an id that the model does not have, or that is listed twice;
  - `reason`: `"model"` for a model text the provider refuses;
  - `engine_error`: `{"type", "message"}` of a failure the engine raised (for example an exception's
    class and text);
  - the counts and limits a refused buffer or size broke.
- **Numerical failures are data.** A run that fails numerically is not an error of the call: the
  call returns OK and reports the run as failed ([Results](#results), [Plans](#level-a-plans)).

## Objects and lifetime

- `open_session` hands out a **session**, `prepare` a **plan** and `open_kernel` a **kernel** (the
  kernel struct is the caller's; its `model`, and what that points to, is the provider's). The
  provider allocates and frees them; the module closes each exactly once with `close_session`,
  `close_plan` or `close_kernel`. `close_*(NULL)` does nothing, and so does `close_kernel` of a
  kernel whose `model` is NULL; `close_kernel` sets `model` to NULL.
- A call that fails hands out nothing: `open_session` sets `*out_session` and `prepare` sets
  `*out_plan` to NULL, and `open_kernel` sets `out_kernel->model` to NULL when `out_kernel` is large
  enough to hold it.
- Plans and kernels keep the compiled model alive on their own. Closing a session does not
  invalidate its plans or kernels: each stays usable until it is closed.
- An object MUST NOT be closed while a call on it runs, and MUST NOT be used after it is closed.
- A module closes every session, plan and kernel before its `destroy` returns, as it stops its own
  threads ([threading-and-ownership.md](threading-and-ownership.md#what-a-module-must-do)). The
  objects of an instance that the runtime abandons as unresponsive are never freed
  ([threading-and-ownership.md](threading-and-ownership.md#unresponsive-instances)).

## Threading

- `describe` and `open_session` MAY be called from any thread at any time, concurrently.
- A session serves one call at a time: `describe_model`, `simulate`, `prepare` or `open_kernel` on a
  session that another call is using returns `INVALID_STATE` at once, without waiting. A plan serves
  one call at a time in the same way (`run`, `run_batch`).
- A call holds its session or plan from after its argument checks until its work on the object is
  done, including while it logs or polls `cancel_requested()` (it may release the object before it
  writes its error document). A call on a plan does not hold the plan's session, and a kernel's
  functions hold nothing.
- Different sessions, plans and kernels MAY be used at the same time from different threads, also
  when they come from one session; a provider MUST be free of data races under these rules.
- A kernel's functions MUST be called by one thread at a time; this is not detected. Different
  kernels MAY be used concurrently.

## Host, logging and cancellation

- `host` is the calling instance's host API ([module-abi.md](module-abi.md#host-api)), valid for the
  call; it MAY be NULL, which means no logging and no cancellation. The provider checks that
  `host->struct_size` covers a callback before it uses it, and never keeps `host` after the call.
- Records that a call can attribute to itself go through `host->log`, so they carry the step's
  attributes. What the engine emits through its own process-wide logging, which cannot tell the
  calling instance apart, goes to the provider's own logger
  ([runtime-services.md](runtime-services.md#loading)), or is silenced: the provider routes or
  silences it once, when it starts, and never sends it to `host`. So does what the provider logs
  outside calls.
- A call polls `host->cancel_requested()` at least once before its work starts (after its argument
  checks and after taking its session or plan), and `run_batch` also before each set; a call with
  no work to do (a batch of no sets) need not poll. When it reports nonzero, the call stops and
  returns `CANCELLED`. An engine that cannot be interrupted inside one run finishes that run first;
  a run longer than the call's deadline then ends as the runtime's timeout rules say
  ([time-cancellation-timeouts.md](time-cancellation-timeouts.md)).
- Opening a session may compile the model, which can take long: modules SHOULD open sessions, and
  prepare plans, in `create` or `initialize` (lifecycle deadline) rather than in `invoke`.
- Kernel functions take no host, so they never log through one and never poll.

## describe

`describe(context, out_json)` writes the service's identity. For `test-simulation`:

```json
{"engine":{"name":"test-simulation-engine","version":"1.0.0"},"methods":["analytic"],"model_formats":["test-decay"],"service":"praenora.simulation","version":{"major":0,"minor":0}}
```

- `service` is `"praenora.simulation"`; `version` is `{"major": 0, "minor": <header.abi_minor>}`.
- `engine` is the `engine` object of the provider's manifest, which the run fingerprint records
  ([runtime-services.md](runtime-services.md#fingerprint)).
- `model_formats` lists the model formats `open_session` accepts and `methods` the solver methods
  requests may name; neither is empty, and the first entry of each is the default.
- It is thread-safe and free of side effects, and writes the same bytes every time.

## Sessions

`open_session(context, host, model, options_json, out_session, error_json)` opens a session for one
model.

- `model` is the model's text in its format, at most 64 MiB. It is the model itself, never a path
  or a URI: a provider MUST NOT read files or the network on its behalf.
- `options_json` is `{"format": "<model format>"}` or `{}`; empty bytes count as `{}`. `format`
  defaults to the first of `describe`'s `model_formats`.
- Errors: an unknown field, or a `format` that is not a string, is `INVALID_ARGUMENT` at its
  pointer; a format the provider does not offer is `UNSUPPORTED` at `/format`, with `details.format`
  and `details.model_formats`; a text that is not a model of the format, or that the engine cannot
  compile, is `DATA_INVALID`, with `details.reason` `"model"` and a pointer where the provider can
  locate the problem, or with `details.engine_error`.
- A provider MAY compile a model once and share the compiled model between sessions, plans and
  kernels of the same text and options (a cache). They stay independent of each other.

`describe_model(session, host, out_json, error_json)` writes the model's ids and whether it has
events:

```json
{"events":false,"parameters":["k_x","k_y"],"quantities":["x","y","total"],"states":["x","y"]}
```

- `states`: the model's state variables, in the order of `y` in kernels and of the state columns of
  results;
- `parameters`: what requests (`parameter_values`), plans (`parameters`) and kernels can set;
- `quantities`: what requests (`output.quantities`) and kernels can read; they may include states;
- `events`: whether the model has events (discrete changes at times or conditions the engine
  detects while it integrates, such as SBML events). It is always present.

Each list holds strings without duplicates, and the document is the same for every session of one
model.

## Parameters, initial values and events

These rules hold for both levels.

- **Parameter values take effect as initial values.** The values of parameters that a call sets (a
  request's `parameter_values`, a plan's parameter inputs, a kernel's `set_parameters`) replace the
  model's before anything is computed from them: the model's initial assignments (initial values
  defined from parameters, such as an SBML species whose initial amount is a parameter's value or
  a function of one) are evaluated with them. Then a request's `initial_conditions`, or a plan's
  initial-condition inputs, replace the values of states at `start`; the initial assignments are
  not evaluated again with them (a state whose initial value is defined from another state keeps
  the value computed from that state's original value when only that state is set).
- **Events.** Level A applies the model's events as the engine defines them; this version has no way
  to turn them off (`events_enabled` is `UNSUPPORTED`). A kernel is the model's continuous
  right-hand side only and cannot apply events, so `open_kernel` MUST refuse a model that has
  events, with `UNSUPPORTED` and `details.reason` `"events"`.

## Level A: simulate

`simulate(session, host, request_json, result_json, error_json)` runs one time course and writes its
result to `result_json`.

### Requests

A request uses the field names of the simulation configuration of `ode-uq-spec` 0.0.0
([feasibility/libroadrunner.md](../feasibility/libroadrunner.md#alignment-with-the-ode-uq-spec-simulation-configuration)),
so a module can pass the numeric parts of one through. For example:

```json
{
  "time_domain": {"start": 0, "end": 2},
  "parameter_values": {"k_y": 2},
  "solver": {"method": "analytic"},
  "output": {"save_at": "explicit_times", "times": [0, 1, 2], "quantities": ["total"],
             "include_states": true}
}
```

| Field | Required | Meaning |
|---|---|---|
| `time_domain` | yes | `{"start", "end"}`: finite numbers, `end` greater than `start`. The run starts at `start` from the initial state. |
| `initial_conditions` | no | `{"<state id>": <number>}`: the values of states at `start`, replacing the model's, after the parameter values took effect ([Parameters, initial values and events](#parameters-initial-values-and-events)). |
| `parameter_values` | no | `{"<parameter id>": <number>}`: parameter values, replacing the model's; they take effect as initial values. |
| `solver` | no | The method and its settings ([Solver settings](#solver-settings)). |
| `output` | yes | The output times and columns ([Output](#output)). |
| `checks` | no | `{"reject_nonfinite": true, "enforce_time_domain": true}`. Both checks are always on; `false` is `UNSUPPORTED`. |
| `inputs`, `events_enabled` | no | Fields of `ode-uq-spec` that this version does not map: `UNSUPPORTED` when present. A module turns constant inputs into `parameter_values`. |

Every number MUST be finite. A field the request does not know, at any level, is
`INVALID_ARGUMENT` at its pointer; a value of the wrong type or out of range is `INVALID_ARGUMENT`
at its pointer; an id the model does not have is `INVALID_ARGUMENT` at its pointer, with
`details.id`. A known feature the provider does not map is `UNSUPPORTED` at its pointer: nothing is
ever ignored.

#### Solver settings

| Setting | Value |
|---|---|
| `method` | One of `describe`'s `methods`; default: the first. Another name is `UNSUPPORTED` at `/solver/method`, with `details.methods`. |
| `stiffness` | `"stiff"`, `"nonstiff"` or `"auto"` (`"auto"` only for a method that switches by itself). |
| `relative_tolerance`, `absolute_tolerance` | Positive numbers. |
| `max_steps` | An integer of at least 1: the method's limit on solver steps, in the method's own scope, which the provider documents (for an adaptive method mapped to CVODE: the steps between two consecutive output times). A run that reaches it fails, with reason `step_limit` when the provider can tell that the limit ended it, otherwise `integrator`. |
| `max_step`, `min_step` | The largest (positive) and smallest (non-negative) step size. |
| `seed` | An integer from 0 to 2^53 − 1, which a stochastic method requires (`INVALID_ARGUMENT` at `/solver/seed` when missing): a module derives it from its step seed ([reproducibility.md](reproducibility.md#seeds)). A provider never uses a default seed. |
| `event_time_tolerance` | Not mapped in this version: `UNSUPPORTED`. |

Each method maps some of these settings, and the provider documents which. A setting of this table
that the chosen method does not map is `UNSUPPORTED` at its pointer, and so is a value of a mapped
setting that the method cannot honor (for example `stiffness` `"auto"` for a method that does not
switch by itself): a provider never substitutes another value silently. Any other name is
`INVALID_ARGUMENT`. The result reports the settings the run used, after defaults, under these names
(`solver`).

#### Output

| Field | Meaning |
|---|---|
| `save_at` | Required: how the output times are chosen. `"explicit_times"`: the times of `times`. `"regular_grid"`: every `step` from `start`. `"solver_steps"`: the solver's own steps, for methods that take steps, and in `simulate` only. `"target_times"`: not offered (`UNSUPPORTED`): a module turns the times of its target data into `explicit_times`. |
| `times` | With `explicit_times` only: a non-empty array of strictly increasing times within `[start, end]`. |
| `step` | With `regular_grid` only: a positive number. The output times are `t_i = start + i * step` for `i` = 0, 1, ..., `n` with `n = floor((end - start) / step + 1e-9)`, computed in double precision; a last time beyond `end` is replaced by `end`. Times that do not strictly increase (a `step` below the precision of `start`) are `INVALID_ARGUMENT` at `/output/step`. |
| `quantities` | An array of quantity ids, each once; default `[]`. |
| `include_states` | Whether the states are columns; default `true`. |
| `dense_output` | `false` or absent; `true` is `UNSUPPORTED`. |

A field that does not belong to the chosen `save_at` (`times` with `regular_grid`, `step` with
`explicit_times`) is `INVALID_ARGUMENT`.

### Results

The columns of a run are `"time"`, then the states in `describe_model`'s order when
`include_states` is true, then the requested quantities in the request's order; the rows are one per
output time, each with one value per column, and the time column holds the output times exactly.
The result of the request above, for `test-simulation`'s model
`{"states": {"x": 10, "y": 4}, "parameters": {"k_x": 0.5, "k_y": 1.5}}`:

```json
{"columns":["time","x","y","total"],"ok":true,"rows":[[0.0,10.0,4.0,14.0],[1.0,6.065306597126334,0.5413411329464508,6.606647730072785],[2.0,3.6787944117144233,0.07326255555493671,3.75205696726936]],"solver":{"method":"analytic"}}
```

A run that fails numerically gives status OK and the result `{"ok": false, "error": <failure>}`,
whose failure document has the `pmp-error` shape with code 11 (`FAILED`) and `details.reason`:

| Reason | Meaning |
|---|---|
| `integrator` | The integrator failed (for example an error of the engine's solver), with `details.engine_error` when the engine raised it. |
| `step_limit` | The solver reached `max_steps`, when the provider can tell; otherwise the failure is `integrator`. |
| `nonfinite` | A value is not finite. JSON cannot carry NaN or infinity, so such a run always fails. |

The failure document MAY add details, such as the column and time of the first value that is not
finite:

```json
{"error":{"code":11,"details":{"column":"x","reason":"nonfinite","service":"praenora.simulation","time":1.0},"message":"praenora.simulation: x is not finite at time 1.0"},"ok":false}
```

**Independent runs.** Every run starts from the model's original state, with what its request (or
its plan's inputs) sets, applied as [Parameters, initial values and
events](#parameters-initial-values-and-events) says; no earlier call on the session changes it.

**Same numbers.** For one provider (one engine identity), model text and options, the same settings
and values give the same doubles: in `simulate`, `run` and `run_batch`, in any session, plan or
thread, and in any process of the same build. `simulate` writes them as canonical JSON, whose
numbers read back as the same doubles.

## Level A: plans

`prepare(session, host, plan_json, out_plan, info_json, error_json)` fixes everything of a run but
its numbers. A plan document is a request whose `initial_conditions` and `parameter_values` objects
are replaced by lists of the inputs:

| Field | Required | Meaning |
|---|---|---|
| `parameters` | no | An array of parameter ids: the first inputs, in this order. |
| `initial_conditions` | no | An array of state ids: the inputs after the parameters, in this order. |
| `time_domain`, `solver`, `output`, `checks` | as in requests | As in requests, except that `save_at` `"solver_steps"` is `UNSUPPORTED`: a plan has a fixed number of rows. |

An unknown id is `INVALID_ARGUMENT` at `/parameters/<i>` or `/initial_conditions/<i>`, with
`details.id`, and so is an id listed twice. `parameter_values` in a plan is an unknown field.

`info_json` receives the plan's shape, before the plan is handed out:

```json
{"columns":["time","total"],"inputs":["k_x","x","y"],"rows":5,"solver":{"method":"analytic"}}
```

`inputs` are the parameters, then the initial conditions; `columns` and `solver` are what `simulate`
reports for the same settings, and `rows` is the number of output times.

### run

`run(plan, host, values, value_count, out, out_count, outcome, failure_json, error_json)` runs the
plan once:

- `value_count` MUST equal the number of inputs and `out_count` the plan's rows times columns;
  otherwise the call returns `INVALID_ARGUMENT`, with the counts in `details`. A value that is not
  finite is `INVALID_ARGUMENT`, with `details.index`, its index in `values`. `outcome` is required.
- The run starts from the model's original state with the inputs set (the parameters first, as
  initial values, then the initial conditions), and writes its values to `out`, row-major: the rows
  and columns, and the numbers, of `simulate`'s result for the same settings.
- `*outcome` is `PRAENORA_SIMULATION_RUN_OK`, or `PRAENORA_SIMULATION_RUN_FAILED` for a numerical
  failure: the status is still OK, every value of `out` is a quiet NaN, and the failure document
  (the `error` of a failed `simulate` result) goes to `failure_json` when it is not NULL.
- When the status is not OK, `*outcome` and `out` are unspecified.

### run_batch

`run_batch(plan, host, values, value_count, set_count, out, out_count, outcomes, error_json)` runs
`set_count` sets of inputs, in order:

- `values` holds `set_count * inputs` doubles (set `i` from `values[i * inputs]`), `out` holds
  `set_count * rows * columns` doubles (set `i` from `out[i * rows * columns]`) and `outcomes`
  `set_count` entries. A count that does not match, or a product that does not fit in 64 bits, is
  `INVALID_ARGUMENT`. Every value is checked to be finite before any set runs (`details.index` is
  the index in `values`, across sets). `set_count` 0 runs nothing.
- Each set gives exactly what `run` gives for its values: its outcome, and its values or NaN. A
  batch writes no failure documents; a module that needs one runs that set again with `run`.
- The batch polls `cancel_requested()` before each set; when it returns `CANCELLED`, the outputs
  and outcomes of the sets that did not run are unspecified.

## Level B: kernels

`open_kernel(session, host, options_json, out_kernel, error_json)` fills a kernel for the session's
model:

- `options_json` is `{"parameters": [ids], "quantities": [ids]}`; empty bytes count as `{}`. A list
  that is left out means every one of `describe_model`'s, in its order; `[]` means none. The ids are
  the kernel's parameters and quantities, in the module's order. An unknown id, or one listed twice,
  is `INVALID_ARGUMENT` at its pointer.
- **Events.** A model that has events has no kernel: `open_kernel` refuses it with `UNSUPPORTED`
  and `details.reason` `"events"` ([Parameters, initial values and
  events](#parameters-initial-values-and-events)).
- **Sizes.** The caller sets `out_kernel->struct_size` to the size it was compiled with, at least
  `PRAENORA_SIMULATION_KERNEL_V0_SIZE` (otherwise `INVALID_ARGUMENT`, and nothing is written). The
  provider fills only the fields that both its own and the caller's struct have, and sets
  `struct_size` to the smaller of the two sizes, `reserved` to 0, the counts, the functions and
  `model`.
- A new kernel has the model's original parameter values.

A kernel is the model's continuous right-hand side only: none of its functions applies events.
`rhs`, `quantities` and `initial_state` are functions of their arguments and the kernel's current
parameters only: no call changes what a later call computes, except `set_parameters`, which also
brings the model's values derived from parameters (initial assignments, and what the engine derives
from them, such as compartment sizes) up to date. The kernel's functions, each called with the
kernel's `model`:

| Function | What it does |
|---|---|
| `rhs(model, t, y, dydt)` | Writes `dydt = f(t, y)` (`state_count` values) for the kernel's current parameters. It changes nothing: neither `y`, nor the kernel's parameters, nor any later result. |
| `set_parameters(model, values)` | Sets the kernel's parameters (`parameter_count` values, in the kernel's order) until the next call, as initial values: the values derived from them are evaluated again. |
| `initial_state(model, y)` | Writes the model's initial state (`state_count` values) for the current parameters, with the model's initial assignments evaluated again. |
| `quantities(model, t, y, out)` | Writes the kernel's quantities (`quantity_count` values, in the kernel's order) at `(t, y)` for the current parameters. |

Each returns `INVALID_ARGUMENT` when `model`, or an array it needs, is NULL (an array of 0 elements
may be NULL), `INTERNAL` when the engine fails unexpectedly (the outputs are then unspecified), and
OK otherwise: values that are not finite, in or out, are computed, never refused. Kernel functions
take no host, so they never log through one and never poll, and they never block, so that a
module's integrator can call them at a high rate. A kernel stays usable until `close_kernel`,
whatever happens to its session.

## Size limits

| What | Limit | Macro | Exceeded |
|---|---|---|---|
| The model text of `open_session` | 64 MiB | `PRAENORA_SIMULATION_MAX_MODEL_SIZE` | `RESOURCE_EXHAUSTED` |
| Each JSON document passed to the service: session and kernel options, requests, plans | 16 MiB | `PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE` | `RESOURCE_EXHAUSTED`, before the document is parsed |
| The values of one run's output, rows times columns (`simulate`, `prepare`) | 4,194,304 (2^22) | `PRAENORA_SIMULATION_MAX_RUN_VALUES` | `RESOURCE_EXHAUSTED` |

The output of `solver_steps` has as many rows as the solver takes steps, known only while the run
goes on: the provider stops such a run with `RESOURCE_EXHAUSTED` when it reaches the limit. A
batch's buffers are the module's, so only the 64-bit rule of `run_batch` bounds them. A provider MAY
also return `RESOURCE_EXHAUSTED` when it runs out of memory.

## Errors

| Condition | Status |
|---|---|
| A NULL session, plan, kernel, out-pointer or required sink; bytes whose data is NULL with a size; buffer counts that do not match | `INVALID_ARGUMENT` |
| A document that is not a JSON object; an unknown field; a value of the wrong type or out of range; an unknown or repeated id | `INVALID_ARGUMENT`, `details.pointer` |
| A model text that is not a model of its format, or that the engine cannot compile | `DATA_INVALID` |
| A model format, method, setting or `ode-uq-spec` field the provider does not map, or a setting's value the method cannot honor | `UNSUPPORTED`, `details.pointer` |
| `open_kernel` for a model that has events | `UNSUPPORTED`, `details.reason` `"events"` |
| A session or plan that another call is using | `INVALID_STATE` |
| `cancel_requested()` reported nonzero | `CANCELLED` |
| A size limit; out of memory | `RESOURCE_EXHAUSTED` |
| A sink refused a write | the status `write` returned |
| A run failed numerically | OK; the failure is reported as data |
| Any other failure (an unexpected exception inside the provider) | `INTERNAL` |

A call checks its arguments first, then takes its session or plan, then polls `cancel_requested()`,
then reads its document and works. When a document has several problems, which one is reported is
the provider's choice, but the same document always gets the same error.

## Reproducibility

- A run's fingerprint records each service it resolved, with its provider and the provider's engine
  object, which `describe` reports as `engine`
  ([runtime-services.md](runtime-services.md#fingerprint)). Results computed through the service
  are byte-identical only for the same engine identity, model text, options, settings and values,
  on the same platform with the same build ([reproducibility.md](reproducibility.md)).
- A module SHOULD record what it asked for and what the service used (the result's or plan's
  `solver`) where its results are reported, in its outputs or diagnostics.

## Evolution

A later minor of the service MAY append functions to the table and fields to the kernel, and MAY
add optional request, plan and option fields and document members that a module can ignore. It never
changes the meaning of what exists. A provider of a later minor still refuses what it does not map,
so a module that sends a field a provider does not know gets `INVALID_ARGUMENT` or `UNSUPPORTED`,
never a silent default. A breaking change is a new major, with a new table.

## The roadrunner provider

The provider `roadrunner` (`src/providers/roadrunner/`) offers `praenora.simulation@0.0` for SBML
models, with libRoadRunner's C++ API as its engine. It is built only with the build option
`PRAENORA_WITH_ROADRUNNER` (OFF by default;
[platforms.md](../platforms.md#the-roadrunner-provider)), links libRoadRunner, LLVM, libSBML and
SUNDIALS statically and exports only the provider entry point. Its version is the runtime's. In the
build tree it sits where the build tree's runtime looks for the providers of its installation, and
an installation holds it in `<prefix>/lib/praenora/providers/roadrunner/`. This section states what
the rules above leave to a provider ([ADR-0027](../adr/ADR-0027-simulation-service-v0.md)).

**Identity.** `describe` writes, for the build checked on macOS arm64:

```json
{"engine":{"jit":"mcjit","libsbml":"5.21.2","llvm":"13.0.1","llvm_optimization":"none","name":"libroadrunner","sundials":"7.9.0","version":"2.11.0"},"methods":["cvode"],"model_formats":["sbml"],"service":"praenora.simulation","version":{"major":0,"minor":0}}
```

The engine object is the manifest's: the versions of libRoadRunner (from its headers), LLVM (from
`llvm-config`), libSBML and SUNDIALS (from their CMake packages) that the build links, LLVM's MCJit
backend and no LLVM optimization passes. A provider built against other versions has another engine
object, and so another run fingerprint.

**Model format `sbml`.** The model text is an SBML document of Level 1, 2 or 3 (checked with
Level 1 Version 2, Level 2 Version 4, and Level 3 Versions 1 and 2). `open_session` refuses with
`DATA_INVALID` and `details.reason` `"model"`:

- text that does not start with `<` after an optional UTF-8 byte order mark and white space: the
  model is the document's text;
- text that holds a NUL character or a document type declaration (`<!DOCTYPE`, in any case of
  letters; refused before any XML parser sees the text);
- text that libRoadRunner would not take as document text, by its own test
  (`rr::SBMLReader::is_sbml`): libRoadRunner would read it as a path or a URI. So libRoadRunner
  never reads a path or a URI on behalf of a model;
- a document for which libSBML reports an error or a fatal error when it reads it (`details.line`
  and `details.column` locate the first). Among those: a Level 3 package that the document
  requires (`required="true"`) and that libSBML cannot interpret;
- a Level 3 document that requires a package libSBML knows, and any Level 3 document that declares
  the `comp` package, which can name other documents by URI or path. Packages exist in Level 3
  only; those that a document does not require are ignored, as SBML allows;
- a model that libRoadRunner cannot simulate, by libRoadRunner's own test: a document with the
  `fbc` package (its `required` attribute set) whose reactions have no kinetic law with math, a
  flux-balance model;
- a document libRoadRunner cannot compile, with `details.engine_error` (the packages qual, spatial
  and multi among them).

**Ids.** `describe_model` lists, with conserved-moiety analysis off:

- `states`: libRoadRunner's state vector: the variables of rate rules, then the floating species
  that no rule defines, each in libRoadRunner's order. The values of species are amounts.
- `parameters`: the model's global parameters, in libRoadRunner's order, except those that an
  assignment rule, a rate rule or an initial assignment defines, and those that an initial
  assignment of a species reference (a stoichiometry) or a Level 2 `stoichiometryMath` uses,
  directly or through other initial assignments and assignment rules: libRoadRunner keeps such
  stoichiometries at the values they had when the model was loaded. Compartment sizes and initial
  amounts are not parameters; a request sets states with `initial_conditions`.
- `quantities`, in this order: the states (by their ids); the amounts of the other floating species
  (those an assignment rule defines); `[<species>]`, the concentration of each floating species;
  the id of each reaction, for its rate; the global parameters that an assignment rule defines. SBML
  ids hold no `[`, so the ids are unique.
- `events`: whether the model has SBML events.

Boundary species and compartment sizes are neither parameters nor quantities in this version.

**Parameter values and initial conditions.** Every run, and every kernel, starts from
libRoadRunner's reset to the model as loaded (SBML initialization included). Parameter values then
become initial values, and a reset to the initial values without SBML initialization evaluates the
initial assignments with them; then `initial_conditions` replace the states' values. Before each
run the model's random number generator is seeded with 0: libRoadRunner draws the order of
simultaneous events of equal priority, which SBML leaves open.

**Method `cvode`**, the only one: SUNDIALS' CVODE as libRoadRunner drives it.

| Setting | CVODE |
|---|---|
| `stiffness` | `"stiff"` (BDF; the default) or `"nonstiff"` (Adams). `"auto"` is `UNSUPPORTED`: CVODE does not switch by itself. |
| `relative_tolerance` | The relative tolerance; default 1e-6. |
| `absolute_tolerance` | One absolute tolerance for every state, in the state's units (amounts for species), as given; default 1e-12. (A single value given to libRoadRunner would be scaled by each state's initial value; the provider passes one value per state instead.) |
| `max_steps` | CVODE's limit on internal steps between two consecutive output times, an integer from 1 to 2147483647; default 20000. A run that reaches it fails with reason `step_limit`. In a `solver_steps` run each call of CVODE takes one step, so the limit is never reached there. |
| `max_step`, `min_step` | CVODE's largest and smallest step; by default no limit and 0. A `min_step` above `max_step` is `INVALID_ARGUMENT`. |
| `seed`, `event_time_tolerance` | Not mapped: `UNSUPPORTED`. |

The `solver` of results and plans reports `method`, `stiffness`, `relative_tolerance`,
`absolute_tolerance` and `max_steps` always, and `max_step` and `min_step` when they were given.

**Output times.** libRoadRunner integrates from `start` through each output time. When the first
output time comes after `start`, the run integrates from `start` and leaves that row out; when the
only output time is `start`, the run integrates to `end` and reports only the row at `start`, so it
fails when the time course fails. `save_at` `"solver_steps"` (in `simulate` only) reports a row at
`start`, one after each step of CVODE, and one at `end`, from CVODE's own interpolation; a run whose
rows would exceed `PRAENORA_SIMULATION_MAX_RUN_VALUES` ends with `RESOURCE_EXHAUSTED`. The time
column holds the requested times exactly, and in a `solver_steps` run the solver's times, the first
`start` and the last `end`.

**Events.** libRoadRunner finds event triggers while it integrates and applies the events. Before
each run the provider empties libRoadRunner's queue of pending event assignments (its reset keeps
the assignments of delayed events that an earlier run left, and its copies queue some of their
own), and has its integrator evaluate the triggers at `start` against their initial values, as
libRoadRunner does when it loads a model and the run starts at 0: an event whose trigger becomes
true at `start` is applied there, or after its delay from `start`, once. After every
`solver_steps` run (libRoadRunner's variable-step mode keeps an event or switch it found at the end
of a step for its next call), and after a run that ended with an exception, the next run of the
object takes a fresh copy of the compiled model. So every run is independent of the earlier ones.

**Numerical failures.** A CVODE failure (`rr::IntegratorException`) has reason `step_limit` when its
message names `CV_TOO_MUCH_WORK` and `integrator` otherwise; any other exception during the time
course is reason `integrator`; both carry `details.engine_error`. A value that is not finite is
reason `nonfinite`, with the column and the time.

**Cancellation.** Besides the polls of every provider, a run polls `cancel_requested()` after each
output interval (after each step in a `solver_steps` run), through libRoadRunner's integrator
listener, and returns `CANCELLED` when it reports nonzero.

**Kernels.** Each kernel has its own libRoadRunner object. `rhs` is libRoadRunner's
`getStateVectorRate(t, y, dydt)`; `set_parameters` sets initial values and resets the model to them
(initial assignments and compartment sizes follow) and keeps the initial state, which
`initial_state` copies; `quantities` sets the time and the state, then reads libRoadRunner's values.
An exception of libRoadRunner makes a kernel function return `INTERNAL`. Kernel functions take no
lock of the provider; `set_parameters` allocates.

**Compiled models and threads.** Compiled models are cached by the SHA-256 of the engine object, the
options and the model's bytes: up to 16, the least recently used first out (one that a session,
plan or kernel uses stays alive). A call that finds another compiling the same text waits for it.
Each session (for `simulate`), plan and kernel has its own libRoadRunner object, copied from the
compiled model. libRoadRunner does not state that making, copying and destroying its objects is
thread-safe, so the provider does those, and compiling, one at a time in the process: `close_*`
and the first `simulate` of a session, `prepare` and `open_kernel` may wait for another call's
compilation of any model. A call that waits polls `cancel_requested()`, while `close_*` wait
without polling. Runs and kernel functions of distinct objects run concurrently.

**In a module host.** A step in isolation mode `process` has the provider loaded and started in its
module host ([process-isolation.md](process-isolation.md#service-providers)): its libRoadRunner,
LLVM and compiled models are that process's, so its kernels' functions are called in the process
that holds them. Each module host compiles the models its step opens, with a cache of its own; it
starts the provider, and so sets the configuration below, once, as the runtime's process does. A
model compiled there gives the results it gives in the runtime's process (checked with a plan and
a kernel of an SBML model, byte for byte), an LLVM error that ends the process ends only the module
host (`MODULE_CRASHED`), and a run that outlives its deadline and grace is ended by killing the
module host.

**Shared compiled models.** In the module host of a step that shares compiled models
([process-isolation.md](process-isolation.md#shared-compiled-models)) the provider host offers the
store, and the provider uses it behind its own cache, under the cache's key (which holds nothing of
the process): a model its cache does not hold is looked for in the store first, and loaded from the
libRoadRunner state saved there (`loadStateS()`) instead of compiled; one that is not there is
compiled, and its state (`saveStateS('b')`) is then kept there. Both happen under the engine lock,
as compiling does. The state is kept in an envelope: the bytes `praenora-roadrunner-state` and a
new line, the engine's identity (the manifest's engine object and the load options), the state's
length and its SHA-256. The provider checks all of it before libRoadRunner reads the state; an
entry that fails a check, or that libRoadRunner cannot load, does not load: the provider logs a
warning through the calling instance's host ("the compiled model in the shared store does not
load", with why) and compiles the model. The checks find an entry that is corrupt or cut short, not
one made on purpose: the store's trust is that of every sharing step's modules. A model loaded from
a state is the model compiled from its text (its ids, the engine's index of each parameter, how each
quantity is read, its events; checked field by field) and gives the same results, byte for byte. Its
checks of the model text run as for a compile. A state larger than the store's entry limit, 128 MiB,
is not kept. The debug record of the call says "roadrunner: compiled the model", "roadrunner: loaded
the compiled model from the shared store" or, from the provider's own cache, "roadrunner: the model
was compiled before".

**Under the restricted sandbox profile.** In a module host under the restricted profile
([process-isolation.md](process-isolation.md#the-restricted-profile)) the provider works as without
it: libRoadRunner and LLVM load after the profile is applied, MCJit generates a model's code in
memory (written, then made executable), and a saved state of the shared compiled models arrives on
the channel; nothing is written to a file, also not to libRoadRunner's temporary directory
(`TEMP_DIR_PATH`, `.`), which it does not use for MCJit. The results are those without the profile.

**Process-wide configuration.** At `start()` the provider sets every key of libRoadRunner's
configuration (`rr::Config`) to libRoadRunner 2.11.0's built-in default, except the random seed
(0): libRoadRunner reads a configuration file of the user's (`ROADRUNNER_CONFIG`, `roadrunner.conf`
in the home directory) on its first use, and cannot change results afterwards. The JIT backend is
MCJit; models are loaded without LLVM optimization passes, without conserved-moiety analysis, with
settable initial values and with LLVM's symbol cache (libRoadRunner's default). The provider never
changes the configuration again.

**Logging.** libRoadRunner's process-wide logger is set to the error level, so its warnings and
below are not recorded, and its messages go to the provider's logger, `provider.<name>`
([runtime-services.md](runtime-services.md#loading)), with the prefix `libRoadRunner:` and one level
lower than libRoadRunner's: its errors as warnings, its critical and fatal messages as errors.
libRoadRunner logs an error for each CVODE failure, also for a run that the service reports as
failed, which is data. The engine's records are context: the service's results and error documents
are what count. Calls log at debug level through the calling instance's host: a session opened
(compiled, found compiled, or loaded from the shared compiled models) and the rows simulated.

## The test provider `test-simulation`

The fixture provider `test-simulation` (`tests/fixtures/providers/test-simulation/`, built for the
tests only and never installed) offers `praenora.simulation@0.0` without an engine, so that the
service checks and modules using the service run in every build.

- **Model format `test-decay`.** The model text is a JSON object

  ```json
  {"states": {"x": "x0", "y": 2}, "parameters": {"x0": 10, "k_x": 0.5, "k_y": 0.25},
   "events": [{"time": 1, "state": "x", "value": 10}]}
  ```

  with at least one state, ids matching `[A-Za-z_][A-Za-z0-9_]*` (states not `total`), one
  parameter `k_<id>` for each state `<id>`, and finite numbers. A state's initial value is a number
  or the id of a parameter (its initial assignment), and the parameters are the `k_<id>` and those
  that states name, no others. Each state decays on its own, `dx/dt = -k_x * x`; a run from `start`
  has `x(t) = x(start) * exp(-k_x * (t - start))`, computed exactly. States and parameters are
  listed in the order of their ids (UTF-8 bytes); the quantities are the states, then `total`, the
  sum of the states in that order.
- **Events.** `events` (optional) declares events, each `{"time", "state", "value"}`. The test
  provider does not simulate them: `simulate` and `prepare` refuse a model that has events with
  `UNSUPPORTED` and `details.reason` `"events"`. This is a property of the toy format, not of the
  service, whose engines apply events; like every provider, it reports them in `describe_model` and
  refuses the model's kernel.
- **Method `analytic`**, which maps no solver setting and has no `solver_steps` output.
- A value that is not finite (a negative `k` large enough to overflow, for example) fails the run
  with reason `nonfinite`, and the failure names the first such column and time.
- It logs a debug message through the calling instance's host when it opens a session (and
  `test-simulation: compiled the model`, or `test-simulation: loaded the model from the shared
  store`) and when it simulates, and `test-simulation started` to its own logger when it starts.
- **Shared compiled models.** Where its provider host offers the store
  ([process-isolation.md](process-isolation.md#shared-compiled-models)), its "compiled" form of a
  model, the canonical parsed model (the model's JSON document as nlohmann/json writes it), is
  kept there under `test-decay:` and the FNV-1a hash of the model's text, with the text; a session
  looks for it there first, and an entry that is not that text's or does not read is a warning and
  a compile. It has no cache of its own.

## Service checks

The service checks are a GoogleTest suite, `SimulationService` (CTest label `contract`;
[`tests/contract/simulation_service_checks.hpp`](../../tests/contract/simulation_service_checks.hpp)),
that load a provider through the runtime's provider loader and test this document against its
table. A **parameter set** names the provider's manifest, a model format and model text (without
events), the model's states, some of its parameters (with the model's values) and quantities, its
initial state (and, when it depends on the parameters, the initial state as a function of them),
solver settings, the exact solution, derivative and quantities to compare with, parameter values for
which a run fails numerically, tolerances, and optionally a model that has events. The checks
cover:

- the table's header and functions, and `describe` (the engine equals the manifest's);
- sessions and `describe_model` (its `events` too), and what `open_session` refuses;
- `simulate` against the exact solution, with parameter values, initial conditions, a later start
  time, regular grids (also one whose step does not divide the time domain, and one whose last time
  is replaced by the end), explicit times that do not begin at the start, a single output time;
  independent and repeatable runs; numerical failures as results; unknown fields and ids
  (`INVALID_ARGUMENT` with pointers) and unmapped features (`UNSUPPORTED`);
- parameter values as initial values, in `simulate`, plans and kernels;
- plans: their inputs and shape, `run` against `simulate` (the same doubles), `run_batch` order and
  outcomes, failure documents and NaN outputs, buffer counts, and what `prepare` refuses;
- kernels: `rhs` against the exact derivative, `initial_state`, `set_parameters` in the module's
  order, results that depend only on the arguments and the current parameters, quantities, NULL
  pointers, the options, and the refusal of a model with events;
- busy sessions and plans (`INVALID_STATE`), calls racing on one session (each served or refused as
  busy), cancellation (`CANCELLED`, also between the sets of a batch), NULL and refusing sinks, host
  APIs whose `struct_size` ends before a callback, NULL objects, size limits, plans and kernels that
  outlive their session (also when it is closed while they run on other threads), and distinct
  sessions, plans and kernels used concurrently on several threads (run under ThreadSanitizer in the
  `*-tsan` builds).

A provider is checked by instantiating the suite with its parameter sets in a source file of its own
(`tests/contract/simulation_service_test.cpp` does so for `test-simulation`, with three models, and
`tests/contract/simulation_service_roadrunner_test.cpp` for `roadrunner`, with two SBML models whose
solutions are known exactly). The fixture module `simulating-module` uses the service from a module,
through the runtime: its test vectors run plans, a kernel and a numerical failure in the contract
checks, and an integration test runs it in a workflow.

## References

- [ADR-0027](../adr/ADR-0027-simulation-service-v0.md): the simulation service and its providers.
- [ADR-0026](../adr/ADR-0026-runtime-service-providers.md) and
  [runtime-services.md](runtime-services.md): service providers, resolution and the fingerprint.
- [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md): runtime services through the
  host API.
- [module-abi.md](module-abi.md): conventions, the host API and sinks.
- [threading-and-ownership.md](threading-and-ownership.md): threads and ownership across the ABI.
- [reproducibility.md](reproducibility.md): canonical JSON, seeds and the fingerprint.
- [feasibility/libroadrunner.md](../feasibility/libroadrunner.md): a libRoadRunner-backed provider
  and the alignment with `ode-uq-spec`.
