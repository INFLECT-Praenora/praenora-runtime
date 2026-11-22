/* What the fixture providers share.
 *
 * The identity comes from the PRAENORA_PROVIDER_NAME, PRAENORA_PROVIDER_VERSION,
 * PRAENORA_PROVIDER_SERVICES and PRAENORA_PROVIDER_ENGINE definitions of praenora_add_provider(),
 * so describe() reports exactly what the generated manifest says. C99 and C++. */
#ifndef PRAENORA_FIXTURES_PROVIDER_FIXTURE_H
#define PRAENORA_FIXTURES_PROVIDER_FIXTURE_H

#include "provider_abi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The describe() document of the provider, as one string literal. */
#define PROVIDER_FIXTURE_DESCRIBE_JSON                                                             \
    "{\"name\":\"" PRAENORA_PROVIDER_NAME "\",\"version\":\"" PRAENORA_PROVIDER_VERSION "\","      \
    "\"services\":" PRAENORA_PROVIDER_SERVICES ",\"engine\":" PRAENORA_PROVIDER_ENGINE "}"

/* Writes the NUL-terminated `text` to `sink`. */
praenora_status provider_fixture_write(praenora_sink* sink, const char* text);

/* Writes PROVIDER_FIXTURE_DESCRIBE_JSON. */
praenora_status PRAENORA_CALL provider_fixture_describe(praenora_sink* out_json);

/* Logs the NUL-terminated `message` through the provider host, when it has a log callback. */
void provider_fixture_log(const praenora_provider_host* host, int32_t level, const char* message);

/* Whether `name` (`size` bytes, not NUL-terminated) equals the NUL-terminated `expected`. */
int provider_fixture_name_is(const char* name, size_t size, const char* expected);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_FIXTURES_PROVIDER_FIXTURE_H */
