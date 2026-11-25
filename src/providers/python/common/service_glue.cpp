// The service glue of the providers that run Python (service_glue.hpp).
#include "service_glue.hpp"

#include "loader.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>

namespace praenora::detail::python {

namespace {

using json = nlohmann::json;

// Text as nlohmann/json writes it: invalid UTF-8 replaced.
std::string dump(const json& document) {
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace

bool usable(const praenora_sink* sink) noexcept {
    return sink != nullptr && sink->write != nullptr;
}

praenora_bytes bytes(std::string_view text) noexcept {
    return praenora_bytes{text.data(), text.size()};
}

void write_error(praenora_sink* error_json, praenora_status code, std::string_view message,
                 std::string_view details_json) noexcept {
    if (!usable(error_json)) {
        return;
    }
    try {
        json details = json::parse(details_json.begin(), details_json.end(), nullptr, false);
        if (!details.is_object()) {
            details = json{{"text", std::string(details_json)}};
        }
        const std::string text =
            dump(json{{"code", code}, {"message", std::string(message)}, {"details", details}});
        static_cast<void>(error_json->write(error_json->context, text.data(), text.size()));
    } catch (...) {
        // Nothing to report it with: the caller's status says what failed.
    }
}

void log_to(const praenora_provider_host* host, std::int32_t level,
            std::string_view message) noexcept {
    if (host == nullptr ||
        host->struct_size <
            offsetof(praenora_provider_host, log_enabled) + sizeof host->log_enabled ||
        host->log == nullptr || host->log_enabled == nullptr ||
        host->log_enabled(host->context, level) == 0) {
        return;
    }
    host->log(host->context, level, message.data(), message.size());
}

praenora_status PRAENORA_CALL TextSink::write(void* context, const char* data,
                                              std::size_t size) noexcept {
    if (data == nullptr && size != 0) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return PRAENORA_STATUS_OK;
    }
    try {
        static_cast<TextSink*>(context)->text_.append(data, size);
        return PRAENORA_STATUS_OK;
    } catch (...) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
}

praenora_status ProviderCore::start(const praenora_provider_host* host,
                                    praenora_sink* error_json) noexcept {
    praenora_status status = PRAENORA_STATUS_INTERNAL;
    const praenora_python_core_v0* core = Loader::start(host, error_json, &status);
    if (core == nullptr) {
        return status == PRAENORA_STATUS_OK ? PRAENORA_STATUS_INTERNAL : status;
    }
    host_.store(host);
    core_.store(core);
    try {
        TextSink description;
        if (core->describe(description.get()) == PRAENORA_STATUS_OK) {
            log_to(host, PRAENORA_LOG_INFO, "the Python core started: " + description.text());
        }
    } catch (...) {
        // The record is not needed for the start to succeed.
    }
    return PRAENORA_STATUS_OK;
}

const praenora_python_core_v0* ProviderCore::core() const noexcept { return core_.load(); }

const praenora_provider_host* ProviderCore::host() const noexcept { return host_.load(); }

PythonProviderObject& PythonProviderObject::start_with_digest(const praenora_provider_host* host,
                                                              std::string_view callable,
                                                              std::string_view sources_sha256,
                                                              praenora_sink* error_json,
                                                              praenora_status* status) noexcept {
    praenora_status ignored = PRAENORA_STATUS_OK;
    if (status == nullptr) {
        status = &ignored;
    }
    // One object for the provider library; its members are trivially destructible, so the end of
    // the process undoes nothing of it while a service function may still run.
    static PythonProviderObject object;
    static std::mutex mutex;
    try {
        const std::lock_guard<std::mutex> lock(mutex);
        *status = object.started() ? PRAENORA_STATUS_OK
                                   : object.create(host, callable, sources_sha256, error_json);
    } catch (const std::bad_alloc&) {
        *status = PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        write_error(error_json, *status,
                    "the provider ran out of memory creating its Python object");
    } catch (const std::exception& error) {
        *status = PRAENORA_STATUS_INTERNAL;
        write_error(error_json, *status,
                    std::string("the provider could not create its Python object: ") +
                        error.what());
    } catch (...) {
        *status = PRAENORA_STATUS_INTERNAL;
        write_error(error_json, *status, "the provider could not create its Python object");
    }
    return object;
}

praenora_status PythonProviderObject::create(const praenora_provider_host* host,
                                             std::string_view callable,
                                             std::string_view sources_sha256,
                                             praenora_sink* error_json) {
    if (sources_sha256.empty()) {
        write_error(error_json, PRAENORA_STATUS_INVALID_STATE,
                    "the provider was built without Python sources (PYTHON_SOURCES of "
                    "praenora_add_python_provider()), so it has no Python object to create");
        return PRAENORA_STATUS_INVALID_STATE;
    }
    const praenora_status started = core_.start(host, error_json);
    if (started != PRAENORA_STATUS_OK) {
        return started;
    }
    const praenora_python_core_v0* core = core_.core();
    const std::string sources =
        (std::filesystem::u8path(Loader::library_directory()) / "python").u8string();
    praenora_python_object* created = nullptr;
    const praenora_status status =
        core->create_object(host, nullptr, bytes(sources), bytes(sources_sha256), bytes(callable),
                            bytes("null"), &created, error_json);
    if (status != PRAENORA_STATUS_OK) {
        return status;
    }
    object_.store(created);
    return PRAENORA_STATUS_OK;
}

bool PythonProviderObject::started() const noexcept { return object_.load() != nullptr; }

const praenora_provider_host* PythonProviderObject::host() const noexcept {
    return started() ? core_.host() : nullptr;
}

praenora_status PythonProviderObject::call(const praenora_host_api* module_host,
                                           std::string_view method, std::string_view args_json,
                                           praenora_sink* result_json,
                                           praenora_sink* error_json) const noexcept {
    praenora_python_object* object = object_.load();
    const praenora_python_core_v0* core = core_.core();
    if (object == nullptr || core == nullptr) {
        write_error(error_json, PRAENORA_STATUS_INVALID_STATE,
                    "the provider's Python object is not created: the provider is not started");
        return usable(error_json) ? PRAENORA_STATUS_INVALID_STATE
                                  : PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return core->call(object, module_host, bytes(method), bytes(args_json), result_json,
                      error_json);
}

} // namespace praenora::detail::python
