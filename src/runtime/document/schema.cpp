#include "document/schema.hpp"

#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json-schema.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace praenora::detail {

namespace {

constexpr std::string_view kDraft07 = "http://json-schema.org/draft-07/schema";
constexpr std::string_view kUrnPrefix = "urn:praenora:schema:";

bool is_draft07(std::string_view uri) {
    return uri == kDraft07 || (uri.size() == kDraft07.size() + 1 &&
                               uri.compare(0, kDraft07.size(), kDraft07) == 0 && uri.back() == '#');
}

// The validator's own checks for the formats it implements; any other format is an annotation.
// default_string_format_check() signals an unimplemented format with std::logic_error and most
// non-conforming values with std::invalid_argument (itself a logic_error), which is rethrown. Any
// other exception (for example the one an invalid "regex" value produces) is not a logic_error and
// propagates as well; either way the validator records the value as invalid.
void check_format(const std::string& format, const std::string& value) {
    try {
        nlohmann::json_schema::default_string_format_check(format, value);
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::logic_error&) {
        // Not implemented by the validator: accepted.
    }
}

class CollectingErrorHandler : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& instance,
               const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        issues.push_back(refine(SchemaIssue{pointer.to_string(), message}));
    }

    std::vector<SchemaIssue> issues;

private:
    // The validator reports a property rejected by `additionalProperties` at the enclosing
    // object, as "validation failed for additional property '<key>': <reason>". Point at the
    // property instead, so diagnostics name its line, and say plainly when it is not allowed.
    static SchemaIssue refine(SchemaIssue issue) {
        constexpr std::string_view kPrefix = "validation failed for additional property '";
        if (issue.message.compare(0, kPrefix.size(), kPrefix) != 0) {
            return issue;
        }
        const auto end = issue.message.find("': ", kPrefix.size());
        if (end == std::string::npos) {
            return issue;
        }
        const std::string key = issue.message.substr(kPrefix.size(), end - kPrefix.size());
        const std::string reason = issue.message.substr(end + 3);
        issue.pointer += "/" + json_pointer_escape(key);
        if (reason == "instance invalid as per false-schema") {
            issue.message = "property '" + key + "' is not allowed";
        }
        return issue;
    }
};

// Resolves the external references of a schema while it compiles.
void load_referenced_schema(const nlohmann::json_uri& uri, nlohmann::json& value) {
    const std::string location = uri.location();
    if (is_draft07(location)) {
        value = nlohmann::json_schema::draft7_schema_builtin;
        return;
    }
    if (location.compare(0, kUrnPrefix.size(), kUrnPrefix) == 0) {
        // urn:praenora:schema:<kind>:<major>.<minor>
        const std::string rest = location.substr(kUrnPrefix.size());
        const auto colon = rest.rfind(':');
        if (colon != std::string::npos) {
            const SemVer version = parse_semver(rest.substr(colon + 1) + ".0");
            if (version.prerelease.empty() && version.build.empty()) {
                const auto text =
                    SchemaRegistry::builtin().text_for_kind(rest.substr(0, colon), version);
                value = nlohmann::json::parse(text.begin(), text.end());
                return;
            }
        }
    }
    throw std::invalid_argument("cannot resolve the reference '" + location +
                                "': only urn:praenora:schema:<kind>:<major>.<minor> and the "
                                "draft-07 meta-schema can be referenced");
}

const nlohmann::json_schema::json_validator& meta_schema_validator() {
    static const nlohmann::json_schema::json_validator validator(
        nlohmann::json_schema::draft7_schema_builtin, nullptr, check_format);
    return validator;
}

[[noreturn]] void schema_violation(const std::string& message, const std::string& pointer = "") {
    SourceLocation location;
    location.pointer = pointer;
    throw Error(ErrorCode::SCHEMA_VIOLATION, message, location);
}

} // namespace

struct CompiledSchema::Impl {
    nlohmann::json_schema::json_validator validator{load_referenced_schema, check_format};
};

CompiledSchema::CompiledSchema(const nlohmann::json& schema, std::string name)
    : name_(std::move(name)), impl_(std::make_unique<Impl>()) {
    const std::string subject = "schema '" + name_ + "'";
    if (schema.is_object()) {
        const auto declared = schema.find("$schema");
        if (declared != schema.end() &&
            (!declared->is_string() || !is_draft07(declared->get<std::string>()))) {
            schema_violation(subject + " declares $schema " + declared->dump() +
                                 "; Praenora uses JSON Schema draft-07 (" + std::string(kDraft07) +
                                 "#)",
                             "/$schema");
        }
    }

    CollectingErrorHandler meta_issues;
    meta_schema_validator().validate(schema, meta_issues);
    if (!meta_issues.issues.empty()) {
        const auto& first = meta_issues.issues.front();
        std::string message = subject + " is not a valid JSON Schema (draft-07): " +
                              (first.pointer.empty() ? std::string("(root)") : first.pointer) +
                              ": " + first.message;
        if (meta_issues.issues.size() > 1) {
            message += " (and " + std::to_string(meta_issues.issues.size() - 1) + " more)";
        }
        schema_violation(message, first.pointer);
    }

    try {
        impl_->validator.set_root_schema(schema);
    } catch (const std::exception& e) {
        schema_violation(subject + " cannot be compiled: " + e.what());
    }
}

CompiledSchema::~CompiledSchema() = default;
CompiledSchema::CompiledSchema(CompiledSchema&&) noexcept = default;
CompiledSchema& CompiledSchema::operator=(CompiledSchema&&) noexcept = default;

std::vector<SchemaIssue> CompiledSchema::validate(const nlohmann::json& instance) const {
    std::vector<SchemaIssue> issues;
    run(instance, issues);
    return issues;
}

nlohmann::json CompiledSchema::apply_defaults(const nlohmann::json& instance,
                                              std::vector<SchemaIssue>& issues) const {
    const nlohmann::json patch = run(instance, issues);
    if (!patch.is_array() || patch.empty()) {
        return instance;
    }
    try {
        return instance.patch(patch);
    } catch (const std::exception& e) {
        issues.push_back(SchemaIssue{"", std::string("cannot apply defaults: ") + e.what()});
        return instance;
    }
}

nlohmann::json CompiledSchema::run(const nlohmann::json& instance,
                                   std::vector<SchemaIssue>& issues) const {
    CollectingErrorHandler handler;
    nlohmann::json patch;
    try {
        patch = impl_->validator.validate(instance, handler);
    } catch (const std::exception& e) {
        handler.issues.push_back(SchemaIssue{"", std::string("validation failed: ") + e.what()});
    }
    issues = std::move(handler.issues);
    return patch;
}

const std::string& CompiledSchema::name() const { return name_; }

} // namespace praenora::detail
