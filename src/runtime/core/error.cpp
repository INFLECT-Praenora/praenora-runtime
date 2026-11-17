// The error-code table and the implementation of praenora/runtime/error.hpp.
#include <praenora/runtime/error.hpp>

#include "core/location_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace praenora {

namespace {

constexpr int exit_code_for(ErrorCategory category) {
    switch (category) {
    case ErrorCategory::usage:
        return 2;
    case ErrorCategory::validation:
    case ErrorCategory::compatibility:
        return 3;
    case ErrorCategory::load:
        return 4;
    case ErrorCategory::execution:
    case ErrorCategory::data:
        return 1;
    case ErrorCategory::timeout:
        return 5;
    case ErrorCategory::cancelled:
        return 130;
    case ErrorCategory::environment:
        return 74;
    case ErrorCategory::internal:
        return 70;
    }
    return 70;
}

constexpr ErrorCodeInfo row(ErrorCode code, std::string_view name, ErrorCategory category,
                            std::string_view owner, bool retryable, std::string_view summary) {
    return ErrorCodeInfo{code, name, category, owner, exit_code_for(category), retryable, summary};
}

using C = ErrorCategory;
using E = ErrorCode;
constexpr bool retry = true;
constexpr bool no_retry = false;

// The taxonomy table, sorted by code. "none" stands for "no owner": the code is not a fault.
constexpr std::array kTable = {
    // OK is not a failure. It has no category of its own; it is reported as execution with exit 0.
    ErrorCodeInfo{E::OK, "OK", C::execution, "none", 0, no_retry, "The operation succeeded."},
    row(E::UNKNOWN, "UNKNOWN", C::internal, "runtime", no_retry,
        "An error of unknown origin occurred."),
    row(E::INVALID_ARGUMENT, "INVALID_ARGUMENT", C::usage, "caller", no_retry,
        "An argument or input value is invalid."),
    row(E::INVALID_STATE, "INVALID_STATE", C::usage, "caller/runtime", no_retry,
        "The operation is not allowed in the current state."),
    row(E::NOT_FOUND, "NOT_FOUND", C::environment, "environment", no_retry,
        "A required file, directory or resource does not exist."),
    row(E::UNSUPPORTED, "UNSUPPORTED", C::compatibility, "caller/module", no_retry,
        "The requested feature or version is not supported."),
    row(E::RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED", C::environment, "environment", retry,
        "A resource limit was reached, for example memory or an output size limit."),
    row(E::CANCELLED, "CANCELLED", C::cancelled, "none", no_retry, "The operation was cancelled."),
    row(E::TIMEOUT, "TIMEOUT", C::timeout, "module", retry,
        "The operation did not finish before its deadline."),
    row(E::IO_ERROR, "IO_ERROR", C::environment, "environment", retry,
        "Reading or writing a file failed."),
    row(E::INTERNAL, "INTERNAL", C::internal, "runtime/module", no_retry,
        "An internal invariant was violated."),
    row(E::FAILED, "FAILED", C::execution, "module", no_retry, "The operation failed."),
    row(E::DATA_INVALID, "DATA_INVALID", C::data, "user data/module", no_retry,
        "Data does not have the expected form or values."),
    row(E::PARSE_ERROR, "PARSE_ERROR", C::validation, "user", no_retry,
        "A document is not well-formed JSON or YAML, or exceeds a document limit."),
    row(E::SCHEMA_VIOLATION, "SCHEMA_VIOLATION", C::validation, "user/module", no_retry,
        "A document or payload does not satisfy its JSON Schema."),
    row(E::FORMAT_VERSION_UNSUPPORTED, "FORMAT_VERSION_UNSUPPORTED", C::validation, "user/module",
        no_retry, "The document's format_version is not supported by this runtime."),
    row(E::DOCUMENT_KIND_MISMATCH, "DOCUMENT_KIND_MISMATCH", C::validation, "user", no_retry,
        "The document's kind is not the kind expected."),
    row(E::CONFIG_INVALID, "CONFIG_INVALID", C::validation, "user", no_retry,
        "A configuration value is invalid."),
    row(E::WORKFLOW_NOT_FOUND, "WORKFLOW_NOT_FOUND", C::validation, "user", no_retry,
        "The configured workflow definition is not in the catalog."),
    row(E::BINDING_INVALID, "BINDING_INVALID", C::validation, "user", no_retry,
        "The step bindings do not match the steps of the workflow definition."),
    row(E::PARAMETERS_INVALID, "PARAMETERS_INVALID", C::validation, "user", no_retry,
        "Step parameters do not satisfy the module's parameter schema."),
    row(E::INPUT_BINDING_INVALID, "INPUT_BINDING_INVALID", C::validation, "user", no_retry,
        "A workflow input is not bound to a valid data source."),
    row(E::MANIFEST_INVALID, "MANIFEST_INVALID", C::validation, "module", no_retry,
        "A module or provider manifest is malformed or inconsistent."),
    row(E::MODULE_NOT_FOUND, "MODULE_NOT_FOUND", C::validation, "user/environment", no_retry,
        "No manifest for the requested module name and version is on the search paths."),
    row(E::MODULE_IDENTITY_MISMATCH, "MODULE_IDENTITY_MISMATCH", C::load, "module", no_retry,
        "The loaded module's describe output disagrees with its manifest."),
    row(E::LIBRARY_DIGEST_MISMATCH, "LIBRARY_DIGEST_MISMATCH", C::load, "environment/module",
        no_retry,
        "A module or provider library's SHA-256 differs from the digest in its manifest."),
    row(E::LIBRARY_NOT_FOUND, "LIBRARY_NOT_FOUND", C::validation, "module/environment", no_retry,
        "The module or provider library named by the manifest does not exist."),
    row(E::ABI_INCOMPATIBLE, "ABI_INCOMPATIBLE", C::validation, "module", no_retry,
        "The module was built for a different ABI major version."),
    row(E::PROTOCOL_INCOMPATIBLE, "PROTOCOL_INCOMPATIBLE", C::validation, "module", no_retry,
        "The module's protocol (PMP) version is incompatible with the runtime."),
    row(E::CAPABILITY_NOT_IMPLEMENTED, "CAPABILITY_NOT_IMPLEMENTED", C::validation, "user/module",
        no_retry, "The module does not implement the capability the step requires."),
    row(E::PLATFORM_MISMATCH, "PLATFORM_MISMATCH", C::validation, "environment", no_retry,
        "No manifest of the module matches the host operating system and architecture."),
    row(E::MESSAGE_TYPE_INCOMPATIBLE, "MESSAGE_TYPE_INCOMPATIBLE", C::validation, "protocol",
        no_retry,
        "A message type version differs from the one required, such as at the two ends of a "
        "connection."),
    row(E::CATALOG_ENTRY_NOT_FOUND, "CATALOG_ENTRY_NOT_FOUND", C::validation, "protocol", no_retry,
        "A referenced message type, capability or workflow definition is not in the catalog."),
    row(E::CATALOG_ENTRY_DUPLICATE, "CATALOG_ENTRY_DUPLICATE", C::compatibility, "protocol",
        no_retry, "Two catalog entries have the same id and version."),
    row(E::SERVICE_UNAVAILABLE, "SERVICE_UNAVAILABLE", C::compatibility, "environment", no_retry,
        "No usable service provider offers a runtime service that a module requires."),
    row(E::LIBRARY_LOAD_FAILED, "LIBRARY_LOAD_FAILED", C::load, "environment/module", no_retry,
        "A module or provider library could not be loaded, or a provider failed its load checks."),
    row(E::ENTRY_POINT_MISSING, "ENTRY_POINT_MISSING", C::load, "module", no_retry,
        "The library does not export its entry point (praenora_module_get_api or "
        "praenora_provider_get_api)."),
    row(E::API_TABLE_INVALID, "API_TABLE_INVALID", C::load, "module", no_retry,
        "The function table of a module or provider is missing, malformed or incomplete."),
    row(E::GRAPH_INVALID, "GRAPH_INVALID", C::validation, "protocol", no_retry,
        "The workflow graph breaks a structural rule, such as fan-in or a delayed connection "
        "into a required input."),
    row(E::GRAPH_CYCLE, "GRAPH_CYCLE", C::validation, "protocol", no_retry,
        "The connections without delay form a cycle."),
    row(E::PORT_UNCONNECTED, "PORT_UNCONNECTED", C::validation, "protocol", no_retry,
        "A required step input or a workflow output has no incoming connection."),
    row(E::PORT_UNKNOWN, "PORT_UNKNOWN", C::validation, "protocol", no_retry,
        "A connection names a step, port, workflow input or workflow output that does not "
        "exist."),
    row(E::PORT_TYPE_MISMATCH, "PORT_TYPE_MISMATCH", C::validation, "protocol", no_retry,
        "A connection joins ports of different message types."),
    row(E::STEP_FAILED, "STEP_FAILED", C::execution, "module", no_retry,
        "A module call returned an error."),
    row(E::OUTPUT_INVALID, "OUTPUT_INVALID", C::execution, "module", no_retry,
        "A module result is malformed, names an undeclared port or violates the port's schema."),
    row(E::STEP_TIMEOUT, "STEP_TIMEOUT", C::timeout, "module/config", retry,
        "A module call passed its deadline and returned within the cancellation grace period."),
    row(E::MODULE_UNRESPONSIVE, "MODULE_UNRESPONSIVE", C::timeout, "module", no_retry,
        "A module call did not return within its deadline plus the cancellation grace period, or "
        "a call or load was refused because such a call may still be running in the module's "
        "library."),
    row(E::INPUT_MISSING, "INPUT_MISSING", C::execution, "runtime", no_retry,
        "A required input message was not available when a step ran."),
    row(E::RUN_STOPPED, "RUN_STOPPED", C::cancelled, "none", no_retry,
        "The run was stopped on request."),
    // Exit 1 during a run; the CLI reports 3 when validation finds it.
    row(E::TARGET_DATA_INVALID, "TARGET_DATA_INVALID", C::data, "user data", no_retry,
        "Target data is malformed or breaks the observation rules."),
    // Found by validation only, so the CLI reports 3.
    row(E::TARGET_DATA_UNAVAILABLE, "TARGET_DATA_UNAVAILABLE", C::data, "environment", retry,
        "Target data could not be read, for example because its file does not exist."),
};

constexpr bool table_is_sorted() {
    for (std::size_t i = 1; i < kTable.size(); ++i) {
        if (static_cast<std::int32_t>(kTable[i - 1].code) >=
            static_cast<std::int32_t>(kTable[i].code)) {
            return false;
        }
    }
    return true;
}
static_assert(table_is_sorted(), "the error-code table must be sorted by code");

const std::vector<ErrorCodeInfo>& table() {
    static const std::vector<ErrorCodeInfo> rows(kTable.begin(), kTable.end());
    return rows;
}

constexpr std::string_view severity_name(Severity severity) {
    switch (severity) {
    case Severity::error:
        return "error";
    case Severity::warning:
        return "warning";
    case Severity::note:
        return "note";
    }
    return "error";
}

// Dumps without throwing on invalid UTF-8, which messages from modules may contain.
template <typename Json> std::string dump(const Json& json) {
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

nlohmann::ordered_json location_json(const SourceLocation& location) {
    return detail::location_parts<nlohmann::ordered_json>(location);
}

} // namespace

const ErrorCodeInfo& error_code_info(ErrorCode code) {
    const auto& rows = table();
    const auto it = std::lower_bound(
        rows.begin(), rows.end(), code, [](const ErrorCodeInfo& info, ErrorCode value) {
            return static_cast<std::int32_t>(info.code) < static_cast<std::int32_t>(value);
        });
    if (it != rows.end() && it->code == code) {
        return *it;
    }
    return error_code_info(ErrorCode::UNKNOWN);
}

const std::vector<ErrorCodeInfo>& all_error_codes() { return table(); }

std::optional<ErrorCode> error_code_from_name(std::string_view name) {
    for (const auto& info : table()) {
        if (info.name == name) {
            return info.code;
        }
    }
    return std::nullopt;
}

std::string to_string(ErrorCategory category) {
    switch (category) {
    case ErrorCategory::usage:
        return "usage";
    case ErrorCategory::validation:
        return "validation";
    case ErrorCategory::compatibility:
        return "compatibility";
    case ErrorCategory::load:
        return "load";
    case ErrorCategory::execution:
        return "execution";
    case ErrorCategory::timeout:
        return "timeout";
    case ErrorCategory::cancelled:
        return "cancelled";
    case ErrorCategory::data:
        return "data";
    case ErrorCategory::environment:
        return "environment";
    case ErrorCategory::internal:
        return "internal";
    }
    return "internal";
}

struct Error::Payload {
    SourceLocation location;
    std::string details_json;
};

Error::Error(ErrorCode code, std::string message, SourceLocation location, std::string details_json)
    : std::runtime_error(message), code_(code),
      payload_(
          std::make_shared<const Payload>(Payload{std::move(location), std::move(details_json)})) {}

Error::~Error() = default;

ErrorCode Error::code() const noexcept { return code_; }

const SourceLocation& Error::location() const noexcept { return payload_->location; }

const std::string& Error::details_json() const noexcept { return payload_->details_json; }

std::string Error::to_json() const {
    const auto& info = error_code_info(code_);
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    json["code"] = static_cast<std::int32_t>(code_);
    json["name"] = std::string(info.name);
    json["category"] = to_string(info.category);
    json["message"] = what();
    json["location"] = location_json(payload_->location);
    auto details = nlohmann::ordered_json::parse(payload_->details_json, nullptr, false);
    if (details.is_discarded()) {
        json["details"] = payload_->details_json;
    } else {
        json["details"] = std::move(details);
    }
    return dump(json);
}

bool ValidationReport::ok() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& d) { return d.severity == Severity::error; });
}

