/* fx-self-test: a correct C module whose function table also has self_test, which reports one
 * passed sub-check. fx-self-test-fails is built from this file with FX_SELF_TEST_FAILS defined: its
 * self_test reports a failed sub-check "tables" with the message "wrong value". C99. */
#include "../common/c_fixture.h"

#ifdef FX_SELF_TEST_FAILS
static const char report[] =
    "{\"passed\":false,\"checks\":[{\"name\":\"arithmetic\",\"passed\":true,\"message\":\"ok\"},"
    "{\"name\":\"tables\",\"passed\":false,\"message\":\"wrong value\"}]}";
#else
static const char report[] =
    "{\"passed\":true,\"checks\":[{\"name\":\"arithmetic\",\"passed\":true,\"message\":\"ok\"}]}";
#endif

static praenora_status PRAENORA_CALL self_test(const praenora_host_api* host,
                                               praenora_sink* report_json) {
    (void)host;
    return c_fixture_write(report_json, report, sizeof report - 1);
}

static const praenora_module_api_v0 table = {
    .header = {.abi_major = 0u,
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
    .self_test = self_test,
};

PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL
praenora_module_get_api(uint32_t abi_major) {
    return abi_major == 0u ? &table.header : NULL;
}
