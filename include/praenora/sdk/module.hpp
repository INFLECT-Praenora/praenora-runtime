// Praenora module SDK: an optional, header-only C++ adapter over ABI v0 (abi.h).
//
// A module derives a class from praenora::sdk::Module, gives it a constructor taking
// `const praenora::sdk::CreateContext&`, and expands PRAENORA_SDK_DEFINE_MODULE(ClassName) once,
// at namespace scope, in one source file of the library. The macro defines the exported entry
// point and a static v0 function table. The table's functions catch every exception and turn it
// into a status and an error document {"code": N, "message": "...", "details": {...}}:
//   - praenora::sdk::Error: its code, or INTERNAL for a code outside 1-99 and below 10000;
//   - std::bad_alloc: RESOURCE_EXHAUSTED;
//   - any other std::exception: INTERNAL, with what() as the message;
//   - anything else: INTERNAL.
// When a sink refuses a write, the call stops and returns the sink's status. create() fails with
// INVALID_ARGUMENT when an argument is NULL, and with UNSUPPORTED when the host API belongs to
// another ABI major or is too short to hold its fixed fields. The adapter itself needs none of
// the host callbacks: Host skips any callback the host lacks.
//
// describe() reports the identity in three macros that must be string literals where the module
// macro is expanded; praenora_add_module() defines them for every module:
//   PRAENORA_MODULE_NAME, PRAENORA_MODULE_VERSION, and PRAENORA_MODULE_CAPABILITIES (a JSON array
//   of {"id", "version"} objects).
//
// The adapter uses only the C++17 standard library. It parses no JSON, so modules choose their
// own JSON library. Outputs reach the host's sinks in writes of detail::kSinkChunkSize bytes
// (the last one shorter), so large outputs take several writes.
#ifndef PRAENORA_SDK_MODULE_HPP
#define PRAENORA_SDK_MODULE_HPP

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>
#include <praenora/sdk/version.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(PRAENORA_ABI_MAJOR == 0u, "praenora/sdk/module.hpp implements the ABI v0 table");

namespace praenora::sdk {

// Levels of Host::log; the values are the PRAENORA_LOG_* levels of the ABI.
enum class LogLevel : std::int32_t {
    trace = PRAENORA_LOG_TRACE,
    debug = PRAENORA_LOG_DEBUG,
    info = PRAENORA_LOG_INFO,
    warn = PRAENORA_LOG_WARN,
    error = PRAENORA_LOG_ERROR,
    critical = PRAENORA_LOG_CRITICAL,
};

// The host API of one instance. Every call first checks that the host's struct_size covers the
// callback. When it does not, log() does nothing, log_enabled() and cancel_requested() return
// false, and service() returns NULL. Host is a small value type; copies stay valid until the
// instance is destroyed. All calls are thread-safe.
class Host {
public:
    explicit Host(const praenora_host_api* api) noexcept : api_(api) {}

    // Writes a log message; the host adds the step, module and tick.
    void log(LogLevel level, std::string_view message) const noexcept {
        if (covers(offsetof(praenora_host_api, log), sizeof(api_->log)) && api_->log != nullptr) {
            api_->log(api_->context, static_cast<std::int32_t>(level), message.data(),
                      message.size());
        }
    }

    // Whether messages at `level` are recorded; lets a module skip formatting them.
    bool log_enabled(LogLevel level) const noexcept {
        return covers(offsetof(praenora_host_api, log_enabled), sizeof(api_->log_enabled)) &&
               api_->log_enabled != nullptr &&
               api_->log_enabled(api_->context, static_cast<std::int32_t>(level)) != 0;
    }

    // True once the current call's deadline has passed or a stop was requested during the call;
    // every call starts with it false. A long-running call should then throw
    // Error(PRAENORA_STATUS_CANCELLED, ...) soon.
    bool cancel_requested() const noexcept {
        return covers(offsetof(praenora_host_api, cancel_requested),
                      sizeof(api_->cancel_requested)) &&
               api_->cancel_requested != nullptr && api_->cancel_requested(api_->context) != 0;
    }

    // The function table of the runtime service `name` at major version `major`, or NULL.
    const void* service(std::string_view name, std::uint32_t major) const noexcept {
        if (!covers(offsetof(praenora_host_api, get_service), sizeof(api_->get_service)) ||
            api_->get_service == nullptr) {
            return nullptr;
        }
        return api_->get_service(api_->context, name.data(), name.size(), major);
    }

private:
    bool covers(std::size_t offset, std::size_t size) const noexcept {
        return api_ != nullptr && api_->struct_size >= offset + size;
    }

