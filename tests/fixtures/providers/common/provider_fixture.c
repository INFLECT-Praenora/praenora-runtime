/* The functions the fixture providers share. C99. */
#include "provider_fixture.h"

#include <string.h>

praenora_status provider_fixture_write(praenora_sink* sink, const char* text) {
    if (sink == NULL || sink->write == NULL) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return sink->write(sink->context, text, strlen(text));
}

praenora_status PRAENORA_CALL provider_fixture_describe(praenora_sink* out_json) {
    return provider_fixture_write(out_json, PROVIDER_FIXTURE_DESCRIBE_JSON);
}

void provider_fixture_log(const praenora_provider_host* host, int32_t level, const char* message) {
    if (host == NULL ||
        host->struct_size < offsetof(praenora_provider_host, log) + sizeof host->log ||
        host->log == NULL) {
        return;
    }
    host->log(host->context, level, message, strlen(message));
}

int provider_fixture_name_is(const char* name, size_t size, const char* expected) {
    return name != NULL && size == strlen(expected) && memcmp(name, expected, size) == 0;
}
