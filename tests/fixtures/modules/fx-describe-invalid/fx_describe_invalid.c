/* fx-describe-invalid: describe() writes a JSON object that violates the pmp-describe schema: it
 * lacks the required "capabilities". The other functions are those of fx-minimal-c. C99. */
#include "../common/c_fixture.h"

static const char describe_json[] =
    "{\"name\":\"" PRAENORA_MODULE_NAME "\",\"version\":\"" PRAENORA_MODULE_VERSION "\","
    "\"abi\":{\"major\":0,\"minor\":0},"
    "\"sdk_version\":\"" PRAENORA_SDK_VERSION_STRING "\","
    "\"pmp_version\":\"" PRAENORA_PMP_VERSION_STRING "\"}";

static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) {
    return c_fixture_write(out_json, describe_json, sizeof describe_json - 1);
}

static const praenora_module_api_v0 table = {
    .header = {.abi_major = 0u,
               .abi_minor = 0u,
               .struct_size = (uint32_t)sizeof(praenora_module_api_v0),
               .reserved = 0u},
    .describe = describe,
    .create = c_fixture_create,
    .initialize = c_fixture_initialize,
    .invoke = c_fixture_invoke,
    .finalize = c_fixture_finalize,
    .destroy = c_fixture_destroy,
    .diagnostics = NULL,
    .self_test = NULL,
};

PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL
praenora_module_get_api(uint32_t abi_major) {
    return abi_major == 0u ? &table.header : NULL;
}
