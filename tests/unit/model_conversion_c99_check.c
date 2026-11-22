/* Compiled as C99, with the project's warnings, into praenora_unit_tests. It includes the model
 * conversion service header (praenora/sdk/model_conversion.h), fills a service table in plain C,
 * calls through it, and reports the layout the C compiler sees so that ModelConversionAbi tests
 * can compare it with the C++ view. */
#include <praenora/sdk/model_conversion.h>

#include <stddef.h>
#include <string.h>

int praenora_model_conversion_c99_check(void);
void praenora_model_conversion_c99_layout(size_t* out);

/* Compile-time checks: a false condition gives a negative array size and stops the build. */
#define C99_REQUIRE(name, condition) typedef char c99_require_##name[(condition) ? 1 : -1]

C99_REQUIRE(service_major_is_zero, PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR == 0u);
C99_REQUIRE(service_minor_is_zero, PRAENORA_MODEL_CONVERSION_SERVICE_MINOR == 0u);
C99_REQUIRE(service_name_length, sizeof(PRAENORA_MODEL_CONVERSION_SERVICE) - 1 == 25);
C99_REQUIRE(source_limit, PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE == (size_t)16777216);
C99_REQUIRE(options_limit, PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE == (size_t)1048576);
C99_REQUIRE(table_header_first, offsetof(praenora_model_conversion_v0, header) == 0);
C99_REQUIRE(table_base_size,
            PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE == sizeof(praenora_model_conversion_v0));

/* A table whose convert copies the source to out_text, upper-casing ASCII letters, and writes a
 * fixed report; it refuses an empty source as the service does. */
static int conversions = 0;

static praenora_status write_text(praenora_sink* sink, const char* data, size_t size) {
    if (sink == NULL || sink->write == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return sink->write(sink->context, data, size);
}

static praenora_status PRAENORA_CALL c_describe(void* context, praenora_sink* out_json) {
    static const char text[] = "{\"service\":\"" PRAENORA_MODEL_CONVERSION_SERVICE "\"}";
    (void)context;
    return write_text(out_json, text, sizeof text - 1);
}

static praenora_status PRAENORA_CALL c_convert(void* context, const praenora_host_api* host,
                                               praenora_bytes options_json, praenora_bytes source,
                                               praenora_sink* out_text, praenora_sink* report_json,
                                               praenora_sink* error_json) {
    static const char report[] =
        "{\"from\":\"a\",\"module\":\"main\",\"to\":\"b\",\"warnings\":[]}";
    static const char refusal[] = "{\"code\":12,\"details\":{\"rule\":\"empty\",\"service\":"
                                  "\"praenora.model-conversion\"},\"message\":\"empty\"}";
    char upper[64];
    size_t i;
    praenora_status status;
    (void)context;
    (void)host;
    (void)options_json;
    if (out_text == NULL || report_json == NULL || error_json == NULL ||
        (source.data == NULL && source.size != 0) || source.size > sizeof upper) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    if (source.size == 0) {
        (void)write_text(error_json, refusal, sizeof refusal - 1);
        return PRAENORA_STATUS_DATA_INVALID;
    }
    for (i = 0; i < source.size; ++i) {
        const char c = source.data[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    status = write_text(out_text, upper, source.size);
    if (status == PRAENORA_STATUS_OK) {
        status = write_text(report_json, report, sizeof report - 1);
    }
    if (status == PRAENORA_STATUS_OK) {
        conversions += 1;
    }
    return status;
}

static const praenora_model_conversion_v0 c_table = {
    .header = {.abi_major = PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR,
               .abi_minor = PRAENORA_MODEL_CONVERSION_SERVICE_MINOR,
               .struct_size = (uint32_t)sizeof(praenora_model_conversion_v0),
               .reserved = 0u},
    .context = NULL,
    .describe = c_describe,
    .convert = c_convert,
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

int praenora_model_conversion_c99_check(void) {
    const praenora_model_conversion_v0* table = &c_table;
    const void* as_service = table; /* what get_service returns */
    const praenora_api_header* header = (const praenora_api_header*)as_service;
    capture text = {{0}, 0};
    capture report = {{0}, 0};
    capture error = {{0}, 0};
    praenora_sink text_sink;
    praenora_sink report_sink;
    praenora_sink error_sink;
    praenora_bytes options;
    praenora_bytes source;
    praenora_bytes none;

    conversions = 0;
    if (strcmp(PRAENORA_MODEL_CONVERSION_SERVICE, "praenora.model-conversion") != 0 ||
        strlen(PRAENORA_MODEL_CONVERSION_SERVICE) != 25) {
        return 1;
    }
    if (header->abi_major != PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR || header->reserved != 0u ||
        header->struct_size < PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE) {
        return 2;
    }
    text_sink.context = &text;
    text_sink.write = capture_write;
    report_sink.context = &report;
    report_sink.write = capture_write;
    error_sink.context = &error;
    error_sink.write = capture_write;
    if (table->describe(table->context, &text_sink) != PRAENORA_STATUS_OK ||
        text.size != strlen("{\"service\":\"praenora.model-conversion\"}")) {
        return 3;
    }
    text.size = 0;
    options.data = "{\"from\":\"a\",\"to\":\"b\"}";
    options.size = strlen(options.data);
    source.data = "decay";
    source.size = 5;
    if (table->convert(table->context, NULL, options, source, &text_sink, &report_sink,
                       &error_sink) != PRAENORA_STATUS_OK ||
        text.size != 5 || memcmp(text.data, "DECAY", 5) != 0 || report.size == 0 ||
        error.size != 0 || conversions != 1) {
        return 4;
    }
    none.data = NULL;
    none.size = 0;
    if (table->convert(table->context, NULL, options, none, &text_sink, &report_sink,
                       &error_sink) != PRAENORA_STATUS_DATA_INVALID ||
        error.size == 0 || conversions != 1) {
        return 5;
    }
    if (table->convert(table->context, NULL, options, source, NULL, &report_sink, &error_sink) !=
        PRAENORA_STATUS_INVALID_ARGUMENT) {
        return 6;
    }
    return 0;
}

void praenora_model_conversion_c99_layout(size_t* out) {
    out[0] = sizeof(praenora_model_conversion_v0);
    out[1] = offsetof(praenora_model_conversion_v0, header);
    out[2] = offsetof(praenora_model_conversion_v0, context);
    out[3] = offsetof(praenora_model_conversion_v0, describe);
    out[4] = offsetof(praenora_model_conversion_v0, convert);
    out[5] = PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE;
}
