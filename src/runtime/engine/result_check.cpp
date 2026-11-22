#include "engine/result_check.hpp"

#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"

#include <praenora/runtime/error.hpp>

#include <cstddef>
#include <vector>

namespace praenora::detail {

namespace {

using nlohmann::json;

// How much of a result that is not JSON the problem keeps.
constexpr std::size_t kRawPrefixBytes = 200;

std::string identity(const std::string& name, const SemVer& version) {
    return name + "@" + to_string(version);
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

const CompiledSchema& result_schema() {
    static const CompiledSchema& schema =
        SchemaRegistry::builtin().for_kind("pmp-result", parse_semver(kPmpVersion));
    return schema;
}

} // namespace

std::string utf8_prefix(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
        --end; // text[end] continues a sequence that starts before it
    }
    return text.substr(0, end);
}

json issues_json(const std::vector<SchemaIssue>& issues) {
    json list = json::array();
    for (const SchemaIssue& issue : issues) {
        list.push_back({{"pointer", issue.pointer}, {"message", issue.message}});
    }
    return list;
}

std::optional<ResultProblem> check_invoke_result(const std::string& result,
                                                 const CapabilityDef& capability,
                                                 bool validate_payloads,
                                                 const PortTypeLookup& type_of,
                                                 AcceptedResult& accepted) {
    json document;
    try {
        document = parse_json_text(result, "result");
    } catch (const Error& error) {
        return ResultProblem{std::string("it is not JSON (") + error.what() + ")",
                             "",
                             {{"raw_prefix", utf8_prefix(result, kRawPrefixBytes)}}};
    }
    if (const auto issues = result_schema().validate(document); !issues.empty()) {
        return ResultProblem{"it does not satisfy pmp-result: " + issues.front().pointer + ": " +
                                 issues.front().message,
                             issues.front().pointer,
                             {{"issues", issues_json(issues)}}};
    }
    const json& outputs = document.at("outputs");
    std::string declared;
    for (const auto& entry : capability.outputs) {
        declared += (declared.empty() ? "" : ", ") + entry.first;
    }
    for (const auto& output : outputs.items()) {
        if (capability.outputs.count(output.key()) == 0) {
            return ResultProblem{"it names the output " + quoted(output.key()) +
                                     ", which capability " +
                                     identity(capability.id, capability.version) +
                                     " does not declare; its outputs are " + declared,
                                 "/outputs/" + json_pointer_escape(output.key()),
                                 {{"port", output.key()}}};
        }
    }
    for (const auto& [port, def] : capability.outputs) {
        if (def.required && !outputs.contains(port)) {
            return ResultProblem{
                "it lacks the required output " + quoted(port), "/outputs", {{"port", port}}};
        }
    }
    AcceptedResult checked;
    for (const auto& [port, def] : capability.outputs) { // sorted by port
        const auto output = outputs.find(port);
        if (output == outputs.end()) {
            continue; // an optional output left out
        }
        const json& data = output->at("data");
        if (validate_payloads) {
            const MessageTypeDef& type = type_of(def);
            if (const auto issues = type.compiled->validate(data); !issues.empty()) {
                return ResultProblem{
                    "output " + quoted(port) + " does not satisfy its message type " +
                        identity(type.id, type.version) + ": " + issues.front().pointer + ": " +
                        issues.front().message,
                    "/outputs/" + json_pointer_escape(port) + "/data" + issues.front().pointer,
                    {{"port", port},
                     {"type", identity(type.id, type.version)},
                     {"issues", issues_json(issues)}}};
            }
        }
        checked.outputs.emplace(port, data);
    }
    if (const auto found = document.find("diagnostics"); found != document.end()) {
        checked.diagnostics = *found;
    }
    accepted = std::move(checked);
    return std::nullopt;
}

} // namespace praenora::detail
