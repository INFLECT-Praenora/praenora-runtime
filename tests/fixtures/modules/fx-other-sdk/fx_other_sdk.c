/* fx-other-sdk: describe() reports SDK version 0.0.1, as its manifest does, although it is built
 * with another SDK: the SDK version decides no compatibility, so the module loads. Built a second
 * time as fx-sdk-overflow (FX_SDK_OVERFLOW), whose describe() reports SDK version
 * 0.99999999999999999999.0: the pmp-describe schema's pattern accepts it, but its minor number does
 * not fit in 64 bits, so the host cannot compare it with the manifest. The other functions are
 * those of fx-minimal-c. C99. */
#include "../common/c_fixture.h"

#ifdef FX_SDK_OVERFLOW
#define FX_SDK_VERSION "0.99999999999999999999.0"
#else
#define FX_SDK_VERSION "0.0.1"
#endif

static const char describe_json[] =
    C_FIXTURE_DESCRIBE_JSON_SDK(PRAENORA_MODULE_VERSION, FX_SDK_VERSION);

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