    const praenora_host_api* api_;
};

// What a module's constructor receives from create(). `host` stays valid until the instance is
// destroyed; `create_json` (pmp_version, step, capability, module, parameters) is borrowed for
// the constructor's duration only.
struct CreateContext {
    const Host& host;
    std::string_view create_json;
};

// What Module::initialize receives: the initialize document (pmp_version, run {fingerprint,
// seed}, time {unit, start, period}), borrowed for the call's duration.
struct InitializeContext {
    std::string_view initialize_json;
};

// Fails a module call with a status: a general code 1-99 or a module-defined code from
// PRAENORA_STATUS_MODULE_BASE up, a message, and details as the text of a JSON object.
class Error : public std::exception {
public:
    Error(praenora_status code, std::string message, std::string details_json = "{}")
        : code_(code), message_(std::move(message)), details_json_(std::move(details_json)) {}

    praenora_status code() const noexcept { return code_; }
    const char* what() const noexcept override { return message_.c_str(); }
    const std::string& details_json() const noexcept { return details_json_; }

private:
    praenora_status code_;
    std::string message_;
    std::string details_json_;
};

// Base class of a module. Each instance is one object, created by create() through the derived
// class's constructor and deleted by destroy(); the host makes one call at a time. Destructors
// must not throw.
class Module {
public:
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    virtual ~Module() = default;

    // Called once, after a successful create.
    virtual void initialize(const InitializeContext&) {}
    // Called once per tick with the invocation document; returns the result document
    // {"outputs": {port: {"data": ...}}, "diagnostics": {...}}.
    virtual std::string invoke(std::string_view invocation_json) = 0;
    // Called once at shutdown, only if initialize succeeded.
    virtual void finalize() {}
    // A JSON object describing the instance, added to a failure's details (features.diagnostics).
    virtual std::string diagnostics() const { return "{}"; }
};

namespace detail {

// Outputs are written to sinks in pieces of this size; only the last piece is shorter.
inline constexpr std::size_t kSinkChunkSize = 4096;

// True for the failure codes a module may return: general codes 1-99 and module-defined codes.
constexpr bool is_module_status(praenora_status code) noexcept {
    return (code >= 1 && code <= 99) || code >= PRAENORA_STATUS_MODULE_BASE;
}

// The length of the well-formed UTF-8 sequence (2-4 bytes) starting at text[i], or 0 if the bytes
// there are not one (Unicode, table 3-7).
inline std::size_t utf8_sequence_length(std::string_view text, std::size_t i) noexcept {
    const auto byte = [&text](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    unsigned char second_low = 0x80;
    unsigned char second_high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead == 0xE0) {
        length = 3;
        second_low = 0xA0;
    } else if (lead == 0xED) {
        length = 3;
        second_high = 0x9F;
    } else if (lead >= 0xE1 && lead <= 0xEF) {
        length = 3;
    } else if (lead == 0xF0) {
        length = 4;
        second_low = 0x90;
    } else if (lead == 0xF4) {
        length = 4;
        second_high = 0x8F;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        length = 4;
    } else {
        return 0;
    }
    if (text.size() - i < length || byte(i + 1) < second_low || byte(i + 1) > second_high) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) {
            return 0;
        }
    }
    return length;
}

// Passes `text`, escaped as the contents of a JSON string (without the quotes), to
// append(const char* data, std::size_t size) in pieces. '"' and '\' are escaped, control
// characters become \b \f \n \r \t or \u00XX, well-formed UTF-8 is passed through, and every byte
// that is not part of well-formed UTF-8 becomes the escape for U+FFFD (the replacement
// character). The escaping itself never allocates.
template <class Append> void json_escape_to(std::string_view text, Append&& append) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::size_t copied = 0; // text[copied, i) is passed through unchanged
    std::size_t i = 0;
    const auto pass_through = [&](std::size_t end) {
        if (end > copied) {
            append(text.data() + copied, end - copied);
        }
    };
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0x80) {
            const std::size_t length = utf8_sequence_length(text, i);
            if (length > 0) {
                i += length;
                continue;
            }
            pass_through(i);
            append("\\ufffd", 6);
        } else if (byte < 0x20 || byte == '"' || byte == '\\') {
            pass_through(i);
            switch (byte) {
            case '"':
                append("\\\"", 2);
                break;
            case '\\':
                append("\\\\", 2);
                break;
            case '\b':
                append("\\b", 2);
                break;
            case '\f':
                append("\\f", 2);
                break;
            case '\n':
                append("\\n", 2);
                break;
            case '\r':
                append("\\r", 2);
                break;
            case '\t':
                append("\\t", 2);
                break;
            default: {
                const char escape[6] = {'\\', 'u', '0', '0', kHex[byte >> 4], kHex[byte & 0x0F]};
                append(escape, sizeof escape);
                break;
            }
            }
        } else {
            ++i;
            continue;
        }
        copied = ++i;
    }
    pass_through(text.size());
}

