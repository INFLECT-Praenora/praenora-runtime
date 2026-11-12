/* fx-null-table: the entry point exists but returns no table, whatever major is requested. C99. */
#include <praenora/sdk/abi.h>

PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL
praenora_module_get_api(uint32_t abi_major) {
    (void)abi_major;
    return NULL;
}
