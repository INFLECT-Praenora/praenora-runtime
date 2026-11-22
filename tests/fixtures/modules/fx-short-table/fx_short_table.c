/* fx-short-table: its table header claims a struct_size of 8 bytes, below the v0 base size, so the
 * host must not read any function pointer from it. C99. */
#include "../common/c_fixture.h"

static const praenora_module_api_v0 table = {
    .header = {.abi_major = 0u, .abi_minor = 0u, .struct_size = 8u, .reserved = 0u},
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
    return abi_major == 0u ? &table.header : NULL;
}
