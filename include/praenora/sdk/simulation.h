/* The simulation service praenora.simulation@0: simulating a model through the engine of a
 * service provider (docs/specs/simulation-service.md, which is normative).
 *
 * A module declares the service in its manifest (REQUIRES_SERVICES praenora.simulation@0.0 in
 * praenora_add_module()) and gets this table from its host API:
 *     get_service(context, PRAENORA_SIMULATION_SERVICE, 19, PRAENORA_SIMULATION_SERVICE_MAJOR).
 * It accepts the table when header.abi_major is PRAENORA_SIMULATION_SERVICE_MAJOR, header.reserved
 * is 0 and header.struct_size is at least PRAENORA_SIMULATION_V0_BASE_SIZE; a later minor version
 * only appends functions, which a module uses only when struct_size covers them.
 *
 * Two levels of access to one compiled model:
 *   - Level A, simulate: simulate() runs one time course described by a JSON request and writes a
 *     JSON result. For many runs, prepare() fixes everything of a run but its numbers in a plan,
 *     and run() and run_batch() fill module-owned double buffers.
 *   - Level B, the compiled model: open_kernel() fills a kernel, a table of C functions (right-hand
 *     side, initial state, parameters, quantities) for modules with integrators of their own.
 *
 * Parameter values (a request's parameter_values, a plan's parameter inputs, a kernel's
 * set_parameters) take effect as the model's initial values: the model's initial assignments are
 * evaluated with them; then initial conditions replace state values at the start. Level A applies
 * the model's events as the engine defines them (this version cannot turn them off). A kernel is
 * the model's continuous right-hand side only: open_kernel() refuses a model that has events
 * (UNSUPPORTED, details.reason "events"), and describe_model() reports whether a model has any.
 *
 * Conventions are those of abi.h: only fixed-width integers, size_t, pointers, length-delimited
 * UTF-8 (praenora_bytes) and sinks cross the table; no exception or longjmp leaves a function.
 *   - Sinks are module-owned: the provider writes to them during the call only. Every sink
 *     argument is required (INVALID_ARGUMENT when it or its write function is NULL), except
 *     failure_json. A refused write ends the call with the status write() returned.
 *   - Inputs (praenora_bytes, double arrays) are borrowed for the call. A pointer may be NULL only
 *     when the number of elements it points to is 0.
 *   - Sessions, plans and kernels are allocated and freed by the provider. Plans and kernels keep
 *     the compiled model alive on their own: closing a session does not invalidate its plans or
 *     kernels. Each is closed exactly once, with the table's close function; close_*(NULL) does
 *     nothing. A module closes all of them before its destroy() returns.
 *   - Threading: describe() and open_session() may be called from any thread at any time. A
 *     session serves one call at a time and a plan one call at a time: a call on a session or plan
 *     that is busy with another call returns INVALID_STATE. Different sessions, plans and kernels
 *     may be used at the same time from different threads. A kernel's functions MUST be called by
 *     one thread at a time; this is not detected.
 *   - host: the calling instance's host API, valid for the call (NULL means: no logging and no
 *     cancellation). Records a call can attribute to itself go through it; what the engine logs
 *     through its own process-wide logging goes to the provider's logger, or nowhere, never to
 *     host. The provider polls cancel_requested() before work starts and between the items of a
 *     batch, returning CANCELLED, and never keeps host. Kernel functions take no host, never log
 *     and never poll.
 *   - Statuses are the general codes of status.h, or the status a refusing sink returned. A failed
 *     call writes an error document of the pmp-error shape {"code", "message", "details"} to
 *     error_json, with details.service "praenora.simulation" and, for a failure the engine raised,
 *     details.engine_error. A numerical failure of a run is data, not an error: the call returns
 *     OK and reports the run as failed.
 *
 * C99 and C++. Besides abi.h and status.h, this header depends only on <stddef.h> and
 * <stdint.h>. */
#ifndef PRAENORA_SDK_SIMULATION_H
#define PRAENORA_SDK_SIMULATION_H

