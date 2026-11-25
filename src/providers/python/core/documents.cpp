// Failures, error documents and sinks, and the text, documents and exceptions that come out of
// Python (documents.hpp).
#include "documents.hpp"

#include "interpreter.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace praenora::detail::python {

namespace {

// An error document longer than the limit keeps its code and the start of its message.
constexpr std::size_t kTruncatedMessageSize = 4096;

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// The attribute `name` of `object`, or null (the error cleared).
Ref attribute(PyObject* object, const char* name) {
    Ref value(PyObject_GetAttrString(object, name));
    if (!value) {
        PyErr_Clear();
    }
    return value;
}

// "<Type>: <message>", or "<Type>" for an empty message; the module of the type is named but
// for builtins, as Python's own tracebacks write it.
std::string exception_message(PyObject* exception) {
    PyObject* type = reinterpret_cast<PyObject*>(Py_TYPE(exception));
    const Ref qualname = attribute(type, "__qualname__");
    const Ref module = attribute(type, "__module__");
    std::string name = qualname && PyUnicode_Check(qualname.get()) ? utf8_of(qualname.get())
                                                                   : Py_TYPE(exception)->tp_name;
    if (module && PyUnicode_Check(module.get())) {
        const std::string module_name = utf8_of(module.get());
        if (module_name != "builtins" && module_name != "__main__") {
            name = module_name + "." + name;
        }
    }
    const std::string message = text_of(exception);
    return message.empty() ? name : name + ": " + message;
}

// The lines of the exception's traceback, as traceback.format_exception() writes them (its
// cause and context included); empty when they cannot be had.
json traceback_lines(PyObject* exception) {
    json lines = json::array();
    PyObject* format = hooks().format_exception;
    if (format == nullptr) {
        return lines;
    }
    const Ref parts(PyObject_CallOneArg(format, exception));
    if (!parts || !PyList_Check(parts.get())) {
        PyErr_Clear();
        return lines;
    }
    std::string text;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(parts.get()); ++i) {
        PyObject* part = PyList_GET_ITEM(parts.get(), i);
        if (PyUnicode_Check(part)) {
            text += utf8_of(part);
        }
    }
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// A status that a failed call may return: a general code (1 to 99) or a module-defined one.
bool failure_code(long long code) {
    return (code >= 1 && code <= 99) || (code >= PRAENORA_STATUS_MODULE_BASE &&
                                         code <= std::numeric_limits<std::int32_t>::max());
}

// The failure of a praenora.sdk.Error (or Cancelled, whose code is `forced`).
Failure sdk_failure(PyObject* exception, std::optional<praenora_status> forced) {
    Failure failure{PRAENORA_STATUS_INTERNAL, std::string()};
    const Ref message = attribute(exception, "message");
    failure.message = message ? text_of(message.get()) : text_of(exception);
    const Ref details = attribute(exception, "details");
    if (details && details.get() != Py_None) {
        const Ref text(hooks().dump_document != nullptr
                           ? PyObject_CallOneArg(hooks().dump_document, details.get())
                           : nullptr);
        json parsed = text ? json::parse(utf8_of(text.get()), nullptr, false) : json();
        if (!text) {
            PyErr_Clear();
        }
        if (parsed.is_object()) {
            failure.details = std::move(parsed);
        } else {
            failure.details = json{{"invalid_details", text_of(details.get())}};
        }
    }
    if (forced) {
        failure.code = *forced;
        return failure;
    }
    const Ref code = attribute(exception, "code");
    long long value = 0;
    if (code) {
        value = PyLong_AsLongLong(code.get());
        if (value == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            value = 0;
        }
    }
    if (failure_code(value)) {
        failure.code = static_cast<praenora_status>(value);
    } else {
        // Not a status a failure can have (OK, for one): the call fails as an internal error.
        failure.details["raised_code"] = value;
    }
    return failure;
}

} // namespace

void fail(praenora_status code, std::string message, json details) {
    throw Failure{code, std::move(message), std::move(details)};
}

void check_size(std::size_t size, const std::string& what) {
    if (size > kMaxDocumentSize) {
        fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED,
             what + " is " + std::to_string(size) + " bytes, more than the 16 MiB (" +
                 std::to_string(kMaxDocumentSize) + " bytes) that a document may take",
             json{{"size", size}, {"limit", kMaxDocumentSize}});
    }
}

void write_chunks(praenora_sink* sink, std::string_view bytes, const std::string& what) {
    for (std::size_t offset = 0; offset < bytes.size(); offset += kSinkChunkSize) {
        const std::size_t size = std::min(kSinkChunkSize, bytes.size() - offset);
        const praenora_status status = sink->write(sink->context, bytes.data() + offset, size);
        if (status != PRAENORA_STATUS_OK) {
            fail(status, "the sink refused " + what, json{{"status", status}});
        }
    }
}

