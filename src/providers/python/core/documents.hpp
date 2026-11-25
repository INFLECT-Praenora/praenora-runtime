// What crosses the Python core's interface: the failures that become error documents, the size
// limit of documents, the sinks they are written to in chunks, and the conversions of text,
// documents and exceptions out of Python (docs/specs/python-service.md).
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_DOCUMENTS_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_DOCUMENTS_HPP

#include "python_include.hpp"

#include <praenora/sdk/python.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <new>
#include <string>
#include <string_view>

namespace praenora::detail::python {

using json = nlohmann::json;

// The size limit of every document (16 MiB).
constexpr std::size_t kMaxDocumentSize = PRAENORA_PYTHON_MAX_DOCUMENT_SIZE;
// The largest write to a sink.
constexpr std::size_t kSinkChunkSize = std::size_t{1} << 20;

// A failed call: its status, message and details (an object). Thrown inside the core only.
struct Failure {
    praenora_status code;
    std::string message;
    json details = json::object();
};

[[noreturn]] void fail(praenora_status code, std::string message, json details = json::object());

inline bool usable(const praenora_sink* sink) noexcept {
    return sink != nullptr && sink->write != nullptr;
}

inline bool readable(praenora_bytes bytes) noexcept {
    return bytes.data != nullptr || bytes.size == 0;
}

inline std::string_view view(praenora_bytes bytes) noexcept {
    return bytes.size == 0 ? std::string_view() : std::string_view(bytes.data, bytes.size);
}

// RESOURCE_EXHAUSTED when `size` is over the limit; `what` says which document, as in "the
// result of invoke".
void check_size(std::size_t size, const std::string& what);

// Writes `bytes` to `sink` in chunks of at most kSinkChunkSize. A refused write throws a Failure
// with the status the sink returned.
void write_chunks(praenora_sink* sink, std::string_view bytes, const std::string& what);

// The error document of a failure: canonical JSON {"code", "details", "message"}, with text that
// is not valid UTF-8 replaced by U+FFFD.
std::string error_document(const Failure& failure);

// Writes the error document when the sink takes it; never throws.
void write_error(praenora_sink* error_json, const Failure& failure) noexcept;

// Runs `body`, turning what it throws into a status and an error document. No exception leaves.
template <class Body> praenora_status guarded(praenora_sink* error_json, Body&& body) noexcept {
    try {
        body();
        return PRAENORA_STATUS_OK;
    } catch (const Failure& failure) {
        write_error(error_json, failure);
        return failure.code;
    } catch (const std::bad_alloc&) {
        write_error(error_json, Failure{PRAENORA_STATUS_RESOURCE_EXHAUSTED, "out of memory"});
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        write_error(error_json, Failure{PRAENORA_STATUS_INTERNAL, error.what()});
        return PRAENORA_STATUS_INTERNAL;
    } catch (...) {
        write_error(error_json, Failure{PRAENORA_STATUS_INTERNAL, "an unknown exception"});
        return PRAENORA_STATUS_INTERNAL;
    }
}

// ---------------------------------------------------------------------------------------------
// Out of Python. Each needs the interpreter's lock, and the core started.

// A str as UTF-8: a lone surrogate, which UTF-8 cannot hold, becomes U+FFFD.
std::string utf8_of(PyObject* text);

// str(value) as UTF-8, or a placeholder when str() fails.
std::string text_of(PyObject* value);

// A UTF-8 str of `text`; INVALID_ARGUMENT naming `what` when it is not valid UTF-8.
Ref text_object(std::string_view text, const std::string& what);

// The document in `bytes` as Python values (None for no bytes), after the size limit;
// INVALID_ARGUMENT naming `what` when it is not JSON.
Ref load_document(std::string_view bytes, const std::string& what);

// `value` as a compact JSON document, within the size limit; INVALID_ARGUMENT naming `what` when
// it holds NaN, an infinity or a value JSON cannot hold.
std::string dump_document(PyObject* value, const std::string& what);

// The failure of the exception that is being raised, which it takes and clears:
// praenora.sdk.Cancelled CANCELLED, praenora.sdk.Error its code, message and details, MemoryError
// RESOURCE_EXHAUSTED, any other INTERNAL with "<Type>: <message>" and details.traceback.
Failure raised_failure();

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_DOCUMENTS_HPP
