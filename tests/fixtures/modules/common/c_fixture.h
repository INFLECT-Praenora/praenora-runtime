/* The ABI v0 functions shared by the C fixture modules.
 *
 * Each C fixture defines its own function table and entry point from these functions.
 * fx-minimal-c is a correct module; fx-create-fail breaks no ABI rule but fails create() on
 * purpose; every other C fixture breaks one ABI or describe() rule on purpose. The identity comes
 * from the PRAENORA_MODULE_NAME, PRAENORA_MODULE_VERSION and PRAENORA_MODULE_CAPABILITIES
 * definitions of praenora_add_module(). The fixtures implement ABI 0.0. C99. */
#ifndef PRAENORA_FIXTURES_C_FIXTURE_H
#define PRAENORA_FIXTURES_C_FIXTURE_H

#include <praenora/sdk/abi.h>
#include <praenora/sdk/version.h>

/* The describe() document of a fixture reporting `version`, as one string literal. */
#define C_FIXTURE_DESCRIBE_JSON(version)                                                           \
    "{\"name\":\"" PRAENORA_MODULE_NAME "\",\"version\":\"" version "\","                          \
    "\"abi\":{\"major\":0,\"minor\":0},"                                                           \
    "\"sdk_version\":\"" PRAENORA_SDK_VERSION_STRING "\","                                         \
    "\"pmp_version\":\"" PRAENORA_PMP_VERSION_STRING "\","                                         \
    "\"capabilities\":" PRAENORA_MODULE_CAPABILITIES "}"

/* Writes `size` bytes of `text` to `sink`. */
praenora_status c_fixture_write(praenora_sink* sink, const char* text, size_t size);

/* Writes C_FIXTURE_DESCRIBE_JSON(PRAENORA_MODULE_VERSION). */
praenora_status PRAENORA_CALL c_fixture_describe(praenora_sink* out_json);

/* Allocates an instance that counts its invocations. */
praenora_status PRAENORA_CALL c_fixture_create(const praenora_host_api* host,
                                               praenora_bytes create_json,
                                               praenora_instance** out_instance,
                                               praenora_sink* error_json);

praenora_status PRAENORA_CALL c_fixture_initialize(praenora_instance* instance,
                                                   praenora_bytes initialize_json,
                                                   praenora_sink* error_json);

/* Writes {"outputs":{"out":{"data":{"module":"<name>","invocations":N}}}}, where N counts this
 * instance's invocations from 1. */
praenora_status PRAENORA_CALL c_fixture_invoke(praenora_instance* instance,
                                               praenora_bytes invocation_json,
                                               praenora_sink* result_json,
                                               praenora_sink* error_json);

praenora_status PRAENORA_CALL c_fixture_finalize(praenora_instance* instance,
                                                 praenora_sink* error_json);

void PRAENORA_CALL c_fixture_destroy(praenora_instance* instance);

#endif /* PRAENORA_FIXTURES_C_FIXTURE_H */