// `text` escaped as the contents of a JSON string, without the quotes (see json_escape_to).
inline std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    json_escape_to(text,
                   [&escaped](const char* data, std::size_t size) { escaped.append(data, size); });
    return escaped;
}

inline bool usable(const praenora_sink* sink) noexcept {
    return sink != nullptr && sink->write != nullptr;
}

// Writes to a host sink through a fixed buffer, in pieces of kSinkChunkSize bytes. It neither
// allocates nor throws. After the sink refuses a write, everything else is dropped and finish()
// returns the sink's status.
class SinkWriter {
public:
    explicit SinkWriter(praenora_sink* sink) noexcept : sink_(sink) {}
    SinkWriter(const SinkWriter&) = delete;
    SinkWriter& operator=(const SinkWriter&) = delete;

    void append(const char* data, std::size_t size) noexcept {
        while (size > 0 && status_ == PRAENORA_STATUS_OK) {
            if (used_ == 0 && size >= kSinkChunkSize) {
                emit(data, kSinkChunkSize); // a whole piece straight from the caller's bytes
                data += kSinkChunkSize;
                size -= kSinkChunkSize;
                continue;
            }
            // (std::min): <windows.h> without NOMINMAX defines a min macro.
            const std::size_t take = (std::min)(size, kSinkChunkSize - used_);
            std::memcpy(buffer_ + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
            if (used_ == kSinkChunkSize) {
                emit(buffer_, used_);
                used_ = 0;
            }
        }
    }

    void append(std::string_view text) noexcept { append(text.data(), text.size()); }

    void append_escaped(std::string_view text) noexcept {
        json_escape_to(text, [this](const char* data, std::size_t size) { append(data, size); });
    }

    void append_number(std::int64_t value) noexcept {
        char digits[24];
        std::size_t start = sizeof digits;
        std::uint64_t magnitude = value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                                            : static_cast<std::uint64_t>(value);
        do {
            digits[--start] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (value < 0) {
            digits[--start] = '-';
        }
        append(digits + start, sizeof digits - start);
    }

    // Writes what is still buffered; returns OK or the status of the first refused write.
    praenora_status finish() noexcept {
        if (used_ > 0 && status_ == PRAENORA_STATUS_OK) {
            emit(buffer_, used_);
        }
        used_ = 0;
        return status_;
    }

private:
    void emit(const char* data, std::size_t size) noexcept {
        status_ = sink_->write(sink_->context, data, size);
    }

    praenora_sink* sink_;
    praenora_status status_ = PRAENORA_STATUS_OK;
    std::size_t used_ = 0;
    char buffer_[kSinkChunkSize];
};

inline praenora_status write_all(praenora_sink* sink, std::string_view text) noexcept {
    SinkWriter out(sink);
    out.append(text);
    return out.finish();
}

// True if `json` is '{' ... '}' apart from surrounding whitespace. The adapter parses no JSON;
// this only keeps details that are plainly not an object from breaking the error document.
inline bool looks_like_object(std::string_view json) noexcept {
    const auto first = json.find_first_not_of(" \t\r\n");
    const auto last = json.find_last_not_of(" \t\r\n");
    return first != std::string_view::npos && json[first] == '{' && json[last] == '}';
}

// Writes {"code":N,"message":"...","details":{...}} to error_json, when there is a usable sink.
// Details that do not look like a JSON object are reported as {"text":"<details>"}. When
// `rejected_code` is given, the message also says that the module reported that code, which is
// not a module status code. Never allocates or throws.
inline void write_error(praenora_sink* error_json, praenora_status code, std::string_view message,
                        std::string_view details_json = "{}",
                        const praenora_status* rejected_code = nullptr) noexcept {
    if (!usable(error_json)) {
        return;
    }
    SinkWriter out(error_json);
    out.append("{\"code\":");
    out.append_number(code);
    out.append(",\"message\":\"");
    out.append_escaped(message);
    if (rejected_code != nullptr) {
        out.append(" (the module reported status ");
        out.append_number(*rejected_code);
        out.append(", which is not a module status code)");
    }
    out.append("\",\"details\":");
    if (looks_like_object(details_json)) {
        out.append(details_json);
    } else if (details_json.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        out.append("{}");
    } else {
        out.append("{\"text\":\"");
        out.append_escaped(details_json);
        out.append("\"}");
    }
    out.append("}");
    static_cast<void>(out.finish());
}

inline std::string_view message_of(const char* what) noexcept {
    return what != nullptr ? std::string_view(what) : std::string_view();
}

// Runs `body` and turns any exception into a status and an error document (see the top of this
// header).
template <class Body> praenora_status guarded(praenora_sink* error_json, Body&& body) noexcept {
    try {
        body();
        return PRAENORA_STATUS_OK;
    } catch (const Error& error) {
        const praenora_status code = error.code();
        if (is_module_status(code)) {
            write_error(error_json, code, message_of(error.what()), error.details_json());
            return code;
        }
        write_error(error_json, PRAENORA_STATUS_INTERNAL, message_of(error.what()),
                    error.details_json(), &code);
        return PRAENORA_STATUS_INTERNAL;
    } catch (const std::bad_alloc& error) {
        write_error(error_json, PRAENORA_STATUS_RESOURCE_EXHAUSTED, message_of(error.what()));
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL, message_of(error.what()));
        return PRAENORA_STATUS_INTERNAL;
    } catch (...) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    "an exception not derived from std::exception");
        return PRAENORA_STATUS_INTERNAL;
    }
}

