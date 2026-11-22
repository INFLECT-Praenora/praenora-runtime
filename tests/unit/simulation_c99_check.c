/* Compiled as C99, with the project's warnings, into praenora_unit_tests. It includes the
 * simulation service header (praenora/sdk/simulation.h), fills a kernel and a service table in
 * plain C, calls through them, and reports the layout the C compiler sees so that SimulationAbi
 * tests can compare it with the C++ view. */
#include <praenora/sdk/simulation.h>

#include <stddef.h>
#include <string.h>

int praenora_simulation_c99_check(void);
void praenora_simulation_c99_layout(size_t* out);

/* Compile-time checks: a false condition gives a negative array size and stops the build. */
#define C99_REQUIRE(name, condition) typedef char c99_require_##name[(condition) ? 1 : -1]

C99_REQUIRE(service_major_is_zero, PRAENORA_SIMULATION_SERVICE_MAJOR == 0u);
C99_REQUIRE(service_minor_is_zero, PRAENORA_SIMULATION_SERVICE_MINOR == 0u);
C99_REQUIRE(table_header_first, offsetof(praenora_simulation_v0, header) == 0);
C99_REQUIRE(table_base_size, PRAENORA_SIMULATION_V0_BASE_SIZE == sizeof(praenora_simulation_v0));
C99_REQUIRE(kernel_v0_size,
            PRAENORA_SIMULATION_KERNEL_V0_SIZE == sizeof(praenora_simulation_kernel));
C99_REQUIRE(outcomes_differ, PRAENORA_SIMULATION_RUN_OK != PRAENORA_SIMULATION_RUN_FAILED);

/* A kernel of dx/dt = -k * x for one state x and one parameter k, whose quantity is 2 * x. */
typedef struct decay {
    double k;
    double x0;
} decay;