std::size_t ValidationReport::error_count() const {
    return static_cast<std::size_t>(
        std::count_if(diagnostics.begin(), diagnostics.end(),
                      [](const Diagnostic& d) { return d.severity == Severity::error; }));
}

const Diagnostic* ValidationReport::first_error() const {
    const auto first =
        std::find_if(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic& d) { return d.severity == Severity::error; });
    return first == diagnostics.end() ? nullptr : &*first;
}

void ValidationReport::add(Diagnostic diagnostic) { diagnostics.push_back(std::move(diagnostic)); }

std::string ValidationReport::to_json() const {
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    json["ok"] = ok();
    auto list = nlohmann::ordered_json::array();
    for (const auto& d : diagnostics) {
        const auto& info = error_code_info(d.code);
        nlohmann::ordered_json item = nlohmann::ordered_json::object();
        item["severity"] = std::string(severity_name(d.severity));
        item["code"] = static_cast<std::int32_t>(d.code);
        item["name"] = std::string(info.name);
        item["category"] = to_string(info.category);
        item["message"] = d.message;
        item["location"] = location_json(d.location);
        if (!d.step.empty()) {
            item["step"] = d.step;
        }
        if (!d.module.empty()) {
            item["module"] = d.module;
        }
        if (!d.details_json.empty()) {
            auto details = nlohmann::ordered_json::parse(d.details_json, nullptr, false);
            if (details.is_discarded()) {
                item["details"] = d.details_json;
            } else {
                item["details"] = std::move(details);
            }
        }
        list.push_back(std::move(item));
    }
    json["diagnostics"] = std::move(list);
    return dump(json);
}

std::string ValidationReport::to_text() const {
    std::string text;
    for (const auto& d : diagnostics) {
        const auto& location = d.location;
        std::string where = location.file;
        if (location.line > 0) {
            where += (where.empty() ? "" : ":") + std::to_string(location.line);
            if (location.column > 0) {
                where += ":" + std::to_string(location.column);
            }
        }
        if (!where.empty()) {
            text += where;
            text += ": ";
        }
        text += severity_name(d.severity);
        text += '[';
        text += error_code_info(d.code).name;
        text += "]: ";
        if (!location.pointer.empty()) {
            text += location.pointer;
            text += ": ";
        }
        text += d.message;
        text += '\n';
    }
    return text;
}

} // namespace praenora
