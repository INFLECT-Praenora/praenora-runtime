/* Compiled as C99, with the project's warnings, into praenora_unit_tests. It includes the provider
 * interface header, builds a provider table and a provider host in plain C, drives them, and
 * reports the layout the C compiler sees so that ProviderAbi tests can compare it with the C++
 * view. */
#define PRAENORA_PROVIDER_IMPLEMENTATION /* also declares the provider entry point */
#include "provider/provider_abi.h"

#include <stddef.h>
#include <string.h>

int praenora_provider_c99_check(void);
void praenora_provider_c99_layout(size_t* out);

/* Compile-time checks: a false condition gives a negative array size and stops the build. */
#define C99_REQUIRE(name, condition) typedef char c99_require_##name[(condition) ? 1 : -1]

C99_REQUIRE(provider_abi_major_is_zero, PRAENORA_PROVIDER_ABI_MAJOR == 0u);
C99_REQUIRE(provider_header_first, offsetof(praenora_provider_api_v0, header) == 0);
C99_REQUIRE(provider_describe_after_header,
            offsetof(praenora_provider_api_v0, describe) == sizeof(praenora_api_header));
C99_REQUIRE(provider_host_context_after_sizes, offsetof(praenora_provider_host, context) == 8);

/* A sink that appends into a fixed buffer. */
typedef struct capture {
    char data[256];
    size_t size;
} capture;

static praenora_status PRAENORA_CALL capture_write(void* context, const char* data, size_t size) {
    capture* sink = (capture*)context;
    if (size > sizeof sink->data - sink->size) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    return PRAENORA_STATUS_OK;
}

/* A host that records what the provider logged. */
typedef struct host_record {
    int32_t last_level;
    size_t logged_bytes;
} host_record;

static void PRAENORA_CALL host_log(void* context, int32_t level, const char* msg, size_t size) {
    host_record* record = (host_record*)context;
    (void)msg;
    record->last_level = level;
    record->logged_bytes += size;
}

static int32_t PRAENORA_CALL host_log_enabled(void* context, int32_t level) {
    (void)context;
    return level >= PRAENORA_LOG_INFO;
}

/* A provider written against the header in plain C, offering one service. */
typedef struct answer_service {
    praenora_api_header header;
    int32_t answer;
} answer_service;

static const answer_service the_service = {{0u, 0u, (uint32_t)sizeof(answer_service), 0u}, 42};
static const praenora_provider_host* the_host = NULL;

static praenora_status PRAENORA_CALL c_describe(praenora_sink* out_json) {
    static const char text[] = "{\"name\":\"c99-check\",\"version\":\"0.1.0\",\"services\":"
                               "[{\"name\":\"test.answer\",\"major\":0,\"minor\":0}],"
                               "\"engine\":{}}";
    return out_json->write(out_json->context, text, sizeof text - 1);
}

static praenora_status PRAENORA_CALL c_start(const praenora_provider_host* host,
                                             praenora_sink* error_json) {
    (void)error_json;
    the_host = host;
    host->log(host->context, PRAENORA_LOG_INFO, "started", 7);
    return PRAENORA_STATUS_OK;
}

static const void* PRAENORA_CALL c_get_service(const char* name, size_t name_size,
                                               uint32_t major) {
    if (the_host == NULL || major != 0u || name_size != 11 ||
        memcmp(name, "test.answer", 11) != 0) {
        return NULL;
    }
    return &the_service;
}

static const praenora_provider_api_v0 c_table = {
    .header = {.abi_major = PRAENORA_PROVIDER_ABI_MAJOR,
               .abi_minor = PRAENORA_PROVIDER_ABI_MINOR,
               .struct_size = (uint32_t)sizeof(praenora_provider_api_v0),
               .reserved = 0u},
    .describe = c_describe,
    .start = c_start,
    .get_service = c_get_service,
};

/* The entry point a provider library exports; declared by the header for provider code. */
const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) { return &c_table; }

int praenora_provider_c99_check(void) {
    const praenora_provider_get_api_fn get_api = praenora_provider_get_api;
    const praenora_provider_api_v0* api = get_api();
    host_record record = {-1, 0};
    praenora_provider_host host;
    capture out = {{0}, 0};
    capture error = {{0}, 0};
    praenora_sink out_sink;
    praenora_sink error_sink;
    const answer_service* service;

    the_host = NULL; /* each check starts from a provider that has not started */
    if (strcmp(PRAENORA_PROVIDER_ENTRY_POINT, "praenora_provider_get_api") != 0) {
        return 1;
    }
    if (api == NULL || api->header.abi_major != PRAENORA_PROVIDER_ABI_MAJOR ||
        api->header.reserved != 0u || api->header.struct_size < sizeof(praenora_provider_api_v0)) {
        return 2;
    }

    out_sink.context = &out;
    out_sink.write = capture_write;
    error_sink.context = &error;
    error_sink.write = capture_write;
    if (api->describe(&out_sink) != PRAENORA_STATUS_OK || out.size == 0 || out.data[0] != '{') {
        return 3;
    }

    /* No service before start. */
    if (api->get_service("test.answer", 11, 0u) != NULL) {
        return 4;
    }
    memset(&host, 0, sizeof host);
    host.struct_size = (uint32_t)sizeof host;
    host.reserved = 0u;
    host.context = &record;
    host.log = host_log;
    host.log_enabled = host_log_enabled;
    if (api->start(&host, &error_sink) != PRAENORA_STATUS_OK || error.size != 0 ||
        record.last_level != PRAENORA_LOG_INFO || record.logged_bytes != 7 ||
        host.log_enabled(host.context, PRAENORA_LOG_DEBUG) != 0) {
        return 5;
    }

    service = (const answer_service*)api->get_service("test.answer", 11, 0u);
    if (service == NULL || service->header.abi_major != 0u || service->answer != 42 ||
        api->get_service("test.answer", 11, 1u) != NULL ||
        api->get_service("test.other", 10, 0u) != NULL) {
        return 6;
    }
    return 0;
}

void praenora_provider_c99_layout(size_t* out) {
    out[0] = sizeof(praenora_provider_host);
    out[1] = offsetof(praenora_provider_host, context);
    out[2] = offsetof(praenora_provider_host, log);
    out[3] = offsetof(praenora_provider_host, log_enabled);
    out[4] = sizeof(praenora_provider_api_v0);
    out[5] = offsetof(praenora_provider_api_v0, describe);
    out[6] = offsetof(praenora_provider_api_v0, start);
    out[7] = offsetof(praenora_provider_api_v0, get_service);
}