static praenora_status PRAENORA_CALL decay_rhs(void* model, double t, const double* y,
                                               double* dydt) {
    (void)t;
    if (model == NULL || y == NULL || dydt == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    dydt[0] = -((const decay*)model)->k * y[0];
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL decay_set_parameters(void* model, const double* values) {
    if (model == NULL || values == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    ((decay*)model)->k = values[0];
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL decay_initial_state(void* model, double* y) {
    if (model == NULL || y == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    y[0] = ((const decay*)model)->x0;
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL decay_quantities(void* model, double t, const double* y,
                                                      double* out) {
    (void)t;
    if (model == NULL || y == NULL || out == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    out[0] = 2.0 * y[0];
    return PRAENORA_STATUS_OK;
}

/* A table whose only working functions are describe, open_kernel and close_kernel; the others
 * report UNSUPPORTED, which is enough to call through every pointer from C. */
static decay the_decay;
static int kernels_open = 0;

static praenora_status write_text(praenora_sink* sink, const char* text) {
    if (sink == NULL || sink->write == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return sink->write(sink->context, text, strlen(text));
}

static praenora_status PRAENORA_CALL c_describe(void* context, praenora_sink* out_json) {
    (void)context;
    return write_text(out_json, "{\"service\":\"" PRAENORA_SIMULATION_SERVICE "\"}");
}

static praenora_status PRAENORA_CALL c_open_session(void* context, const praenora_host_api* host,
                                                    praenora_bytes model,
                                                    praenora_bytes options_json,
                                                    praenora_simulation_session** out_session,
                                                    praenora_sink* error_json) {
    (void)context;
    (void)host;
    (void)model;
    (void)options_json;
    (void)error_json;
    if (out_session == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    /* Any non-NULL handle will do for the check: it is never dereferenced. */
    *out_session = (praenora_simulation_session*)&the_decay;
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL c_describe_model(praenora_simulation_session* session,
                                                      const praenora_host_api* host,
                                                      praenora_sink* out_json,
                                                      praenora_sink* error_json) {
    (void)session;
    (void)host;
    (void)error_json;
    return write_text(out_json,
                      "{\"states\":[\"x\"],\"parameters\":[\"k\"],\"quantities\":[\"y\"]}");
}

static praenora_status PRAENORA_CALL c_simulate(praenora_simulation_session* session,
                                                const praenora_host_api* host,
                                                praenora_bytes request_json,
                                                praenora_sink* result_json,
                                                praenora_sink* error_json) {
    (void)session;
    (void)host;
    (void)request_json;
    (void)result_json;
    (void)error_json;
    return PRAENORA_STATUS_UNSUPPORTED;
}

static praenora_status PRAENORA_CALL c_prepare(praenora_simulation_session* session,
                                               const praenora_host_api* host,
                                               praenora_bytes plan_json,
                                               praenora_simulation_plan** out_plan,
                                               praenora_sink* info_json,
                                               praenora_sink* error_json) {
    (void)session;
    (void)host;
    (void)plan_json;
    (void)info_json;
    (void)error_json;
    if (out_plan != NULL) {
        *out_plan = NULL;
    }
    return PRAENORA_STATUS_UNSUPPORTED;
}

static praenora_status PRAENORA_CALL c_run(praenora_simulation_plan* plan,
                                           const praenora_host_api* host, const double* values,
                                           uint64_t value_count, double* out, uint64_t out_count,
                                           int32_t* outcome, praenora_sink* failure_json,
                                           praenora_sink* error_json) {
    (void)plan;
    (void)host;
    (void)values;
    (void)value_count;
    (void)out;
    (void)out_count;
    (void)failure_json;
    (void)error_json;
    if (outcome != NULL) {
        *outcome = PRAENORA_SIMULATION_RUN_FAILED;
    }
    return PRAENORA_STATUS_UNSUPPORTED;
}

static praenora_status PRAENORA_CALL c_run_batch(praenora_simulation_plan* plan,
                                                 const praenora_host_api* host,
                                                 const double* values, uint64_t value_count,
                                                 uint64_t set_count, double* out,
                                                 uint64_t out_count, int32_t* outcomes,
                                                 praenora_sink* error_json) {
    (void)plan;
    (void)host;
    (void)values;
    (void)value_count;
    (void)set_count;
    (void)out;
    (void)out_count;
    (void)outcomes;
    (void)error_json;
    return PRAENORA_STATUS_UNSUPPORTED;
}

static praenora_status PRAENORA_CALL c_open_kernel(praenora_simulation_session* session,
                                                   const praenora_host_api* host,
                                                   praenora_bytes options_json,
                                                   praenora_simulation_kernel* out_kernel,
                                                   praenora_sink* error_json) {
    (void)session;
    (void)host;
    (void)options_json;
    (void)error_json;
    if (out_kernel == NULL || out_kernel->struct_size < PRAENORA_SIMULATION_KERNEL_V0_SIZE) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    the_decay.k = 0.5;
    the_decay.x0 = 8.0;
    out_kernel->struct_size = (uint32_t)PRAENORA_SIMULATION_KERNEL_V0_SIZE;
    out_kernel->reserved = 0u;
    out_kernel->model = &the_decay;
    out_kernel->state_count = 1u;
    out_kernel->parameter_count = 1u;
    out_kernel->quantity_count = 1u;
    out_kernel->rhs = decay_rhs;
    out_kernel->set_parameters = decay_set_parameters;
    out_kernel->initial_state = decay_initial_state;
    out_kernel->quantities = decay_quantities;
    kernels_open += 1;
    return PRAENORA_STATUS_OK;
}

static void PRAENORA_CALL c_close_kernel(praenora_simulation_kernel* kernel) {
    if (kernel != NULL && kernel->model != NULL) {
        kernel->model = NULL;
        kernels_open -= 1;
    }
}

static void PRAENORA_CALL c_close_plan(praenora_simulation_plan* plan) { (void)plan; }

static void PRAENORA_CALL c_close_session(praenora_simulation_session* session) { (void)session; }

static const praenora_simulation_v0 c_table = {
    .header = {.abi_major = PRAENORA_SIMULATION_SERVICE_MAJOR,
               .abi_minor = PRAENORA_SIMULATION_SERVICE_MINOR,
               .struct_size = (uint32_t)sizeof(praenora_simulation_v0),
               .reserved = 0u},
    .context = NULL,
    .describe = c_describe,
    .open_session = c_open_session,
    .describe_model = c_describe_model,
    .simulate = c_simulate,
    .prepare = c_prepare,
    .run = c_run,
    .run_batch = c_run_batch,
    .open_kernel = c_open_kernel,
    .close_kernel = c_close_kernel,
    .close_plan = c_close_plan,
    .close_session = c_close_session,
};

/* A sink that appends into a fixed buffer. */
typedef struct capture {
    char data[128];
    size_t size;
} capture;

static praenora_status PRAENORA_CALL capture_write(void* context, const char* data, size_t size) {
    capture* sink = (capture*)context;
    if (size > sizeof sink->data - sink->size) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    return PRAENORA_STATUS_OK;
}

int praenora_simulation_c99_check(void) {
    const praenora_simulation_v0* table = &c_table;
    const void* as_service = table; /* what get_service returns */
    const praenora_api_header* header = (const praenora_api_header*)as_service;
    capture out = {{0}, 0};
    praenora_sink out_sink;
    praenora_sink error_sink;
    praenora_simulation_session* session = NULL;
    praenora_simulation_plan* plan = NULL;
    praenora_simulation_kernel kernel;
    praenora_bytes none;
    double y[1];
    double dydt[1];
    double quantity[1];
    double k = 2.0;
    int32_t outcome = PRAENORA_SIMULATION_RUN_OK;

    kernels_open = 0;
    if (strcmp(PRAENORA_SIMULATION_SERVICE, "praenora.simulation") != 0 ||
        strlen(PRAENORA_SIMULATION_SERVICE) != 19) {
        return 1;
    }
    if (header->abi_major != PRAENORA_SIMULATION_SERVICE_MAJOR || header->reserved != 0u ||
        header->struct_size < PRAENORA_SIMULATION_V0_BASE_SIZE) {
        return 2;
    }
    out_sink.context = &out;
    out_sink.write = capture_write;
    error_sink = out_sink;
    if (table->describe(table->context, &out_sink) != PRAENORA_STATUS_OK ||
        out.size != strlen("{\"service\":\"praenora.simulation\"}")) {
        return 3;
    }
    none.data = NULL;
    none.size = 0;
    if (table->open_session(table->context, NULL, none, none, &session, &error_sink) !=
            PRAENORA_STATUS_OK ||
        session == NULL) {
        return 4;
    }
    out.size = 0;
    if (table->describe_model(session, NULL, &out_sink, &error_sink) != PRAENORA_STATUS_OK ||
        out.size == 0) {
        return 5;
    }
    if (table->simulate(session, NULL, none, &out_sink, &error_sink) !=
            PRAENORA_STATUS_UNSUPPORTED ||
        table->prepare(session, NULL, none, &plan, &out_sink, &error_sink) !=
            PRAENORA_STATUS_UNSUPPORTED ||
        plan != NULL ||
        table->run(plan, NULL, NULL, 0u, NULL, 0u, &outcome, NULL, &error_sink) !=
            PRAENORA_STATUS_UNSUPPORTED ||
        outcome != PRAENORA_SIMULATION_RUN_FAILED ||
        table->run_batch(plan, NULL, NULL, 0u, 0u, NULL, 0u, NULL, &error_sink) !=
            PRAENORA_STATUS_UNSUPPORTED) {
        return 6;
    }

    memset(&kernel, 0, sizeof kernel);
    kernel.struct_size = (uint32_t)PRAENORA_SIMULATION_KERNEL_V0_SIZE - 1u;
    if (table->open_kernel(session, NULL, none, &kernel, &error_sink) !=
        PRAENORA_STATUS_INVALID_ARGUMENT) {
        return 7;
    }
    kernel.struct_size = (uint32_t)sizeof kernel;
    if (table->open_kernel(session, NULL, none, &kernel, &error_sink) != PRAENORA_STATUS_OK ||
        kernel.model == NULL || kernel.state_count != 1u || kernel.reserved != 0u ||
        kernel.struct_size != PRAENORA_SIMULATION_KERNEL_V0_SIZE || kernels_open != 1) {
        return 8;
    }
    if (kernel.initial_state(kernel.model, y) != PRAENORA_STATUS_OK || y[0] != 8.0 ||
        kernel.rhs(kernel.model, 0.0, y, dydt) != PRAENORA_STATUS_OK || dydt[0] != -4.0 ||
        kernel.set_parameters(kernel.model, &k) != PRAENORA_STATUS_OK ||
        kernel.rhs(kernel.model, 0.0, y, dydt) != PRAENORA_STATUS_OK || dydt[0] != -16.0 ||
        kernel.quantities(kernel.model, 0.0, y, quantity) != PRAENORA_STATUS_OK ||
        quantity[0] != 16.0) {
        return 9;
    }
    if (kernel.rhs(kernel.model, 0.0, NULL, dydt) != PRAENORA_STATUS_INVALID_ARGUMENT ||
        kernel.rhs(NULL, 0.0, y, dydt) != PRAENORA_STATUS_INVALID_ARGUMENT) {
        return 10;
    }
    table->close_kernel(&kernel);
    table->close_kernel(NULL);
    table->close_plan(NULL);
    table->close_session(session);
    table->close_session(NULL);
    if (kernels_open != 0 || kernel.model != NULL) {
        return 11;
    }
    return 0;
}

void praenora_simulation_c99_layout(size_t* out) {
    out[0] = sizeof(praenora_simulation_kernel);
    out[1] = offsetof(praenora_simulation_kernel, model);
    out[2] = offsetof(praenora_simulation_kernel, state_count);
    out[3] = offsetof(praenora_simulation_kernel, rhs);
    out[4] = offsetof(praenora_simulation_kernel, quantities);
    out[5] = sizeof(praenora_simulation_v0);
    out[6] = offsetof(praenora_simulation_v0, context);
    out[7] = offsetof(praenora_simulation_v0, describe);
    out[8] = offsetof(praenora_simulation_v0, run_batch);
    out[9] = offsetof(praenora_simulation_v0, close_session);
    out[10] = PRAENORA_SIMULATION_V0_BASE_SIZE;
    out[11] = PRAENORA_SIMULATION_KERNEL_V0_SIZE;
}
