// The error taxonomy: stable codes shared with the module ABI, the exception the C++ API throws,
// and the diagnostics that validation collects.
#ifndef PRAENORA_RUNTIME_ERROR_HPP
#define PRAENORA_RUNTIME_ERROR_HPP

#include <praenora/runtime/export.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace praenora {

// One int32 code space. 0-99 are general codes, equal to the PRAENORA_STATUS_* values of
// <praenora/sdk/status.h> and returned by modules too; 100-999 belong to the runtime. Codes from
// 10000 up are module-defined: they are reported as the cause of a runtime error, never as an
// ErrorCode of their own.
enum class ErrorCode : std::int32_t {
    OK = 0,
    UNKNOWN = 1,
    INVALID_ARGUMENT = 2,
    INVALID_STATE = 3,
    NOT_FOUND = 4,
    UNSUPPORTED = 5,
    RESOURCE_EXHAUSTED = 6,
    CANCELLED = 7,
    TIMEOUT = 8,
    IO_ERROR = 9,
    INTERNAL = 10,
    FAILED = 11,
    DATA_INVALID = 12,
    PARSE_ERROR = 100,
    SCHEMA_VIOLATION = 101,
    FORMAT_VERSION_UNSUPPORTED = 102,
    DOCUMENT_KIND_MISMATCH = 103,
    CONFIG_INVALID = 200,
    WORKFLOW_NOT_FOUND = 201,
    BINDING_INVALID = 202,
    PARAMETERS_INVALID = 203,
    INPUT_BINDING_INVALID = 204,
    MANIFEST_INVALID = 300,
    MODULE_NOT_FOUND = 301,
    MODULE_IDENTITY_MISMATCH = 302,
    LIBRARY_DIGEST_MISMATCH = 303,
    LIBRARY_NOT_FOUND = 304,
    ABI_INCOMPATIBLE = 400,
    PROTOCOL_INCOMPATIBLE = 401,
    CAPABILITY_NOT_IMPLEMENTED = 402,
    PLATFORM_MISMATCH = 403,
    MESSAGE_TYPE_INCOMPATIBLE = 404,
    CATALOG_ENTRY_NOT_FOUND = 405,
    CATALOG_ENTRY_DUPLICATE = 406,
    SERVICE_UNAVAILABLE = 407,
    LIBRARY_LOAD_FAILED = 500,
    ENTRY_POINT_MISSING = 501,
    API_TABLE_INVALID = 502,
    GRAPH_INVALID = 600,
    GRAPH_CYCLE = 601,
    PORT_UNCONNECTED = 602,
    PORT_UNKNOWN = 603,
    PORT_TYPE_MISMATCH = 604,
    STEP_FAILED = 700,
    OUTPUT_INVALID = 701,
    STEP_TIMEOUT = 702,
    MODULE_UNRESPONSIVE = 703,
    INPUT_MISSING = 704,
    RUN_STOPPED = 705,
    TARGET_DATA_INVALID = 800,
    TARGET_DATA_UNAVAILABLE = 801,
};

// The kind of failure a code stands for. The category determines the CLI exit code: usage 2,
// validation 3, compatibility 3, load 4, execution 1, timeout 5, cancelled 130, data 1,
// environment 74, internal 70.
enum class ErrorCategory {
    usage,
    validation,
    compatibility,
    load,
    execution,
    timeout,
    cancelled,
    data,
    environment,
    internal
};

// One row of the taxonomy table. The string views refer to static storage.
struct ErrorCodeInfo {
    ErrorCode code;
    std::string_view name; // stable name, for example "PARSE_ERROR"
    ErrorCategory category;
    std::string_view owner;   // likely owner, for example "user", "module", "environment"
    int exit_code;            // CLI exit code (0 for OK)
    bool retryable;           // retrying the same operation unchanged may succeed
    std::string_view summary; // one-sentence meaning
};

// The table row of `code`. A value outside the table yields the row of UNKNOWN.
PRAENORA_RUNTIME_API const ErrorCodeInfo& error_code_info(ErrorCode code);

// Every row of the table, sorted by code.
PRAENORA_RUNTIME_API const std::vector<ErrorCodeInfo>& all_error_codes();

// The code with the stable name `name` (case-sensitive), if there is one.
PRAENORA_RUNTIME_API std::optional<ErrorCode> error_code_from_name(std::string_view name);

// The category's name as written in ErrorCategory, for example "environment".
PRAENORA_RUNTIME_API std::string to_string(ErrorCategory category);

// Where a problem was found. Unset parts are empty or 0. `pointer` is a JSON pointer into the
// document; line and column are 1-based (columns count bytes).
struct SourceLocation {
    std::string file;
    std::int64_t line = 0, column = 0;
    std::string pointer;
};

#ifdef _MSC_VER
#pragma warning(push)
// The exported class derives from std::runtime_error, which is not exported itself.
#pragma warning(disable : 4251 4275)
#endif

// The exception thrown by the C++ API. what() returns the message.
class PRAENORA_RUNTIME_API Error : public std::runtime_error {
public:
    // `details_json` is a JSON text with code-specific details; it is "{}" when there are none.
    Error(ErrorCode code, std::string message, SourceLocation location = {},
          std::string details_json = "{}");
    // Defined in the library, so its vtable and type_info are emitted there once and exported:
    // `catch (const praenora::Error&)` then works across the shared-library boundary.
    ~Error() override;
    Error(const Error&) = default;
    Error& operator=(const Error&) = default;

    ErrorCode code() const noexcept;
    const SourceLocation& location() const noexcept;
    const std::string& details_json() const noexcept;

    // {"code":N,"name":"…","category":"…","message":"…","location":{…},"details":{…}}.
    // `location` holds only the parts that are set. Details that are not valid JSON appear as a
    // string.
    std::string to_json() const;

private:
    struct Payload;
    ErrorCode code_;
    std::shared_ptr<const Payload> payload_; // shared, so copies cannot throw
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

enum class Severity { error, warning, note };

// One finding of a validation. `step` and `module` are empty unless the finding concerns one.
// `details_json` is the text of a JSON object with more about the finding (for example the
// providers a SERVICE_UNAVAILABLE considered), or empty when it has none.
struct Diagnostic {
    Severity severity = Severity::error;
    ErrorCode code = ErrorCode::UNKNOWN;
    std::string message;
    SourceLocation location;
    std::string step, module;
    std::string details_json;
};

// Every finding of a validation, in the order found.
struct ValidationReport {
    std::vector<Diagnostic> diagnostics;

    // True when no diagnostic has severity error.
    PRAENORA_RUNTIME_API bool ok() const;
    // The number of diagnostics with severity error.
    PRAENORA_RUNTIME_API std::size_t error_count() const;
    // The first diagnostic with severity error, or nullptr when there is none. The pointer is
    // valid until the diagnostics change.
    PRAENORA_RUNTIME_API const Diagnostic* first_error() const;
    PRAENORA_RUNTIME_API void add(Diagnostic diagnostic);

    // {"ok":bool,"diagnostics":[{"severity","code","name","category","message","location",
    // "step","module","details"}]}; "step", "module" and "details" appear only when set.
    PRAENORA_RUNTIME_API std::string to_json() const;

    // One line per diagnostic, each ending in "\n", in the compiler-style format
    // `file:line:col: severity[NAME]: pointer: message`. Empty parts are omitted together with the
    // colon that would follow them.
    PRAENORA_RUNTIME_API std::string to_text() const;
};

} // namespace praenora

#endif // PRAENORA_RUNTIME_ERROR_HPP
