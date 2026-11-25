// The Python core's table (python_core.h): the checks of each function's arguments, the guards
// that keep every exception inside the core, and the sinks, which are written only after the
// interpreter's lock is released. interpreter.cpp starts the interpreter, objects.cpp creates and
// calls the objects, documents.cpp writes the documents.
//
// The core's build gives it PRAENORA_PYTHON_CORE_BUILD_ID, its build identity.
#include "python_core.h"

#include "documents.hpp"
#include "interpreter.hpp"
#include "objects.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#ifndef PRAENORA_PYTHON_CORE_BUILD_ID
#error "PRAENORA_PYTHON_CORE_BUILD_ID is defined by the core's build"
#endif

namespace {

namespace fs = std::filesystem;
namespace core = praenora::detail::python;
using core::fail;
using core::Failure;
using core::guarded;
using core::readable;
using core::usable;
using core::view;

// The provider host must offer log and log_enabled.
bool provider_usable(const praenora_provider_host* host) noexcept {
    return host != nullptr && host->struct_size >= offsetof(praenora_provider_host, log_enabled) +
                                                       sizeof(host->log_enabled);
}

// A module's host API, when given, must be at least the v0.0 one.
bool module_host_usable(const praenora_host_api* host) noexcept {
    return host == nullptr || host->struct_size >= PRAENORA_HOST_API_V0_BASE_SIZE;
}

void require_started() {
    if (!core::interpreter_started()) {
        fail(PRAENORA_STATUS_INVALID_STATE, "the Python core is not started");
    }
}

praenora_status PRAENORA_CALL start(const praenora_provider_host* provider_host,
                                    praenora_bytes environment_dir, praenora_bytes sdk_dir,
                                    praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (!provider_usable(provider_host)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "start() needs a provider host that offers log and log_enabled");
        }
        if (!readable(environment_dir) || !readable(sdk_dir)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "start() was given a null directory");
        }
        const std::string_view environment = view(environment_dir);
        const std::string_view sdk = view(sdk_dir);
        core::start_interpreter(provider_host, fs::u8path(environment.begin(), environment.end()),
                                fs::u8path(sdk.begin(), sdk.end()));
    });
}

praenora_status PRAENORA_CALL describe(praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    if (!core::interpreter_started()) {
        return PRAENORA_STATUS_INVALID_STATE;
    }
    try {
        core::write_chunks(out_json, core::interpreter_description(), "the description");
        return PRAENORA_STATUS_OK;
    } catch (const Failure& failure) {
        return failure.code;
    } catch (...) {
        return PRAENORA_STATUS_INTERNAL;
    }
}

praenora_status PRAENORA_CALL create_object(const praenora_provider_host* provider_host,
                                            const praenora_host_api* module_host,
                                            praenora_bytes path, praenora_bytes path_sha256,
                                            praenora_bytes name, praenora_bytes args_json,
                                            praenora_python_object** out_object,
                                            praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (out_object == nullptr) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "create_object() needs out_object");
        }
        *out_object = nullptr;
        if (!provider_usable(provider_host) || !module_host_usable(module_host)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "create_object() needs a provider host, and a module host API of ABI v0 when it "
                 "is given one");
        }
        if (!readable(path) || !readable(path_sha256) || !readable(name) || !readable(args_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "create_object() was given null bytes");
        }
        require_started();
        *out_object = core::create_object(provider_host, module_host, view(path), view(path_sha256),
                                          view(name), view(args_json));
    });
}

praenora_status PRAENORA_CALL call(praenora_python_object* object,
                                   const praenora_host_api* module_host, praenora_bytes method,
                                   praenora_bytes args_json, praenora_sink* result_json,
                                   praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (object == nullptr || !usable(result_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "call() needs an object and a result sink");
        }
        if (!module_host_usable(module_host)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "call() needs a module host API of ABI v0");
        }
        if (!readable(method) || !readable(args_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "call() was given null bytes");
        }
        require_started();
        const std::string result =
            core::call_object(*object, module_host, view(method), view(args_json));
        core::write_chunks(result_json, result, "the result of " + std::string(view(method)));
    });
}

void PRAENORA_CALL release_object(praenora_python_object* object) noexcept {
    core::release_object(object);
}

praenora_status PRAENORA_CALL build_id(praenora_sink* out_text) noexcept {
    if (!usable(out_text)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    static constexpr char kBuildId[] = PRAENORA_PYTHON_CORE_BUILD_ID;
    return out_text->write(out_text->context, kBuildId, sizeof kBuildId - 1);
}

const praenora_python_core_v0 kTable = {
    {PRAENORA_PYTHON_CORE_MAJOR, PRAENORA_PYTHON_CORE_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_python_core_v0)), 0u},
    start,
    describe,
    create_object,
    call,
    release_object,
    build_id,
};

} // namespace

const praenora_python_core_v0* PRAENORA_CALL praenora_python_core_get_api(void) { return &kTable; }
