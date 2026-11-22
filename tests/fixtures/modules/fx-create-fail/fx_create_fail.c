/* fx-create-fail: create() logs "lifecycle:create" through the host and fails with status 11
 * (FAILED), writing an error document. destroy() logs "lifecycle:destroy" through the host that
 * create() was given, even when it is called with NULL: a host must never call destroy() after a
 * failed create(), and lifecycle tests detect it when one does. The other functions are those of
 * fx-minimal-c. C99. */
#include "../common/c_fixture.h"

#include <string.h>

/* The host API of the latest create() call. Calls arrive one at a time, on one thread. */
static const praenora_host_api* last_host = NULL;

static void log_info(const praenora_host_api* host, const char* text) {
    if (host != NULL && host->struct_size >= offsetof(praenora_host_api, log) + sizeof(host->log) &&
        host->log != NULL) {
        host->log(host->context, PRAENORA_LOG_INFO, text, strlen(text));
    }
}

static praenora_status PRAENORA_CALL create(const praenora_host_api* host,
                                            praenora_bytes create_json,
                                            praenora_instance** out_instance,
                                            praenora_sink* error_json) {
    static const char error[] =
        "{\"code\":11,\"message\":\"fx-create-fail: create fails on purpose\",\"details\":{}}";
    (void)create_json;
    last_host = host;
    log_info(host, "lifecycle:create");
    if (out_instance != NULL) {
        *out_instance = NULL;
    }
    if (error_json != NULL && error_json->write != NULL) {
        error_json->write(error_json->context, error, sizeof error - 1);
    }
    return PRAENORA_STATUS_FAILED;
}

static void PRAENORA_CALL destroy(praenora_instance* instance) {
    log_info(last_host, "lifecycle:destroy");
    c_fixture_destroy(instance);
}

static const praenora_module_api_v0 table = {
    .header = {.abi_major = 0u,
               .abi_minor = 0u,
               .struct_size = (uint32_t)sizeof(praenora_module_api_v0),
               .reserved = 0u},
    .describe = c_fixture_describe,
    .create = create,
    .initialize = c_fixture_initialize,
    .invoke = c_fixture_invoke,
    .finalize = c_fixture_finalize,
    .destroy = destroy,
    .diagnostics = NULL,
    .self_test = NULL,
};

PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL
praenora_module_get_api(uint32_t abi_major) {
    return abi_major == 0u ? &table.header : NULL;
}
