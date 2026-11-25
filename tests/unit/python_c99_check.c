/* Compiled as C99, with the project's warnings, into praenora_unit_tests. It includes the Python
 * service header (praenora/sdk/python.h), fills a service table in plain C, calls through it, and
 * reports the layout the C compiler sees so that PythonAbi tests can compare it with the C++
 * view. */
#include <praenora/sdk/python.h>

#include <stddef.h>
#include <string.h>

int praenora_python_c99_check(void);
void praenora_python_c99_layout(size_t* out);

/* Compile-time checks: a false condition gives a negative array size and stops the build. */
#define C99_REQUIRE(name, condition) typedef char c99_require_##name[(condition) ? 1 : -1]

C99_REQUIRE(service_major_is_zero, PRAENORA_PYTHON_SERVICE_MAJOR == 0u);
C99_REQUIRE(service_minor_is_zero, PRAENORA_PYTHON_SERVICE_MINOR == 0u);
C99_REQUIRE(service_name_length, sizeof(PRAENORA_PYTHON_SERVICE) - 1 == 15);
C99_REQUIRE(document_limit, PRAENORA_PYTHON_MAX_DOCUMENT_SIZE == (size_t)16777216);
C99_REQUIRE(table_header_first, offsetof(praenora_python_v0, header) == 0);
C99_REQUIRE(table_base_size, PRAENORA_PYTHON_V0_BASE_SIZE == sizeof(praenora_python_v0));

/* An object of the table below: a counter that call() adds to. */
struct praenora_python_object {
    int calls;
};

static struct praenora_python_object the_object = {0};
static int released = 0;

static praenora_status write_text(praenora_sink* sink, const char* data, size_t size) {
    if (sink == NULL || sink->write == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return sink->write(sink->context, data, size);
}

static praenora_status PRAENORA_CALL c_describe(void* context, praenora_sink* out_json) {
    static const char text[] = "{\"python\":{\"implementation\":\"cpython\"}}";
    (void)context;
    return write_text(out_json, text, sizeof text - 1);
}

static praenora_status PRAENORA_CALL c_create_object(void* context, const praenora_host_api* host,
                                                     praenora_bytes path,
                                                     praenora_bytes path_sha256,
                                                     praenora_bytes name, praenora_bytes args_json,
                                                     praenora_python_object** out_object,
                                                     praenora_sink* error_json) {
    static const char refusal[] = "{\"code\":5,\"details\":{},\"message\":\"no name\"}";
    (void)context;
    (void)host;
    (void)path;
    (void)path_sha256;
    (void)args_json;
    if (out_object == NULL || error_json == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    if (name.size == 0) {
        (void)write_text(error_json, refusal, sizeof refusal - 1);
        return PRAENORA_STATUS_UNSUPPORTED;
    }
    the_object.calls = 0;
    *out_object = &the_object;
    return PRAENORA_STATUS_OK;
}

static praenora_status PRAENORA_CALL c_call(praenora_python_object* object, praenora_bytes method,
                                            praenora_bytes args_json, praenora_sink* result_json,
                                            praenora_sink* error_json) {
    (void)method;
    if (object == NULL || result_json == NULL || error_json == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    object->calls += 1;
    return write_text(result_json, args_json.data, args_json.size);
}

static void PRAENORA_CALL c_release_object(praenora_python_object* object) {
    if (object != NULL) {
        released += 1;
    }
}

static const praenora_python_v0 c_table = {
    .header = {.abi_major = PRAENORA_PYTHON_SERVICE_MAJOR,
               .abi_minor = PRAENORA_PYTHON_SERVICE_MINOR,
               .struct_size = (uint32_t)sizeof(praenora_python_v0),
               .reserved = 0u},
    .context = NULL,
    .describe = c_describe,
    .create_object = c_create_object,
    .call = c_call,
    .release_object = c_release_object,
};

/* A sink that appends into a fixed buffer. */
typedef struct capture {
    char data[128];
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

static praenora_bytes text_bytes(const char* text) {
    praenora_bytes bytes;
    bytes.data = text;
    bytes.size = strlen(text);
    return bytes;
}

int praenora_python_c99_check(void) {
    const praenora_python_v0* table = &c_table;
    const void* as_service = table; /* what get_service returns */
    const praenora_api_header* header = (const praenora_api_header*)as_service;
    capture result = {{0}, 0};
    capture error = {{0}, 0};
    praenora_sink result_sink;
    praenora_sink error_sink;
    praenora_python_object* object = NULL;
    praenora_bytes none;

    released = 0;
    if (strcmp(PRAENORA_PYTHON_SERVICE, "praenora.python") != 0 ||
        strlen(PRAENORA_PYTHON_SERVICE) != 15) {
        return 1;
    }
    if (header->abi_major != PRAENORA_PYTHON_SERVICE_MAJOR || header->reserved != 0u ||
        header->struct_size < PRAENORA_PYTHON_V0_BASE_SIZE) {
        return 2;
    }
    result_sink.context = &result;
    result_sink.write = capture_write;
    error_sink.context = &error;
    error_sink.write = capture_write;
    if (table->describe(table->context, &result_sink) != PRAENORA_STATUS_OK || result.size == 0) {
        return 3;
    }
    result.size = 0;
    none.data = NULL;
    none.size = 0;
    if (table->create_object(table->context, NULL, none, none, text_bytes("pkg.mod:Name"),
                             text_bytes("null"), &object, &error_sink) != PRAENORA_STATUS_OK ||
        object == NULL || error.size != 0) {
        return 4;
    }
    if (table->call(object, text_bytes("echo"), text_bytes("[1,2]"), &result_sink, &error_sink) !=
            PRAENORA_STATUS_OK ||
        result.size != 5 || memcmp(result.data, "[1,2]", 5) != 0 || object->calls != 1) {
        return 5;
    }
    table->release_object(object);
    table->release_object(NULL);
    if (released != 1) {
        return 6;
    }
    object = NULL;
    if (table->create_object(table->context, NULL, none, none, none, none, &object, &error_sink) !=
            PRAENORA_STATUS_UNSUPPORTED ||
        object != NULL || error.size == 0) {
        return 7;
    }
    return 0;
}

void praenora_python_c99_layout(size_t* out) {
    out[0] = sizeof(praenora_python_v0);
    out[1] = offsetof(praenora_python_v0, header);
    out[2] = offsetof(praenora_python_v0, context);
    out[3] = offsetof(praenora_python_v0, describe);
    out[4] = offsetof(praenora_python_v0, create_object);
    out[5] = offsetof(praenora_python_v0, call);
    out[6] = offsetof(praenora_python_v0, release_object);
    out[7] = PRAENORA_PYTHON_V0_BASE_SIZE;
}
