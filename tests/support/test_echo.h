/* The test-only runtime service "test.echo" (major version 0), offered by the fixture provider
 * test-echo (tests/fixtures/providers).
 *
 * echo() writes its input back. Like every service table, it starts with praenora_api_header.
 * C99 and C++. */
#ifndef PRAENORA_TESTS_SUPPORT_TEST_ECHO_H
#define PRAENORA_TESTS_SUPPORT_TEST_ECHO_H

#include <praenora/sdk/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRAENORA_TEST_ECHO_SERVICE "test.echo"

typedef struct praenora_test_echo_v0 {
    praenora_api_header header; /* abi_major 0 (the service major), abi_minor 0, struct_size */
    void* context;
    /* clang-format off */
    /* Writes `input` to `output` unchanged and returns the status of the write. When `host` is not
     * NULL, logs one debug message through it ("echoed <n> bytes"). INVALID_ARGUMENT for a NULL
     * output, or input data that is NULL with a size. Thread-safe. */
    praenora_status (PRAENORA_CALL *echo)(void* context, const praenora_host_api* host,
                                          praenora_bytes input, praenora_sink* output);
    /* clang-format on */
} praenora_test_echo_v0;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_TESTS_SUPPORT_TEST_ECHO_H */
