// What a provider that runs Python needs besides the loader of the Python core (loader.hpp): its
// start(), which loads and starts the core and keeps it with the provider's host for the service
// functions, the error documents and log records of the provider interface's conventions, and
// small helpers for sinks and bytes. A provider whose typed service table is implemented in Python
// (docs/specs/python-service.md, providers implemented in Python) also gets its Python object
// here: PythonProviderObject creates it at start() and calls its methods with JSON documents. Part
// of the static library praenora_python_provider_support (no Python header, no Python link). A
// provider's typed service table forwards to the core through these; none of it touches CPython.
#ifndef PRAENORA_PROVIDERS_PYTHON_COMMON_SERVICE_GLUE_HPP
#define PRAENORA_PROVIDERS_PYTHON_COMMON_SERVICE_GLUE_HPP

#include "python_core.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace praenora::detail::python {

// Whether `sink` can be written to: it and its write function are not NULL.
bool usable(const praenora_sink* sink) noexcept;

// `text` as length-delimited bytes, borrowed.
praenora_bytes bytes(std::string_view text) noexcept;

// Writes the error document {"code", "message", "details"} to error_json, when it is usable;
// `details_json` is the text of a JSON object ("{}" for none; text that is not one is written as
// {"text": <it>}). Text that is not UTF-8 is written with U+FFFD in its place. Never throws.
void write_error(praenora_sink* error_json, praenora_status code, std::string_view message,
                 std::string_view details_json = "{}") noexcept;

// Writes `message` at the PRAENORA_LOG_* `level` to the provider's own log, when `host` offers
// log and log_enabled and records the level. Never throws.
void log_to(const praenora_provider_host* host, std::int32_t level,
            std::string_view message) noexcept;

// A sink that keeps the bytes written to it: the documents and the text that the core and the
// service functions write. A write of data that is NULL with a size is refused
// (INVALID_ARGUMENT), and one the text cannot take is RESOURCE_EXHAUSTED. Not copyable: its sink
// points at it.
class TextSink {
public:
    TextSink() = default;
    TextSink(const TextSink&) = delete;
    TextSink& operator=(const TextSink&) = delete;

    praenora_sink* get() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept;

    std::string text_;
    praenora_sink sink_{this, &TextSink::write};
};

// The Python core as a provider that runs Python holds it: started in the provider's start(),
// then read by its service functions, from any thread.
class ProviderCore {
public:
    // The provider's start(): Loader::start() with `host`, then a record of what started (the
    // core's describe document, at info) in the provider's log. The status; on failure
    // error_json holds the error document (Loader::start()).
    praenora_status start(const praenora_provider_host* host, praenora_sink* error_json) noexcept;

    // The core's table after a successful start; nullptr before one.
    const praenora_python_core_v0* core() const noexcept;
    // The provider's host of that start; nullptr before one.
    const praenora_provider_host* host() const noexcept;

private:
    std::atomic<const praenora_python_core_v0*> core_{nullptr};
    std::atomic<const praenora_provider_host*> host_{nullptr};
};

// The Python object of a provider whose typed service table is implemented in Python: one for
// the provider library, created by its start() and kept for the rest of the process (a provider is
// never unloaded). The provider's service functions call its methods with call() and turn the JSON
// documents into the types of their table themselves: the glue of a typed table is written in
// C++, per service.
class PythonProviderObject {
public:
    // The provider's start(): ProviderCore::start() with `host` (the loader and the core), then the
    // provider's Python object, created through the core by calling `callable`
    // ("package.module:attribute") with the document null, from the provider's Python sources in
    // <directory of the provider library>/python. The core checks those files against the digest
    // that the build compiled into the provider (PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256 of
    // praenora_add_python_provider(): this function is defined in the provider's own sources only)
    // and refuses them when they differ (DATA_INVALID). The object's records go to the provider's
    // log. *status is the start's status, OK or the failure's, whose error document (the loader's
    // or the core's) is in error_json; a provider built without Python sources fails with
    // INVALID_STATE. Returns the provider's one object: after a success every later call returns
    // it at once, with OK; after a failure a later call tries again. Thread-safe. Never throws.
    // Inline, so that the translation units that do not define it may include this header.
    static inline PythonProviderObject& start(const praenora_provider_host* host,
                                              std::string_view callable, praenora_sink* error_json,
                                              praenora_status* status) noexcept;

    // Whether start() created the object.
    bool started() const noexcept;
    // The provider's host of the start that created it; nullptr before one.
    const praenora_provider_host* host() const noexcept;

    // Calls the object's `method` with the JSON document `args_json` and writes what it returns,
    // as JSON, to result_json, as the service praenora.python@0's call does (its documents, its
    // limits, its error documents). `module_host` is the host API of the module that called the
    // service function, when the function has one (NULL otherwise): the call's records and its
    // cancel flag are that module's; without it they are the provider's. Before a successful
    // start, INVALID_STATE with an error document (INVALID_ARGUMENT when error_json is not
    // usable). Thread-safe: every module may call at once, and Python code runs one thread at a
    // time. Never throws.
    praenora_status call(const praenora_host_api* module_host, std::string_view method,
                         std::string_view args_json, praenora_sink* result_json,
                         praenora_sink* error_json) const noexcept;

private:
    PythonProviderObject() = default;

    // start() with the digest of the provider's Python sources.
    static PythonProviderObject& start_with_digest(const praenora_provider_host* host,
                                                   std::string_view callable,
                                                   std::string_view sources_sha256,
                                                   praenora_sink* error_json,
                                                   praenora_status* status) noexcept;
    // Starts the core and creates the object; the status.
    praenora_status create(const praenora_provider_host* host, std::string_view callable,
                           std::string_view sources_sha256, praenora_sink* error_json);

    ProviderCore core_;
    std::atomic<praenora_python_object*> object_{nullptr};
};

#ifdef PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256
// Defined where praenora_add_python_provider() compiles the provider's own sources with the
// digest of its Python sources, so that each provider starts its own.
inline PythonProviderObject& PythonProviderObject::start(const praenora_provider_host* host,
                                                         std::string_view callable,
                                                         praenora_sink* error_json,
                                                         praenora_status* status) noexcept {
    return start_with_digest(host, callable, PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256, error_json,
                             status);
}
#endif

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_COMMON_SERVICE_GLUE_HPP
