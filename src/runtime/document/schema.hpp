// A compiled JSON Schema (draft-07), backed by pboettch/json-schema-validator.
#ifndef PRAENORA_RUNTIME_DOCUMENT_SCHEMA_HPP
#define PRAENORA_RUNTIME_DOCUMENT_SCHEMA_HPP

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace praenora::detail {

// One validation problem: where (a JSON pointer into the instance; "" is the whole instance) and
// what, in the validator's words.
struct SchemaIssue {
    std::string pointer, message;
};

class CompiledSchema {
public:
    // Compiles `schema`; `name` identifies it in messages (for example "workflow-config@0.1").
    // Throws praenora::Error(SCHEMA_VIOLATION) when `schema` is not a valid draft-07 schema: it
    // is checked against the draft-07 meta-schema, a `$schema` other than draft-07 is refused,
    // and every `$ref` must resolve. External references may name embedded Praenora schemas
    // (urn:praenora:schema:<kind>:<major>.<minor>) or the draft-07 meta-schema, nothing else.
    // String formats the validator implements (date-time, uri, regex, ...) are checked; other
    // formats are accepted as annotations.
    CompiledSchema(const nlohmann::json& schema, std::string name);
    ~CompiledSchema();
    CompiledSchema(CompiledSchema&&) noexcept;
    CompiledSchema& operator=(CompiledSchema&&) noexcept;
    CompiledSchema(const CompiledSchema&) = delete;
    CompiledSchema& operator=(const CompiledSchema&) = delete;

    // Every problem of `instance`; empty when it is valid. Safe to call from several threads.
    std::vector<SchemaIssue> validate(const nlohmann::json& instance) const;

    // Validates `instance`, replaces the content of `issues` with its problems, and returns the
    // instance with the schema's `default` values added for missing properties of the objects
    // that are present.
    nlohmann::json apply_defaults(const nlohmann::json& instance,
                                  std::vector<SchemaIssue>& issues) const;

    const std::string& name() const;

private:
    // Validates; fills `issues` and returns the JSON patch that adds the defaults.
    nlohmann::json run(const nlohmann::json& instance, std::vector<SchemaIssue>& issues) const;

    struct Impl;
    std::string name_;
    std::unique_ptr<Impl> impl_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DOCUMENT_SCHEMA_HPP
