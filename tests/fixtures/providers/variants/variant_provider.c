/* Fixture providers built from one source, each chosen by one definition or none. Every one but
 * FIXTURE_REORDERED and those without a definition breaks one rule of the provider interface on
 * purpose:
 *
 *   FIXTURE_NULL_TABLE         the entry point returns NULL
 *   FIXTURE_TABLE_ABI_MAJOR    the table's header reports provider interface major 1
 *   FIXTURE_TABLE_RESERVED     the table's header.reserved is not 0
 *   FIXTURE_TABLE_SHORT        the table's struct_size does not cover the v0 functions
 *   FIXTURE_TABLE_NULL_START   the table's start is NULL
 *   FIXTURE_IDENTITY_MISMATCH  describe() reports version 9.9.9 and another engine
 *   FIXTURE_DESCRIBE_NOT_JSON  describe() writes text that is not JSON
 *   FIXTURE_START_FAILS        start() fails with an error document that counts its calls
 *   FIXTURE_MISSING_SERVICE    get_service() returns NULL for test.absent, which it offers
 *   FIXTURE_REORDERED          correct; describe() lists its services in reverse order
 *   (none)                     correct
 *
 * Otherwise get_service() returns a table that is only a header for every service. A start() that
 * does not fail on purpose refuses to run a second time, so a runtime that started the provider
 * twice in one process would fail to load it. C99. */
#include "provider_fixture.h"

#include <stdio.h>

typedef struct header_only_service {
    praenora_api_header header;
} header_only_service;

static const header_only_service service_table = {
    {0u, 0u, (uint32_t)sizeof(header_only_service), 0u}};

#if defined(FIXTURE_IDENTITY_MISMATCH)
static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) {
    return provider_fixture_write(
        out_json, "{\"name\":\"" PRAENORA_PROVIDER_NAME "\",\"version\":\"9.9.9\","
                  "\"services\":" PRAENORA_PROVIDER_SERVICES ",\"engine\":{\"name\":\"other\"}}");
}
#elif defined(FIXTURE_DESCRIBE_NOT_JSON)
static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) {
    return provider_fixture_write(out_json, "this is not JSON");
}
#elif defined(FIXTURE_REORDERED)
/* The manifest lists test.echo@0.0, then test.other@1.2. */
static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) {
    return provider_fixture_write(
        out_json, "{\"name\":\"" PRAENORA_PROVIDER_NAME "\","
                  "\"version\":\"" PRAENORA_PROVIDER_VERSION "\","
                  "\"services\":[{\"minor\":2,\"major\":1,\"name\":\"test.other\"},"
                  "{\"name\":\"test.echo\",\"major\":0,\"minor\":0}],"
                  "\"engine\":" PRAENORA_PROVIDER_ENGINE "}");
}
#else
#define describe provider_fixture_describe
#endif

static int start_calls = 0;

static praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                           praenora_sink* error_json) {
    start_calls += 1;
#if defined(FIXTURE_START_FAILS)
    {
        char text[128];
        const int length = snprintf(text, sizeof text,
                                    "{\"code\":11,\"message\":\"the engine cannot start\","
                                    "\"details\":{\"calls\":%d}}",
                                    start_calls);
        (void)host;
        if (length > 0 && (size_t)length < sizeof text) {
            provider_fixture_write(error_json, text);
        }
        return PRAENORA_STATUS_FAILED;
    }
#else
    if (start_calls > 1) {
        provider_fixture_write(error_json, "{\"code\":3,\"message\":\"start() was called twice\","
                                           "\"details\":{}}");
        return PRAENORA_STATUS_INVALID_STATE;
    }
    provider_fixture_log(host, PRAENORA_LOG_INFO, PRAENORA_PROVIDER_NAME " started");
    return PRAENORA_STATUS_OK;
#endif
}

static const void* PRAENORA_CALL get_service(const char* name, size_t name_size, uint32_t major) {
    (void)major;
#if defined(FIXTURE_MISSING_SERVICE)
    if (provider_fixture_name_is(name, name_size, "test.absent")) {
        return NULL;
    }
#else
    (void)name;
    (void)name_size;
#endif
    return &service_table;
}

#if defined(FIXTURE_TABLE_ABI_MAJOR)
#define TABLE_ABI_MAJOR 1u
#else
#define TABLE_ABI_MAJOR PRAENORA_PROVIDER_ABI_MAJOR
#endif
#if defined(FIXTURE_TABLE_RESERVED)
#define TABLE_RESERVED 7u
#else
#define TABLE_RESERVED 0u
#endif
#if defined(FIXTURE_TABLE_SHORT)
#define TABLE_SIZE ((uint32_t)offsetof(praenora_provider_api_v0, get_service))
#else
#define TABLE_SIZE ((uint32_t)sizeof(praenora_provider_api_v0))
#endif
#if defined(FIXTURE_TABLE_NULL_START)
#define TABLE_START NULL
#else
#define TABLE_START start
#endif

static const praenora_provider_api_v0 table = {
    {TABLE_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR, TABLE_SIZE, TABLE_RESERVED},
    describe,
    TABLE_START,
    get_service};

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) {
#if defined(FIXTURE_TABLE_NULL_START)
    (void)start; /* not in the table */
#endif
#if defined(FIXTURE_NULL_TABLE)
    (void)table;
    return NULL;
#else
    return &table;
#endif
}
