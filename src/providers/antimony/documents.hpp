// The antimony provider's documents and failures: what convert() reads (the options, the source's
// text), what it writes (canonical JSON, valid UTF-8), the failures that become error documents,
// and the calling instance's host API (docs/specs/model-conversion-service.md). No engine type.
#ifndef PRAENORA_PROVIDERS_ANTIMONY_DOCUMENTS_HPP
#define PRAENORA_PROVIDERS_ANTIMONY_DOCUMENTS_HPP

#include <praenora/sdk/model_conversion.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::antimony_provider {

using json = nlohmann::json;

constexpr const char* kService = PRAENORA_MODEL_CONVERSION_SERVICE;
constexpr const char* kAntimony = "antimony";
constexpr const char* kSbml = "sbml";

// A failed call: its status, message and details (an object, to which "service" is added).
struct Failure {
    praenora_status code;
    std::string message;
    json details;
};

// Throws a Failure; the message is prefixed with the service's name.
[[noreturn]] void fail(praenora_status code, const std::string& message,
                       json details = json::object());

// The source breaks the format rule `rule`: DATA_INVALID with details.rule; `why` says what it is.
[[noreturn]] void refuse(const std::string& rule, const std::string& why,
                         json details = json::object());

// The engine cannot read or translate the source: DATA_INVALID with details.engine_error
// {"type": `type`, "message": `message`}, and details.line and details.column where they are not 0.
[[noreturn]] void engine_failure(const std::string& type, const std::string& what,
                                 const std::string& message, std::size_t line = 0,
                                 std::size_t column = 0);

// Canonical JSON (compact, keys sorted), with any text that is not valid UTF-8 replaced by U+FFFD.
std::string dump(const json& document);

inline bool usable(const praenora_sink* sink) noexcept {
    return sink != nullptr && sink->write != nullptr;
}

inline bool readable(praenora_bytes bytes) noexcept {
    return bytes.data != nullptr || bytes.size == 0;
}

inline std::string_view view(praenora_bytes bytes) noexcept {
    return bytes.size == 0 ? std::string_view() : std::string_view(bytes.data, bytes.size);
}

// Writes `text` to `sink`; a refused write fails the call with the sink's status.
void write_to(praenora_sink* sink, const std::string& text, const char* what);

// The error document of a failure, written when the sink takes it.
void write_error(praenora_sink* error_json, praenora_status code, const std::string& message,
                 json details = json::object()) noexcept;

// Runs `body`, turning what it throws into a status and an error document.
template <class Body> praenora_status guarded(praenora_sink* error_json, Body&& body) noexcept {
    try {
        body();
        return PRAENORA_STATUS_OK;
    } catch (const Failure& failure) {
        write_error(error_json, failure.code, failure.message, failure.details);
        return failure.code;
    } catch (const std::bad_alloc&) {
        write_error(error_json, PRAENORA_STATUS_RESOURCE_EXHAUSTED,
                    std::string(kService) + ": out of memory");
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    std::string(kService) + ": " + error.what());
        return PRAENORA_STATUS_INTERNAL;
    } catch (...) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    std::string(kService) + ": an unknown exception");
        return PRAENORA_STATUS_INTERNAL;
    }
}

// Logs `message` at `level` through the calling instance's host, when it can and wants it.
void log_to(const praenora_host_api* host, std::int32_t level, const std::string& message);

// The conversions, sorted by from, then to: antimony to sbml and sbml to antimony.
const std::vector<std::pair<std::string, std::string>>& conversions();

// The options of convert(): {"from", "to", "module"?}.
struct Options {
    std::string from, to;
    std::optional<std::string> module;
};

// Reads and checks the options, reporting their problems in the service's order.
Options read_options(praenora_bytes bytes);

// Fails unless the source is valid UTF-8 without a NUL byte; details.offset is where it is not.
void check_text(std::string_view text);

} // namespace praenora::antimony_provider

#endif // PRAENORA_PROVIDERS_ANTIMONY_DOCUMENTS_HPP