inline praenora_status fail(praenora_sink* error_json, praenora_status code,
                            std::string_view message) noexcept {
    write_error(error_json, code, message);
    return code;
}

// The v0 table of module class M, whose identity Identity provides as static functions name(),
// version() and capabilities(). Used through the macros below.
template <class M, class Identity> class ModuleAdapter {
    static_assert(std::is_base_of<Module, M>::value,
                  "a module class must derive from praenora::sdk::Module");
    static_assert(std::is_constructible<M, const CreateContext&>::value,
                  "a module class needs a constructor taking const praenora::sdk::CreateContext&");

public:
    // The table for ABI major 0, or NULL for any other major.
    static const praenora_api_header* get_api(std::uint32_t abi_major) noexcept {
        static constexpr praenora_module_api_v0 table = {
            {PRAENORA_ABI_MAJOR, PRAENORA_ABI_MINOR,
             static_cast<std::uint32_t>(sizeof(praenora_module_api_v0)), 0u},
            &describe,
            &create,
            &initialize,
            &invoke,
            &finalize,
            &destroy,
            &diagnostics,
            nullptr,
        };
        return abi_major == 0u ? &table.header : nullptr;
    }

private:
    // The object behind an instance handle. The module is destroyed before the host wrapper, so
    // a module may use its Host in its destructor.
    struct Instance {
        explicit Instance(const praenora_host_api* api) noexcept : host(api) {}

        Host host;
        std::unique_ptr<Module> module;
    };

    // The host fields every v0 host API has: struct_size, abi_major, abi_minor and context.
    static constexpr std::size_t kHostFixedSize =
        offsetof(praenora_host_api, context) + sizeof(void*);

    static Instance* from(praenora_instance* handle) noexcept {
        return reinterpret_cast<Instance*>(handle);
    }

    static bool readable(praenora_bytes bytes) noexcept {
        return bytes.data != nullptr || bytes.size == 0;
    }

    static std::string_view view(praenora_bytes bytes) noexcept {
        return bytes.size == 0 ? std::string_view() : std::string_view(bytes.data, bytes.size);
    }

    static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) noexcept {
        if (!usable(out_json)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        SinkWriter out(out_json);
        out.append("{\"name\":\"");
        out.append_escaped(message_of(Identity::name()));
        out.append("\",\"version\":\"");
        out.append_escaped(message_of(Identity::version()));
        out.append("\",\"abi\":{\"major\":");
        out.append_number(PRAENORA_ABI_MAJOR);
        out.append(",\"minor\":");
        out.append_number(PRAENORA_ABI_MINOR);
        out.append("},\"sdk_version\":\"" PRAENORA_SDK_VERSION_STRING
                   "\",\"pmp_version\":\"" PRAENORA_PMP_VERSION_STRING "\",\"capabilities\":");
        out.append(message_of(Identity::capabilities()));
        out.append("}");
        return out.finish();
    }

    static praenora_status PRAENORA_CALL create(const praenora_host_api* host,
                                                praenora_bytes create_json,
                                                praenora_instance** out_instance,
                                                praenora_sink* error_json) noexcept {
        if (out_instance == nullptr) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "create: out_instance is NULL");
        }
        *out_instance = nullptr;
        if (host == nullptr) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "create: the host API is NULL");
        }
        if (host->struct_size < kHostFixedSize || host->abi_major != PRAENORA_ABI_MAJOR) {
            return fail(error_json, PRAENORA_STATUS_UNSUPPORTED,
                        "create: the host API is not an ABI v0 host API");
        }
        if (!readable(create_json)) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "create: the create document is NULL");
        }
        Instance* created = nullptr;
        const praenora_status status = guarded(error_json, [&] {
            auto instance = std::make_unique<Instance>(host);
            const CreateContext context{instance->host, view(create_json)};
            instance->module = std::make_unique<M>(context);
            created = instance.release();
        });
        if (status == PRAENORA_STATUS_OK) {
            *out_instance = reinterpret_cast<praenora_instance*>(created);
        }
        return status;
    }

    static praenora_status PRAENORA_CALL initialize(praenora_instance* handle,
                                                    praenora_bytes initialize_json,
                                                    praenora_sink* error_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr || !readable(initialize_json)) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "initialize: the instance or the initialize document is NULL");
        }
        return guarded(error_json, [&] {
            instance->module->initialize(InitializeContext{view(initialize_json)});
        });
    }

    static praenora_status PRAENORA_CALL invoke(praenora_instance* handle,
                                                praenora_bytes invocation_json,
                                                praenora_sink* result_json,
                                                praenora_sink* error_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr || !usable(result_json) || !readable(invocation_json)) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "invoke: the instance, the invocation document or the result sink is NULL");
        }
        std::string result;
        praenora_status status =
            guarded(error_json, [&] { result = instance->module->invoke(view(invocation_json)); });
        if (status != PRAENORA_STATUS_OK) {
            return status;
        }
        status = write_all(result_json, result);
        if (status != PRAENORA_STATUS_OK) {
            write_error(error_json, status, "invoke: the host refused the result document");
        }
        return status;
    }

    static praenora_status PRAENORA_CALL finalize(praenora_instance* handle,
                                                  praenora_sink* error_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "finalize: the instance is NULL");
        }
        return guarded(error_json, [&] { instance->module->finalize(); });
    }

    static void PRAENORA_CALL destroy(praenora_instance* handle) noexcept { delete from(handle); }

    static praenora_status PRAENORA_CALL diagnostics(praenora_instance* handle,
                                                     praenora_sink* out_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr || !usable(out_json)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        std::string text;
        const praenora_status status =
            guarded(nullptr, [&] { text = instance->module->diagnostics(); });
        return status == PRAENORA_STATUS_OK ? write_all(out_json, text) : status;
    }
};

} // namespace detail
} // namespace praenora::sdk

