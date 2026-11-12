/* Compiled as C99, with the project's warnings, into praenora_unit_tests. It includes the SDK's C
 * headers, builds a function table and a host API in plain C, drives them, and reports the layout
 * the C compiler sees so that SdkAbi tests can compare it with the C++ view. */
#define PRAENORA_MODULE_IMPLEMENTATION /* also declares the module entry point */
#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>
#include <praenora/sdk/version.h>

#include "test_gate.h"

#include <stddef.h>
#include <string.h>

int praenora_sdk_c99_check(void);
void praenora_sdk_c99_layout(size_t* out);

/* Compile-time checks: a false condition gives a negative array size and stops the build. */
#define C99_REQUIRE(name, condition) typedef char c99_require_##name[(condition) ? 1 : -1]

C99_REQUIRE(abi_major_is_zero, PRAENORA_ABI_MAJOR == 0u);
C99_REQUIRE(header_size, sizeof(praenora_api_header) == 16);
C99_REQUIRE(header_first, offsetof(praenora_module_api_v0, header) == 0);
C99_REQUIRE(module_base_size,
            PRAENORA_MODULE_API_V0_BASE_SIZE == offsetof(praenora_module_api_v0, diagnostics));
C99_REQUIRE(host_base_size, PRAENORA_HOST_API_V0_BASE_SIZE == sizeof(praenora_host_api));
C99_REQUIRE(host_base_size_frozen,
            PRAENORA_HOST_API_V0_BASE_SIZE ==
                offsetof(praenora_host_api, get_service) + sizeof(void (*)(void)));
C99_REQUIRE(status_size, sizeof(praenora_status) == 4);
C99_REQUIRE(bytes_size, sizeof(praenora_bytes) == 2 * sizeof(void*));
C99_REQUIRE(gate_header_first, offsetof(praenora_test_gate_v0, header) == 0);

/* A sink that appends into a fixed buffer and counts the writes. */
typedef struct capture {
    char data[256];
    size_t size;
    int writes;
} capture;

static praenora_status PRAENORA_CALL capture_write(void* context, const char* data, size_t size) {
    capture* sink = (capture*)context;
    if (size > sizeof sink->data - sink->size) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    sink->writes += 1;
    return PRAENORA_STATUS_OK;
}

static int capture_equals(const capture* sink, const char* expected) {
    return sink->size == strlen(expected) && memcmp(sink->data, expected, sink->size) == 0;
}

/* A host API whose callbacks record what the module asked for. */
typedef struct host_record {
    int32_t last_level;
    size_t logged_bytes;
    int32_t cancel;
} host_record;

static void PRAENORA_CALL host_log(void* context, int32_t level, const char* msg, size_t size) {
    host_record* record = (host_record*)context;
    (void)msg;
    record->last_level = level;
    record->logged_bytes += size;
}

static int32_t PRAENORA_CALL host_log_enabled(void* context, int32_t level) {
    (void)context;
    return level >= PRAENORA_LOG_INFO;
}

static int32_t PRAENORA_CALL host_cancel_requested(void* context) {
    return ((host_record*)context)->cancel;
}

static int32_t PRAENORA_CALL gate_is_open(void* context) { return *(const int32_t*)context; }

static int32_t gate_state = 1;
static const praenora_test_gate_v0 gate = {
    {0u, 0u, (uint32_t)sizeof(praenora_test_gate_v0), 0u}, &gate_state, gate_is_open};

/* Serves the test.gate service, major 0, and nothing else. */
static const void* PRAENORA_CALL host_get_service(void* context, const char* name, size_t name_size,
                                                  uint32_t service_abi_major) {
    static const char gate_name[] = PRAENORA_TEST_GATE_SERVICE;
    (void)context;
    if (service_abi_major == 0u && name_size == sizeof gate_name - 1 &&
        memcmp(name, gate_name, name_size) == 0) {
        return &gate;
    }
    return NULL;
}

/* A module written against the headers in plain C. */
struct praenora_instance {
    const praenora_host_api* host;
    int invocations;
};

static struct praenora_instance the_instance;

static praenora_status PRAENORA_CALL c_describe(praenora_sink* out_json) {
    static const char text[] = "{\"name\":\"c99-check\"}";
    const size_t split = 8;
    praenora_status status = out_json->write(out_json->context, text, split);
    if (status != PRAENORA_STATUS_OK) {
        return status;
    }
    return out_json->write(out_json->context, text + split, sizeof text - 1 - split);
}

