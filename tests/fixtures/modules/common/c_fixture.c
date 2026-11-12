/* The ABI v0 functions shared by the C fixture modules (see c_fixture.h). C99. */
#include "c_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct praenora_instance {
    unsigned long long invocations;
};

static const char describe_json[] = C_FIXTURE_DESCRIBE_JSON(PRAENORA_MODULE_VERSION);

/* Writes an error document, when there is an error sink, and returns `code`. */
static praenora_status fail(praenora_sink* error_json, praenora_status code, const char* document) {
    if (error_json != NULL && error_json->write != NULL) {
        error_json->write(error_json->context, document, strlen(document));
    }
    return code;
}

praenora_status c_fixture_write(praenora_sink* sink, const char* text, size_t size) {
    if (sink == NULL || sink->write == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return sink->write(sink->context, text, size);
}

praenora_status PRAENORA_CALL c_fixture_describe(praenora_sink* out_json) {
    return c_fixture_write(out_json, describe_json, sizeof describe_json - 1);
}

praenora_status PRAENORA_CALL c_fixture_create(const praenora_host_api* host,
                                               praenora_bytes create_json,
                                               praenora_instance** out_instance,
                                               praenora_sink* error_json) {
    (void)create_json;
    if (host == NULL || out_instance == NULL) {
        return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                    "{\"code\":2,\"message\":\"create: NULL argument\",\"details\":{}}");
    }
    *out_instance = (praenora_instance*)calloc(1, sizeof(praenora_instance));
    if (*out_instance == NULL) {
        return fail(error_json, PRAENORA_STATUS_RESOURCE_EXHAUSTED,
                    "{\"code\":6,\"message\":\"out of memory\",\"details\":{}}");
    }
    return PRAENORA_STATUS_OK;
}

praenora_status PRAENORA_CALL c_fixture_initialize(praenora_instance* instance,
                                                   praenora_bytes initialize_json,
                                                   praenora_sink* error_json) {
    (void)initialize_json;
    if (instance == NULL) {
        return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                    "{\"code\":2,\"message\":\"initialize: NULL instance\",\"details\":{}}");
    }
    return PRAENORA_STATUS_OK;
}

praenora_status PRAENORA_CALL c_fixture_invoke(praenora_instance* instance,
                                               praenora_bytes invocation_json,
                                               praenora_sink* result_json,
                                               praenora_sink* error_json) {
    char result[256];
    int length;
    (void)invocation_json;
    if (instance == NULL) {
        return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                    "{\"code\":2,\"message\":\"invoke: NULL instance\",\"details\":{}}");
    }
    instance->invocations += 1;
    length = snprintf(result, sizeof result,
                      "{\"outputs\":{\"out\":{\"data\":{\"module\":\"%s\",\"invocations\":%llu}}}}",
                      PRAENORA_MODULE_NAME, instance->invocations);
    if (length < 0 || (size_t)length >= sizeof result) {
        return fail(error_json, PRAENORA_STATUS_INTERNAL,
                    "{\"code\":10,\"message\":\"invoke: result too long\",\"details\":{}}");
    }
    return c_fixture_write(result_json, result, (size_t)length);
}

praenora_status PRAENORA_CALL c_fixture_finalize(praenora_instance* instance,
                                                 praenora_sink* error_json) {
    if (instance == NULL) {
        return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                    "{\"code\":2,\"message\":\"finalize: NULL instance\",\"details\":{}}");
    }
    return PRAENORA_STATUS_OK;
}

void PRAENORA_CALL c_fixture_destroy(praenora_instance* instance) { free(instance); }
