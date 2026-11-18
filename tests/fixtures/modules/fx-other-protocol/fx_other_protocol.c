/* fx-other-protocol: describe() reports ABI 0.7, as its manifest and its table's header do, and PMP
 * version 0.1.0-rc.1, as its manifest does, although the SDK it is built with has ABI 0.0 and PMP
 * 0.1.0: any ABI minor is accepted and a pre-release of the runtime's PMP version is compatible, so
 * the module loads. Built a second time as fx-newer-pmp (FX_NEWER_PMP), whose describe() reports
 * PMP version 0.2.0 beside the generated manifest's 0.1.0: the manifest is compatible with the
 * runtime, the library is not. Built a third time as fx-pmp-overflow (FX_PMP_OVERFLOW), whose
 * describe() reports PMP version 0.99999999999999999999.0: the pmp-describe schema's pattern
 * accepts it, but its minor number does not fit in 64 bits, so the host cannot compare it with the
 * manifest. Built a fourth time as fx-negative-zero-abi (FX_NEGATIVE_ZERO_ABI), whose describe()
 * writes ABI major and minor as -0, which the pmp-describe schema accepts as the integer 0, and the
 * PMP version of the SDK: it agrees with its generated manifest, so it loads. The other three
 * report ABI 0.0, as their manifests do. The other functions are those of fx-minimal-c. C99. */
#include "../common/c_fixture.h"

#if defined(FX_NEWER_PMP)
#define FX_ABI_MINOR 0
#define FX_PMP_VERSION "0.2.0"
#elif defined(FX_PMP_OVERFLOW)
#define FX_ABI_MINOR 0
#define FX_PMP_VERSION "0.99999999999999999999.0"
#elif defined(FX_NEGATIVE_ZERO_ABI)
#define FX_ABI_MINOR 0
#define FX_PMP_VERSION PRAENORA_PMP_VERSION_STRING
#else
#define FX_ABI_MINOR 7
#define FX_PMP_VERSION "0.1.0-rc.1"
#endif

#define FX_TEXT(value) #value
#define FX_DIGITS(value) FX_TEXT(value)

/* The ABI as describe() writes it. */
#if defined(FX_NEGATIVE_ZERO_ABI)
#define FX_ABI_MAJOR_TEXT "-0"
#define FX_ABI_MINOR_TEXT "-0"
#else
#define FX_ABI_MAJOR_TEXT "0"
#define FX_ABI_MINOR_TEXT FX_DIGITS(FX_ABI_MINOR)
#endif

static const char describe_json[] =
    C_FIXTURE_DESCRIBE_JSON_ABI(PRAENORA_MODULE_VERSION, FX_ABI_MAJOR_TEXT, FX_ABI_MINOR_TEXT,
                                PRAENORA_SDK_VERSION_STRING, FX_PMP_VERSION);

static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) {
    return c_fixture_write(out_json, describe_json, sizeof describe_json - 1);
}

static const praenora_module_api_v0 table = {
    .header = {.abi_major = 0u,
               .abi_minor = FX_ABI_MINOR,
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