static praenora_status PRAENORA_CALL c_create(const praenora_host_api* host,
                                              praenora_bytes create_json,
                                              praenora_instance** out_instance,
                                              praenora_sink* error_json) {
    (void)create_json;
    (void)error_json;
    the_instance.host = host;
    the_instance.invocations = 0;
    *out_instance = &the_instance;
    host->log(host->context, PRAENORA_LOG_INFO, "created", 7);
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL c_initialize(praenora_instance* instance,
                                                  praenora_bytes initialize_json,
                                                  praenora_sink* error_json) {
    (void)instance;
    (void)initialize_json;
    (void)error_json;
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL c_invoke(praenora_instance* instance,
                                              praenora_bytes invocation_json,
                                              praenora_sink* result_json,
                                              praenora_sink* error_json) {
    if (instance->host->cancel_requested(instance->host->context) != 0) {
        static const char error[] = "{\"code\":7,\"message\":\"cancelled\",\"details\":{}}";
        error_json->write(error_json->context, error, sizeof error - 1);
        return PRAENORA_STATUS_CANCELLED;
    }
    instance->invocations += 1;
    /* Echo the borrowed input: it is length-delimited, not NUL-terminated. */
    return result_json->write(result_json->context, invocation_json.data, invocation_json.size);
}

static praenora_status PRAENORA_CALL c_finalize(praenora_instance* instance,
                                                praenora_sink* error_json) {
    (void)instance;
    (void)error_json;
    return PRAENORA_STATUS_OK;
}

static void PRAENORA_CALL c_destroy(praenora_instance* instance) { instance->host = NULL; }

static const praenora_module_api_v0 c_table = {
    .header = {.abi_major = PRAENORA_ABI_MAJOR,
               .abi_minor = PRAENORA_ABI_MINOR,
               .struct_size = (uint32_t)sizeof(praenora_module_api_v0),
               .reserved = 0u},
    .describe = c_describe,
    .create = c_create,
    .initialize = c_initialize,
    .invoke = c_invoke,
    .finalize = c_finalize,
    .destroy = c_destroy,
    .diagnostics = NULL,
    .self_test = NULL,
};

static const praenora_api_header* PRAENORA_CALL c_get_api(uint32_t abi_major) {
    return abi_major == 0u ? &c_table.header : NULL;
}

int praenora_sdk_c99_check(void) {
    const praenora_module_get_api_fn get_api = c_get_api;
    const praenora_api_header* header = get_api(0u);
    const praenora_module_api_v0* api = (const praenora_module_api_v0*)header;
    host_record record = {-1, 0, 0};
    praenora_host_api host;
    praenora_instance* instance = NULL;
    capture out = {{0}, 0, 0};
    capture error = {{0}, 0, 0};
    praenora_sink out_sink;
    praenora_sink error_sink;
    praenora_bytes input;
    static const char input_text[] = "{\"tick\":0}trailing bytes are not part of the input";

    if (strcmp(PRAENORA_MODULE_ENTRY_POINT, "praenora_module_get_api") != 0) {
        return 1;
    }
    if (strcmp(praenora_status_name(PRAENORA_STATUS_OK), "OK") != 0 ||
        strcmp(praenora_status_name(PRAENORA_STATUS_DATA_INVALID), "DATA_INVALID") != 0 ||
        strcmp(praenora_status_name(PRAENORA_STATUS_MODULE_BASE), "UNKNOWN") != 0) {
        return 2;
    }
    if (strlen(PRAENORA_SDK_VERSION_STRING) == 0 || strlen(PRAENORA_PMP_VERSION_STRING) == 0) {
        return 3;
    }
    if (header == NULL || get_api(7u) != NULL || header->abi_major != 0u ||
        header->reserved != 0u || header->struct_size < PRAENORA_MODULE_API_V0_BASE_SIZE) {
        return 4;
    }

    out_sink.context = &out;
    out_sink.write = capture_write;
    error_sink.context = &error;
    error_sink.write = capture_write;
    if (api->describe(&out_sink) != PRAENORA_STATUS_OK || out.writes != 2 ||
        !capture_equals(&out, "{\"name\":\"c99-check\"}")) {
        return 5;
    }

    memset(&host, 0, sizeof host);
    host.struct_size = (uint32_t)sizeof host;
    host.abi_major = PRAENORA_ABI_MAJOR;
    host.abi_minor = PRAENORA_ABI_MINOR;
    host.context = &record;
    host.log = host_log;
    host.log_enabled = host_log_enabled;
    host.cancel_requested = host_cancel_requested;
    host.get_service = host_get_service;
    input.data = input_text;
    input.size = 0;
    if (api->create(&host, input, &instance, &error_sink) != PRAENORA_STATUS_OK ||
        instance == NULL || record.last_level != PRAENORA_LOG_INFO || record.logged_bytes != 7 ||
        host.log_enabled(host.context, PRAENORA_LOG_DEBUG) != 0) {
        return 6;
    }
    {
        const praenora_test_gate_v0* found =
            (const praenora_test_gate_v0*)host.get_service(host.context, "test.gate", 9, 0u);
        if (found == NULL || found->header.abi_major != 0u || found->is_open(found->context) != 1 ||
            host.get_service(host.context, "test.gate", 9, 1u) != NULL) {
            return 12;
        }
    }
    if (api->initialize(instance, input, &error_sink) != PRAENORA_STATUS_OK) {
        return 7;
    }

    out.size = 0;
    out.writes = 0;
    input.size = 10; /* only {"tick":0} */
    if (api->invoke(instance, input, &out_sink, &error_sink) != PRAENORA_STATUS_OK ||
        !capture_equals(&out, "{\"tick\":0}") || instance->invocations != 1) {
        return 8;
    }
    record.cancel = 1;
    if (api->invoke(instance, input, &out_sink, &error_sink) != PRAENORA_STATUS_CANCELLED ||
        error.size == 0 || instance->invocations != 1) {
        return 9;
    }
    if (api->finalize(instance, &error_sink) != PRAENORA_STATUS_OK) {
        return 10;
    }
    api->destroy(instance);
    if (the_instance.host != NULL || api->diagnostics != NULL || api->self_test != NULL) {
        return 11;
    }
    return 0;
}

void praenora_sdk_c99_layout(size_t* out) {
    out[0] = sizeof(praenora_api_header);
    out[1] = sizeof(praenora_bytes);
    out[2] = sizeof(praenora_sink);
    out[3] = sizeof(praenora_host_api);
    out[4] = offsetof(praenora_host_api, context);
    out[5] = offsetof(praenora_host_api, get_service);
    out[6] = sizeof(praenora_module_api_v0);
    out[7] = offsetof(praenora_module_api_v0, describe);
    out[8] = offsetof(praenora_module_api_v0, destroy);
    out[9] = offsetof(praenora_module_api_v0, self_test);
    out[10] = PRAENORA_MODULE_API_V0_BASE_SIZE;
    out[11] = PRAENORA_HOST_API_V0_BASE_SIZE;
}
