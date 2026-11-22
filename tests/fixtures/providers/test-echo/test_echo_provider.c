/* test-echo: a correct provider that offers the service test.echo@0.0 (tests/support/test_echo.h).
 *
 * start() logs "test-echo started" at info level and refuses to run a second time, so a runtime
 * that started the provider twice in one process would fail to load it. get_service() logs each
 * request at debug level through the provider's own logger. The library also defines a function
 * with default visibility that its export list leaves out of the library's exports. C99. */
#include "provider_fixture.h"
#include "test_echo.h"

#include <stdio.h>

static const praenora_provider_host* the_host = NULL;
static int started = 0;

static praenora_status PRAENORA_CALL echo(void* context, const praenora_host_api* host,
                                          praenora_bytes input, praenora_sink* output) {
    (void)context;
    if (output == NULL || output->write == NULL || (input.data == NULL && input.size != 0)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    if (host != NULL && host->log != NULL) {
        char message[64];
        const int length =
            snprintf(message, sizeof message, "echoed %lu bytes", (unsigned long)input.size);
        if (length > 0 && (size_t)length < sizeof message) {
            host->log(host->context, PRAENORA_LOG_DEBUG, message, (size_t)length);
        }
    }
    return output->write(output->context, input.data, input.size);
}

static const praenora_test_echo_v0 echo_table = {
    {0u, 0u, (uint32_t)sizeof(praenora_test_echo_v0), 0u}, NULL, echo};

static praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                           praenora_sink* error_json) {
    if (started) {
        provider_fixture_write(error_json, "{\"code\":3,\"message\":\"start() was called twice\","
                                          "\"details\":{}}");
        return PRAENORA_STATUS_INVALID_STATE;
    }
    started = 1;
    the_host = host;
    provider_fixture_log(host, PRAENORA_LOG_INFO, "test-echo started");
    return PRAENORA_STATUS_OK;
}

static const void* PRAENORA_CALL get_service(const char* name, size_t name_size, uint32_t major) {
    char message[160];
    const int length = snprintf(message, sizeof message, "service %.*s@%lu requested",
                                (int)(name_size < 100 ? name_size : 100),
                                name != NULL ? name : "", (unsigned long)major);
    if (length > 0 && (size_t)length < sizeof message) {
        provider_fixture_log(the_host, PRAENORA_LOG_DEBUG, message);
    }
    if (major == 0u && provider_fixture_name_is(name, name_size, PRAENORA_TEST_ECHO_SERVICE)) {
        return &echo_table;
    }
    return NULL;
}

static const praenora_provider_api_v0 table = {
    {PRAENORA_PROVIDER_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR,
     (uint32_t)sizeof(praenora_provider_api_v0), 0u},
    provider_fixture_describe,
    start,
    get_service};

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) { return &table; }

/* Default visibility, but not on the export list: the library does not export it. */
PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT int test_echo_unlisted_function(void);
int test_echo_unlisted_function(void) { return 42; }
