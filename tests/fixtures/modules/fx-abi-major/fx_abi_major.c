/* fx-abi-major: asked for ABI major 0, it returns a table whose header reports major 7. Everything
 * else about the table is valid. C99. */
#include "../common/c_fixture.h"

static const praenora_module_api_v0 table = {
    .header = {.abi_major = 7u,
               .abi_minor = 0u,
               .struct_size = (uint32_t)sizeof(praenora_module_api_v0),
               .reserved = 0u},
    .describe = c_fixture_describe,
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
    (void)abi_major;
    return &table.header;
}
