/* Fixture providers built from one source, each chosen by one definition or none. Every one but
 * FIXTURE_REORDERED, FIXTURE_NEGATIVE_ZERO and those without a definition breaks one rule of the
 * provider interface on purpose:
 *
 *   FIXTURE_NULL_TABLE         the entry point returns NULL
 *   FIXTURE_TABLE_ABI_MAJOR    the table's header reports provider interface major 1
 *   FIXTURE_TABLE_RESERVED     the table's header.reserved is not 0
 *   FIXTURE_TABLE_SHORT        the table's struct_size does not cover the v0 functions
 *   FIXTURE_TABLE_NULL_START   the table's start is NULL
 *   FIXTURE_IDENTITY_MISMATCH  describe() reports version 9.9.9 and another engine
 *   FIXTURE_DESCRIBE_NOT_JSON  describe() writes text that is not JSON
 *   FIXTURE_START_FAILS        start() fails with an error document that counts its calls
 *   FIXTURE_START_CRASHES      start() ends the process with abort(): only a module host may load
 *                              it, never a test's own process
 *   FIXTURE_ECHO_CRASHES       its test.echo table's echo() ends the process with abort(): at once,
 *                              or, for an input that holds "later", after it returned, from a
 *                              thread of its own, once the file that the environment variable
 *                              PRAENORA_TEST_ECHO_CRASH_AFTER names exists (looked for every
 *                              10 ms, for at most 60 s; without the variable, 500 ms after it
 *                              returned); only a module host may load it
 *   FIXTURE_MISSING_SERVICE    get_service() returns NULL for test.absent, which it offers
 *   FIXTURE_REORDERED          correct; describe() lists its services in reverse order
 *   FIXTURE_NEGATIVE_ZERO      correct; describe() writes its service's major and minor as -0,
 *                              which is the integer 0
 *   FIXTURE_LOG_LEVELS         correct; start() first logs at warn "<name>: info is recorded" or
 *                              "<name>: info is not recorded", as log_enabled() reports info
 *   (none)                     correct
 *
 * Otherwise get_service() returns a table that is only a header for every service. A start() that
 * does not fail on purpose refuses to run a second time, so a runtime that started the provider
 * twice in one process would fail to load it. C99. */
#if defined(FIXTURE_ECHO_CRASHES) && !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* pthread_create(), nanosleep() under strict C99 */
#endif

#include "provider_fixture.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(FIXTURE_ECHO_CRASHES)
#include "test_echo.h"

#include <string.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#endif
#endif

typedef struct header_only_service {
    praenora_api_header header;
} header_only_service;

static const header_only_service service_table = {
    {0u, 0u, (uint32_t)sizeof(header_only_service), 0u}};

#if defined(FIXTURE_ECHO_CRASHES)
/* Whether `input` holds the NUL-terminated `word`. */
static int holds(praenora_bytes input, const char* word) {
    const size_t size = strlen(word);
    size_t i;
    for (i = 0; input.data != NULL && i + size <= input.size; ++i) {
        if (memcmp(input.data + i, word, size) == 0) {
            return 1;
        }
    }
    return 0;
}

#if !defined(_WIN32)
static void* abort_later(void* unused) {
    const char* trigger = getenv("PRAENORA_TEST_ECHO_CRASH_AFTER");
    struct timespec wait;
    int polls;
    (void)unused;
    wait.tv_sec = 0;
    if (trigger == NULL) {
        wait.tv_nsec = 500000000L;
        nanosleep(&wait, NULL);
        abort();
    }
    wait.tv_nsec = 10000000L;
    for (polls = 0; polls < 6000; ++polls) {
        FILE* file = fopen(trigger, "rb");
        if (file != NULL) {
            fclose(file);
            break;
        }
        nanosleep(&wait, NULL);
    }
    abort();
}
#endif

static praenora_status PRAENORA_CALL echo(void* context, const praenora_host_api* host,
                                          praenora_bytes input, praenora_sink* output) {
    (void)context;
    (void)host;
#if !defined(_WIN32)
    if (holds(input, "later") && output != NULL && output->write != NULL) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, abort_later, NULL) == 0) {
            pthread_detach(thread);
            return output->write(output->context, input.data, input.size);
        }
    }
#else
    (void)input;
    (void)output;
    (void)holds;
#endif
    abort();
}

static const praenora_test_echo_v0 echo_table = {
    {0u, 0u, (uint32_t)sizeof(praenora_test_echo_v0), 0u}, NULL, echo};
#endif

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
#elif defined(FIXTURE_NEGATIVE_ZERO)
/* The manifest lists test.echo@0.0. */
static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) {
    return provider_fixture_write(
        out_json, "{\"name\":\"" PRAENORA_PROVIDER_NAME "\","
                  "\"version\":\"" PRAENORA_PROVIDER_VERSION "\","
                  "\"services\":[{\"name\":\"test.echo\",\"major\":-0,\"minor\":-0}],"
                  "\"engine\":" PRAENORA_PROVIDER_ENGINE "}");
}
#else
#define describe provider_fixture_describe
#endif

static int start_calls = 0;

static praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                           praenora_sink* error_json) {
    start_calls += 1;
#if defined(FIXTURE_START_CRASHES)
    (void)host;
    (void)error_json;
    abort();
#elif defined(FIXTURE_START_FAILS)
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
#if defined(FIXTURE_LOG_LEVELS)
    {
        const int info = host != NULL && host->struct_size >= sizeof(praenora_provider_host) &&
                         host->log_enabled != NULL &&
                         host->log_enabled(host->context, PRAENORA_LOG_INFO) != 0;
        provider_fixture_log(host, PRAENORA_LOG_WARN,
                             info ? PRAENORA_PROVIDER_NAME ": info is recorded"
                                  : PRAENORA_PROVIDER_NAME ": info is not recorded");
    }
#endif
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
#elif defined(FIXTURE_ECHO_CRASHES)
    if (provider_fixture_name_is(name, name_size, PRAENORA_TEST_ECHO_SERVICE)) {
        return &echo_table;
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
