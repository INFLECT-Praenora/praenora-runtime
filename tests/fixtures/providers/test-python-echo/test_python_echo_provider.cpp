// test-python-echo: a provider whose typed service table is implemented in Python. It offers the
// test service test.echo@0.0 (tests/support/test_echo.h); its echo forwards to the Python class
// test_python_echo:Echo of its Python sources (python/test_python_echo/__init__.py) through the
// shared glue of the providers that run Python (src/providers/python/common/service_glue.hpp).
//
// start() starts the process's Python core and creates the class's one object
// (PythonProviderObject::start()). echo() sends the input's bytes in hexadecimal,
// {"input": "<hex>"}, which carries any bytes in a JSON document, names the calling module's host
// API when it is given one, and writes the bytes of the hexadecimal text the method returns. A
// failed call returns the Python call's status, its error document going to the provider's log
// (test.echo@0's echo has no error sink). Built with praenora_add_python_provider()
// (tests/fixtures/providers/CMakeLists.txt): it links the provider interface and the support
// library of the providers that run Python only, and no Python. No exception leaves a function of
// the provider.
#include "provider_abi.h"
#include "service_glue.hpp"
#include "test_echo.h"

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using praenora::detail::python::log_to;
using praenora::detail::python::PythonProviderObject;
using praenora::detail::python::TextSink;
using praenora::detail::python::usable;

// The class of the provider's Python object.
constexpr const char* kCallable = "test_python_echo:Echo";

// The provider's Python object, once start() created it.
std::atomic<const PythonProviderObject*> the_object{nullptr};

// The bytes of `data` in lowercase hexadecimal.
std::string to_hex(praenora_bytes data) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(data.size * 2);
    for (std::size_t i = 0; i < data.size; ++i) {
        const auto byte = static_cast<unsigned char>(data.data[i]);
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }
    return text;
}

int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// The bytes that the lowercase hexadecimal `text` spells, into `out`; false for other text.
bool from_hex(std::string_view text, std::string& out) {
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit_value(text[i]);
        const int low = digit_value(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

praenora_status PRAENORA_CALL echo(void*, const praenora_host_api* host, praenora_bytes input,
                                   praenora_sink* output) noexcept {
    if (!usable(output) || (input.data == nullptr && input.size != 0)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    const PythonProviderObject* object = the_object.load();
    if (object == nullptr) {
        return PRAENORA_STATUS_INVALID_STATE;
    }
    try {
        const std::string args = "{\"input\":\"" + to_hex(input) + "\"}";
        TextSink result;
        TextSink error;
        const praenora_status status = object->call(host, "echo", args, result.get(), error.get());
        if (status != PRAENORA_STATUS_OK) {
            log_to(object->host(), PRAENORA_LOG_ERROR, "echo failed in Python: " + error.text());
            return status;
        }
        // The method returns a JSON string of hexadecimal digits, written without white space.
        const std::string& text = result.text();
        std::string echoed;
        if (text.size() < 2 || text.front() != '"' || text.back() != '"' ||
            !from_hex(std::string_view(text).substr(1, text.size() - 2), echoed)) {
            log_to(object->host(), PRAENORA_LOG_ERROR,
                   "echo returned no hexadecimal text in Python: " + text);
            return PRAENORA_STATUS_INTERNAL;
        }
        return output->write(output->context, echoed.data(), echoed.size());
    } catch (...) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED; // a string that could not grow
    }
}

const praenora_test_echo_v0 echo_table = {
    {0u, 0u, static_cast<std::uint32_t>(sizeof(praenora_test_echo_v0)), 0u},
    nullptr,
    &echo,
};

const char kDescription[] =
    "{\"name\":\"" PRAENORA_PROVIDER_NAME "\",\"version\":\"" PRAENORA_PROVIDER_VERSION
    "\",\"services\":" PRAENORA_PROVIDER_SERVICES ",\"engine\":" PRAENORA_PROVIDER_ENGINE "}";

praenora_status PRAENORA_CALL describe(praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return out_json->write(out_json->context, kDescription, std::strlen(kDescription));
}

praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                    praenora_sink* error_json) noexcept {
    praenora_status status = PRAENORA_STATUS_INTERNAL;
    const PythonProviderObject& object =
        PythonProviderObject::start(host, kCallable, error_json, &status);
    if (status == PRAENORA_STATUS_OK) {
        the_object.store(&object);
    }
    return status;
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) noexcept {
    const std::size_t length = std::strlen(PRAENORA_TEST_ECHO_SERVICE);
    if (major == 0u && name != nullptr && name_size == length &&
        std::memcmp(name, PRAENORA_TEST_ECHO_SERVICE, length) == 0) {
        return &echo_table;
    }
    return nullptr;
}

const praenora_provider_api_v0 provider_table = {
    {PRAENORA_PROVIDER_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_provider_api_v0)), 0u},
    &describe,
    &start,
    &get_service,
};

} // namespace

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) {
    return &provider_table;
}