#include <stddef.h>
#include <stdint.h>

#include "abi.h"
#include "status.h"

/* The service's name and version. A module asks for the major; the table's header reports the
 * minor the provider implements. */
#define PRAENORA_SIMULATION_SERVICE "praenora.simulation"
#define PRAENORA_SIMULATION_SERVICE_MAJOR 0u
#define PRAENORA_SIMULATION_SERVICE_MINOR 0u

/* The outcome of one run (run() and run_batch()). */
#define PRAENORA_SIMULATION_RUN_OK 0     /* the run completed; its output holds its values */
#define PRAENORA_SIMULATION_RUN_FAILED 1 /* numerical failure, reported as data */

/* Size limits. Larger inputs and outputs are refused with RESOURCE_EXHAUSTED. */
/* The model text of open_session(), in bytes. */
#define PRAENORA_SIMULATION_MAX_MODEL_SIZE ((size_t)64 * 1024 * 1024)
/* Each JSON document passed to the service (options, request, plan), in bytes. */
#define PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE ((size_t)16 * 1024 * 1024)
/* The values of one run's output, rows times columns (simulate() and prepare()). */
#define PRAENORA_SIMULATION_MAX_RUN_VALUES ((uint64_t)1 << 22)

#ifdef __cplusplus
extern "C" {
#endif

/* A model opened for simulation: opaque, owned by the provider. */
typedef struct praenora_simulation_session praenora_simulation_session;
/* A prepared run of a session's model, all but its numbers fixed: opaque, owned by the provider. */
typedef struct praenora_simulation_plan praenora_simulation_plan;

/* Level B: the compiled model as C functions; filled by open_kernel(). The struct is the
 * caller's; `model` and what it points to are the provider's, freed by close_kernel(). */
typedef struct praenora_simulation_kernel {
    uint32_t struct_size;     /* the caller sets the size it was compiled with; the provider then
                                 sets the size of the fields it filled (the smaller of the two) */
    uint32_t reserved;        /* 0 */
    void* model;              /* opaque per-kernel model state; pass it to every function */
    uint64_t state_count;     /* length of y and dydt; order: describe_model "states" */
    uint64_t parameter_count; /* the kernel's parameters, in the order the module chose */
    uint64_t quantity_count;  /* the kernel's quantities, in the order the module chose */
    /* The model's continuous right-hand side only: no function applies events. rhs, quantities
     * and initial_state depend only on their arguments and the kernel's current parameters: no
     * call changes what a later call computes, except set_parameters. Every function returns
     * INVALID_ARGUMENT when `model`, or an array it needs, is NULL (an array of 0 elements may be
     * NULL), INTERNAL when the engine fails unexpectedly (outputs unspecified), and OK otherwise;
     * non-finite results are returned as computed. */
    /* clang-format off */
    /* dydt = f(t, y) with the kernel's current parameters; changes nothing. */
    praenora_status (PRAENORA_CALL *rhs)(void* model, double t, const double* y, double* dydt);
    /* Sets the kernel's parameters (parameter_count values), and with them the model's values
       derived from parameters (the initial assignments are evaluated again). */
    praenora_status (PRAENORA_CALL *set_parameters)(void* model, const double* values);
    /* The model's initial state for the current parameters (initial assignments re-evaluated). */
    praenora_status (PRAENORA_CALL *initial_state)(void* model, double* y);
    /* The kernel's quantities at (t, y) (quantity_count values). */
    praenora_status (PRAENORA_CALL *quantities)(void* model, double t, const double* y,
                                                double* out);
    /* clang-format on */
} praenora_simulation_kernel;

/* Size of the v0.0 kernel, up to the end of quantities: the smallest struct_size a caller may
 * pass to open_kernel(), and what a provider of minor 0 reports. */
#define PRAENORA_SIMULATION_KERNEL_V0_SIZE                                                         \
    (offsetof(praenora_simulation_kernel, quantities) +                                            \
     sizeof(((praenora_simulation_kernel*)0)->quantities))

/* The v0 table. Every function is required. */
typedef struct praenora_simulation_v0 {
    praenora_api_header header; /* abi_major: the service major (0); abi_minor; struct_size */
    void* context;              /* the provider's; pass it to describe and open_session */
    /* clang-format off */
    /* {"service", "version" {major, minor}, "engine" {...}, "model_formats": [...],
        "methods": [...]}. Thread-safe, free of side effects. */
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);
    /* Opens a session for one model. options_json: {"format": "<model format>"} or {} (empty
       bytes count as {}). On success *out_session holds the session; close it with
       close_session. */
    praenora_status (PRAENORA_CALL *open_session)(void* context, const praenora_host_api* host,
        praenora_bytes model, praenora_bytes options_json,
        praenora_simulation_session** out_session, praenora_sink* error_json);
    /* {"states": [ids], "parameters": [ids], "quantities": [ids], "events": true|false} */
    praenora_status (PRAENORA_CALL *describe_model)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_sink* out_json, praenora_sink* error_json);
    /* Level A, JSON: one time course from the model's original state. Writes {"ok": true,
       "columns", "rows", "solver"} to result_json, or {"ok": false, "error": {...}} with status
       OK for a numerical failure. */
    praenora_status (PRAENORA_CALL *simulate)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_bytes request_json, praenora_sink* result_json,
        praenora_sink* error_json);
    /* Level A, numeric: fixes everything but the numbers. plan_json: {"parameters": [ids],
       "initial_conditions": [ids], "time_domain", "solver", "output"}. info_json: {"inputs":
       [the parameters then the initial conditions], "columns": [...], "rows": n, "solver": {...}}.
       On success *out_plan holds the plan; close it with close_plan. */
    praenora_status (PRAENORA_CALL *prepare)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_bytes plan_json,
        praenora_simulation_plan** out_plan, praenora_sink* info_json, praenora_sink* error_json);
    /* One run: values has input_count doubles; out has rows*columns doubles, row-major.
       failure_json may be NULL; it receives the failure document when *outcome is RUN_FAILED. */
    praenora_status (PRAENORA_CALL *run)(praenora_simulation_plan* plan,
        const praenora_host_api* host, const double* values, uint64_t value_count, double* out,
        uint64_t out_count, int32_t* outcome, praenora_sink* failure_json,
        praenora_sink* error_json);
    /* set_count runs in order: values has set_count*input_count doubles, out
       set_count*rows*columns, outcomes set_count entries. */
    praenora_status (PRAENORA_CALL *run_batch)(praenora_simulation_plan* plan,
        const praenora_host_api* host, const double* values, uint64_t value_count,
        uint64_t set_count, double* out, uint64_t out_count, int32_t* outcomes,
        praenora_sink* error_json);
    /* Level B. options_json: {"parameters": [ids], "quantities": [ids]}. The caller sets
       out_kernel->struct_size to the size it was compiled with; the provider fills the rest. A
       model with events is refused (UNSUPPORTED, details.reason "events"). */
    praenora_status (PRAENORA_CALL *open_kernel)(praenora_simulation_session* session,
        const praenora_host_api* host, praenora_bytes options_json,
        praenora_simulation_kernel* out_kernel, praenora_sink* error_json);
    void (PRAENORA_CALL *close_kernel)(praenora_simulation_kernel* kernel);
    void (PRAENORA_CALL *close_plan)(praenora_simulation_plan* plan);
    void (PRAENORA_CALL *close_session)(praenora_simulation_session* session);
    /* clang-format on */
} praenora_simulation_v0;

/* Size of the v0.0 table, up to the end of close_session: a table of major 0 is at least this
 * large. Frozen at the v0.0 layout, as PRAENORA_HOST_API_V0_BASE_SIZE is. */
#define PRAENORA_SIMULATION_V0_BASE_SIZE                                                           \
    (offsetof(praenora_simulation_v0, close_session) +                                             \
     sizeof(((praenora_simulation_v0*)0)->close_session))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_SDK_SIMULATION_H */