std::string error_document(const Failure& failure) {
    json document{{"code", failure.code},
                  {"message", failure.message},
                  {"details", failure.details.is_object() ? failure.details : json::object()}};
    std::string text = document.dump(-1, ' ', false, json::error_handler_t::replace);
    if (text.size() > kMaxDocumentSize) {
        document["message"] = failure.message.substr(0, kTruncatedMessageSize);
        document["details"] = json{{"truncated", true}};
        text = document.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    return text;
}

void write_error(praenora_sink* error_json, const Failure& failure) noexcept {
    if (!usable(error_json)) {
        return;
    }
    try {
        const std::string text = error_document(failure);
        for (std::size_t offset = 0; offset < text.size(); offset += kSinkChunkSize) {
            const std::size_t size = std::min(kSinkChunkSize, text.size() - offset);
            if (error_json->write(error_json->context, text.data() + offset, size) !=
                PRAENORA_STATUS_OK) {
                return;
            }
        }
    } catch (...) {
        // Nothing more can be said: the call's status still says it failed.
    }
}

std::string utf8_of(PyObject* text) {
    Py_ssize_t size = 0;
    const char* data = PyUnicode_AsUTF8AndSize(text, &size);
    if (data != nullptr) {
        return std::string(data, static_cast<std::size_t>(size));
    }
    // A lone surrogate, which UTF-8 cannot hold: the text is written code point by code point.
    PyErr_Clear();
    const int kind = PyUnicode_KIND(text);
    const void* points = PyUnicode_DATA(text);
    const Py_ssize_t length = PyUnicode_GET_LENGTH(text);
    std::string out;
    out.reserve(static_cast<std::size_t>(length));
    for (Py_ssize_t i = 0; i < length; ++i) {
        std::uint32_t code = PyUnicode_READ(kind, points, i);
        if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFD;
        }
        append_utf8(out, code);
    }
    return out;
}

std::string text_of(PyObject* value) {
    const Ref text(PyObject_Str(value));
    if (!text) {
        PyErr_Clear();
        return std::string("<a ") + Py_TYPE(value)->tp_name + " that str() cannot write>";
    }
    return utf8_of(text.get());
}

Ref text_object(std::string_view text, const std::string& what) {
    Ref object(PyUnicode_DecodeUTF8(text.data(), static_cast<Py_ssize_t>(text.size()), "strict"));
    if (!object) {
        PyErr_Clear();
        fail(PRAENORA_STATUS_INVALID_ARGUMENT, what + " is not valid UTF-8");
    }
    return object;
}

Ref load_document(std::string_view bytes, const std::string& what) {
    check_size(bytes.size(), what);
    if (bytes.empty()) {
        return Ref::borrowed(Py_None);
    }
    const Ref text = text_object(bytes, what);
    Ref value(PyObject_CallOneArg(hooks().load_document, text.get()));
    if (!value) {
        if (PyErr_ExceptionMatches(PyExc_MemoryError)) {
            throw raised_failure();
        }
        const Failure failure = raised_failure();
        fail(PRAENORA_STATUS_INVALID_ARGUMENT,
             "cannot read " + what + " as a JSON document: " + failure.message);
    }
    return value;
}

std::string dump_document(PyObject* value, const std::string& what) {
    const Ref text(PyObject_CallOneArg(hooks().dump_document, value));
    if (!text) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) || PyErr_ExceptionMatches(PyExc_ValueError) ||
            PyErr_ExceptionMatches(PyExc_RecursionError)) {
            const Failure failure = raised_failure();
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 what + " is not a JSON document: " + failure.message);
        }
        throw raised_failure();
    }
    std::string document = utf8_of(text.get());
    check_size(document.size(), what);
    return document;
}

Failure raised_failure() {
    const Ref exception(PyErr_GetRaisedException());
    if (!exception) {
        return Failure{PRAENORA_STATUS_INTERNAL, "a Python call failed without an exception"};
    }
    const Hooks& known = hooks();
    if (known.sdk_cancelled != nullptr &&
        PyErr_GivenExceptionMatches(exception.get(), known.sdk_cancelled)) {
        return sdk_failure(exception.get(), PRAENORA_STATUS_CANCELLED);
    }
    if (known.sdk_error != nullptr &&
        PyErr_GivenExceptionMatches(exception.get(), known.sdk_error)) {
        return sdk_failure(exception.get(), std::nullopt);
    }
    const bool memory = PyErr_GivenExceptionMatches(exception.get(), PyExc_MemoryError) != 0;
    Failure failure{memory ? PRAENORA_STATUS_RESOURCE_EXHAUSTED : PRAENORA_STATUS_INTERNAL,
                    exception_message(exception.get())};
    json lines = traceback_lines(exception.get());
    if (!lines.empty()) {
        failure.details["traceback"] = std::move(lines);
    }
    return failure;
}

} // namespace praenora::detail::python
