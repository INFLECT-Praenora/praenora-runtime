/* The test-only runtime service "test.gate" (major version 0).
 *
 * Tests register it with the host and modules look it up through praenora_host_api.get_service.
 * fx-hang blocks in invoke until is_open reports nonzero, which lets lifecycle tests release a
 * module that ignores cancellation. Like every service table, it starts with praenora_api_header.
 * C99 and C++. */
#ifndef PRAENORA_TESTS_SUPPORT_TEST_GATE_H
#define PRAENORA_TESTS_SUPPORT_TEST_GATE_H

#include <praenora/sdk/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRAENORA_TEST_GATE_SERVICE "test.gate"

typedef struct praenora_test_gate_v0 {
    praenora_api_header header;
    void* context;
    /* Nonzero once the gate is open. Thread-safe. */
    /* clang-format off */
    int32_t (PRAENORA_CALL *is_open)(void* context);
    /* clang-format on */
} praenora_test_gate_v0;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_TESTS_SUPPORT_TEST_GATE_H */