// Defines `getter_fn`, a function with the praenora_module_get_api_fn signature that returns the
// static v0 table of ClassName for ABI major 0 and NULL otherwise. It is not exported, so several
// modules can live in one test executable and be driven in process (praenora/sdk/testing.hpp).
// The identity is taken from PRAENORA_MODULE_NAME, PRAENORA_MODULE_VERSION and
// PRAENORA_MODULE_CAPABILITIES as defined where the macro is expanded. Prefix the expansion with
// `static`, or put it in an unnamed namespace, for internal linkage.
#define PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, getter_fn)                                    \
    const praenora_api_header* PRAENORA_CALL getter_fn(uint32_t praenora_sdk_abi_major) {          \
        struct praenora_sdk_module_identity {                                                      \
            static const char* name() noexcept { return PRAENORA_MODULE_NAME; }                    \
            static const char* version() noexcept { return PRAENORA_MODULE_VERSION; }              \
            static const char* capabilities() noexcept { return PRAENORA_MODULE_CAPABILITIES; }    \
        };                                                                                         \
        return ::praenora::sdk::detail::ModuleAdapter<                                             \
            ClassName, praenora_sdk_module_identity>::get_api(praenora_sdk_abi_major);             \
    }

// Defines the module: a getter as above, with internal linkage, and the library's single exported
// entry point, praenora_module_get_api, which returns its table. Expand it once per library, at
// namespace scope.
#define PRAENORA_SDK_DEFINE_MODULE(ClassName)                                                      \
    namespace {                                                                                    \
    PRAENORA_SDK_DEFINE_MODULE_GETTER(ClassName, praenora_sdk_module_get_api)                      \
    }                                                                                              \
    PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL              \
    praenora_module_get_api(uint32_t abi_major) {                                                  \
        return praenora_sdk_module_get_api(abi_major);                                             \
    }

#endif // PRAENORA_SDK_MODULE_HPP
