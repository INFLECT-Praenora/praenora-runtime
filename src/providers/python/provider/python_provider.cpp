// The provider python: offers praenora.python@0.0 (include/praenora/sdk/python.h; normative:
// docs/specs/python-service.md), Python objects of the runtime's Python environment created and
// called with JSON documents. Its start() loads and starts the process's Python core
// (common/loader.hpp), and its table forwards every call to the core (core/python_core.h): a
// module's host API given to create_object() becomes the object's host, and call(), which has
// none, leaves the object's own host for the call. The provider links no Python. No exception
// leaves a function of the provider.
#include "provider_abi.h"
#include "python_core.h"
#include "service_glue.hpp"

#include <praenora/sdk/python.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using praenora::detail::python::ProviderCore;
using praenora::detail::python::usable;
using praenora::detail::python::write_error;

// The core, once the provider started.
ProviderCore& provider_core() noexcept {
    static ProviderCore core;
    return core;
}

constexpr const char* kNotStarted = "the provider python is not started";

// ---------------------------------------------------------------------------------------------
// The service's functions.

praenora_status PRAENORA_CALL describe(void*, praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    const praenora_python_core_v0* core = provider_core().core();
    if (core == nullptr) {
        return PRAENORA_STATUS_INVALID_STATE;
    }
    return core->describe(out_json);
}

praenora_status PRAENORA_CALL create_object(void*, const praenora_host_api* host,
                                            praenora_bytes path, praenora_bytes path_sha256,
                                            praenora_bytes name, praenora_bytes args_json,
                                            praenora_python_object** out_object,
                                            praenora_sink* error_json) noexcept {
    const praenora_python_core_v0* core = provider_core().core();
    if (core == nullptr) {
        if (out_object != nullptr) {
            *out_object = nullptr;
        }
        write_error(error_json, PRAENORA_STATUS_INVALID_STATE, kNotStarted);
        return usable(error_json) ? PRAENORA_STATUS_INVALID_STATE
                                  : PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return core->create_object(provider_core().host(), host, path, path_sha256, name, args_json,
                               out_object, error_json);
}

praenora_status PRAENORA_CALL call(praenora_python_object* object, praenora_bytes method,
                                   praenora_bytes args_json, praenora_sink* result_json,
                                   praenora_sink* error_json) noexcept {
    const praenora_python_core_v0* core = provider_core().core();
    if (core == nullptr) {
        write_error(error_json, PRAENORA_STATUS_INVALID_STATE, kNotStarted);
        return usable(error_json) ? PRAENORA_STATUS_INVALID_STATE
                                  : PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return core->call(object, nullptr, method, args_json, result_json, error_json);
}

void PRAENORA_CALL release_object(praenora_python_object* object) noexcept {
    const praenora_python_core_v0* core = provider_core().core();
    if (core != nullptr && object != nullptr) {
        core->release_object(object);
    }
}

const praenora_python_v0 python_table = {
    {PRAENORA_PYTHON_SERVICE_MAJOR, PRAENORA_PYTHON_SERVICE_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_python_v0)), 0u},
    nullptr,
    &describe,
    &create_object,
    &call,
    &release_object,
};

// ---------------------------------------------------------------------------------------------
// The provider.

const char kDescription[] =
    "{\"name\":\"" PRAENORA_PROVIDER_NAME "\",\"version\":\"" PRAENORA_PROVIDER_VERSION
    "\",\"services\":" PRAENORA_PROVIDER_SERVICES ",\"engine\":" PRAENORA_PROVIDER_ENGINE "}";

praenora_status PRAENORA_CALL provider_describe(praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return out_json->write(out_json->context, kDescription, std::strlen(kDescription));
}

praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                    praenora_sink* error_json) noexcept {
    return provider_core().start(host, error_json);
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) noexcept {
    const std::size_t length = std::strlen(PRAENORA_PYTHON_SERVICE);
    if (major == PRAENORA_PYTHON_SERVICE_MAJOR && name != nullptr && name_size == length &&
        std::memcmp(name, PRAENORA_PYTHON_SERVICE, length) == 0) {
        return &python_table;
    }
    return nullptr;
}

const praenora_provider_api_v0 provider_table = {
    {PRAENORA_PROVIDER_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_provider_api_v0)), 0u},
    &provider_describe,
    &start,
    &get_service,
};

} // namespace

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) {
    return &provider_table;
}
