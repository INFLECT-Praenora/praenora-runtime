#include "api/module_check_cases.hpp"

#include "core/paths.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "engine/envelope.hpp"

#include <algorithm>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace module_check_text {

// "<file>:<line>:<column>: <pointer>: ", leaving out the parts that are not set.
std::string where(const SourceLocation& location) {
    std::string text;
    if (!location.file.empty()) {
        text += location.file;
        if (location.line > 0) {
            text += ":" + std::to_string(location.line) + ":" + std::to_string(location.column);
        }
        text += ": ";
    }
    if (!location.pointer.empty()) {
        text += location.pointer + ": ";
    }
    return text;
}

// An error's message, preceded by its file, line and column unless the message names the file.
std::string text_of(const Error& error) {
    const SourceLocation& location = error.location();
    const std::string message = error.what();
    if (location.file.empty() || message.find(location.file) != std::string::npos) {
        return message;
    }
    SourceLocation file_only = location;
    file_only.pointer.clear();
    return where(file_only) + message;
}

// The first of `issues`, located in `document`, with the number of the others.
std::string first_issue(const LoadedDocument& document, const std::vector<SchemaIssue>& issues) {
    std::string text = where(locate(document, issues.front().pointer)) + issues.front().message;
    if (issues.size() > 1) {
        text += " (and " + std::to_string(issues.size() - 1) + " more problem" +
                (issues.size() > 2 ? "s" : "") + ")";
    }
    return text;
}

} // namespace module_check_text

namespace {

using module_check_text::first_issue;
using module_check_text::text_of;
using module_check_text::where;

std::string identity(const std::string& id, const SemVer& version) {
    return id + "@" + to_string(version);
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// Checks the data of one tick's inputs (`inputs`) or expected outputs against the ports of
// the case's capability: each port exists and its data satisfies the port's message type, and
// every required input is there.
template <class Problem>
bool check_ports(const Catalog& catalog, const CheckCase& c, const json& data, bool inputs,
                 const std::string& pointer, Problem& problem) {
    const auto& ports = inputs ? c.definition->inputs : c.definition->outputs;
    const char* const side = inputs ? "input" : "output";
    const std::string capability = identity(c.capability.id, c.capability.version);
    for (const auto& [port, value] : data.items()) {
        const auto def = ports.find(port);
        if (def == ports.end()) {
            problem(pointer + "/" + json_pointer_escape(port), c.label + ": capability " +
                                                                   capability + " has no " + side +
                                                                   " port " + quoted(port));
            return false;
        }
        const MessageTypeDef* type =
            catalog.find_message_type(def->second.type, def->second.type_version);
        const auto issues = type->compiled->validate(value);
        if (!issues.empty()) {
            problem(pointer + "/" + json_pointer_escape(port) + issues.front().pointer,
                    c.label + ": the data of " + side + " " + quoted(port) +
                        " does not satisfy its message type " + identity(type->id, type->version) +
                        ": " + issues.front().message);
            return false;
        }
    }
    if (inputs) {
        for (const auto& [port, def] : ports) {
            if (def.required && !data.contains(port)) {
                problem(pointer, c.label + ": the required input " + quoted(port) + " is missing");
                return false;
            }
        }
    }
    return true;
}

} // namespace

TestVectors read_test_vectors(const fs::path& path, const ModuleManifest& manifest,
                              const Catalog& catalog) {
    TestVectors result;
    // Messages name the file once, in the location of the problem.
    const std::string invalid = "the test vectors are invalid: ";
    LoadedDocument document;
    try {
        document = load_document_file(path);
    } catch (const Error& error) {
        // The error names the file.
        if (error.code() == ErrorCode::IO_ERROR) {
            throw Error(ErrorCode::IO_ERROR,
                        std::string("cannot read the test vectors: ") + error.what(),
                        error.location(), error.details_json());
        }
        result.invalid = "the test vectors are invalid: " + text_of(error);
        return result;
    }
    const auto problem = [&](const std::string& pointer, const std::string& message) {
        result.invalid = invalid + where(locate(document, pointer)) + message;
    };
    const json& root = document.json;
    if (!root.is_object() || root.value("kind", json()) != "module-test-vectors") {
        problem(root.is_object() && root.contains("kind") ? "/kind" : "",
                "kind must be \"module-test-vectors\"");
        return result;
    }
    std::optional<SemVer> version;
    if (root.contains("format_version") && root.at("format_version").is_string()) {
        try {
            version = parse_semver(root.at("format_version").get<std::string>());
        } catch (const Error&) {
            // Reported below.
        }
    }
    if (!version) {
        problem(root.contains("format_version") ? "/format_version" : "",
                "format_version must be a version string such as \"0.1.0\"");
        return result;
    }
    const CompiledSchema* schema = nullptr;
    try {
        schema = &SchemaRegistry::builtin().for_kind("module-test-vectors", *version);
    } catch (const Error& error) {
        problem("/format_version", error.what());
        return result;
    }
    std::vector<SchemaIssue> issues;
    const json vectors = schema->apply_defaults(root, issues);
    if (!issues.empty()) {
        result.invalid =
            invalid + first_issue(document, issues) + " (schema " + schema->name() + ")";
        return result;
    }

    std::vector<CheckCase> cases;
    const json& list = vectors.at("cases");
    for (std::size_t i = 0; i < list.size(); ++i) {
        const json& item = list[i];
        const std::string base = "/cases/" + std::to_string(i);
        CheckCase c;
        c.label = item.contains("name") ? "case " + quoted(item.at("name").get<std::string>())
                                        : "case " + std::to_string(i + 1);
        const std::string capability_id = item.at("capability").at("id").get<std::string>();
        const std::string capability_version =
            item.at("capability").at("version").get<std::string>();
        try {
            c.capability = {capability_id, parse_semver(capability_version)};
        } catch (const Error& error) {
            problem(base + "/capability/version", error.what());
            return result;
        }
        const std::string name = identity(c.capability.id, c.capability.version);
        const auto& implemented = manifest.capabilities;
        if (std::none_of(implemented.begin(), implemented.end(), [&c](const CapabilityRef& ref) {
                return ref.id == c.capability.id && ref.version == c.capability.version;
            })) {
            problem(base + "/capability",
                    c.label + " uses capability " + name + ", which the module does not implement");
            return result;
        }
        c.definition = catalog.find_capability(c.capability.id, c.capability.version);
        c.parameters =
            manifest.parameters->apply_defaults(item.value("parameters", json::object()), issues);
        if (!issues.empty()) {
            problem(base + "/parameters" + issues.front().pointer,
                    c.label +
                        ": the parameters do not satisfy the module's "
                        "parameters_schema: " +
                        issues.front().message);
            return result;
        }
        c.seed = item.value("seed", std::uint64_t{0});
        if (const auto time = item.find("time"); time != item.end()) {
            c.unit = time->value("unit", c.unit);
            c.start = time->value("start", c.start);
            c.period = time->value("period", c.period);
        }
        const json& ticks = item.at("ticks");
        for (std::size_t t = 0; t < ticks.size(); ++t) {
            const std::string at = base + "/ticks/" + std::to_string(t);
            VectorTick tick;
            tick.inputs = ticks[t].at("inputs");
            if (!check_ports(catalog, c, tick.inputs, true, at + "/inputs", problem)) {
                return result;
            }
            if (const auto expect = ticks[t].find("expect"); expect != ticks[t].end()) {
                tick.expected = expect->at("outputs");
                if (!check_ports(catalog, c, *tick.expected, false, at + "/expect/outputs",
                                 problem)) {
                    return result;
                }
                if (const auto tolerance = expect->find("tolerance"); tolerance != expect->end()) {
                    tick.relative = tolerance->value("relative", 0.0);
                    tick.absolute = tolerance->value("absolute", 0.0);
                }
            }
            c.ticks.push_back(std::move(tick));
        }
        c.fingerprint = sha256_hex(canonical_json(item));
        cases.push_back(std::move(c));
    }
    result.cases = std::move(cases);
    return result;
}

std::optional<CheckCase> default_case(const ModuleManifest& manifest, const Catalog& catalog,
                                      bool vectors_invalid, std::string& problem) {
    CheckCase fallback;
    fallback.label = "the default case";
    fallback.capability = manifest.capabilities.front();
    fallback.definition =
        catalog.find_capability(fallback.capability.id, fallback.capability.version);
    std::vector<SchemaIssue> issues;
    fallback.parameters = manifest.parameters->apply_defaults(json::object(), issues);
    if (!issues.empty()) {
        problem = "no parameters to create an instance with: the module's parameters_schema "
                  "does not accept {} with its defaults (" +
                  where(SourceLocation{"", 0, 0, issues.front().pointer}) + issues.front().message +
                  "), and " +
                  (vectors_invalid
                       ? std::string("the test vectors are invalid (see invoke.result_schema)")
                       : std::string("there are no test vectors to take parameters from"));
        return std::nullopt;
    }
    fallback.fingerprint = sha256_hex(canonical_json(
        {{"capability",
          {{"id", fallback.capability.id}, {"version", to_string(fallback.capability.version)}}},
         {"parameters", fallback.parameters}}));
    return fallback;
}

std::string create_document(const CheckCase& c, const ModuleManifest& manifest) {
    return canonical_json(
        {{"pmp_version", kPmpVersion},
         {"step", kCheckStep},
         {"capability", {{"id", c.capability.id}, {"version", to_string(c.capability.version)}}},
         {"module", {{"name", manifest.name}, {"version", to_string(manifest.version)}}},
         {"parameters", c.parameters}});
}

std::string initialize_document(const CheckCase& c) {
    return canonical_json({{"pmp_version", kPmpVersion},
                           {"run", {{"fingerprint", c.fingerprint}, {"seed", c.seed}}},
                           {"time", {{"unit", c.unit}, {"start", c.start}, {"period", c.period}}}});
}

std::string invocation_document(const CheckCase& c, std::size_t t) {
    const std::uint64_t tick = t;
    const double logical_time = c.start + static_cast<double>(tick + 1) * c.period;
    json inputs = json::object();
    for (const auto& [port, data] : c.ticks[t].inputs.items()) {
        const PortDef& def = c.definition->inputs.at(port);
        const Endpoint source{Endpoint::Kind::workflow_input, "inputs", port};
        inputs[port] = make_envelope(fingerprint16(c.fingerprint), tick, logical_time, source,
                                     def.type, def.type_version, data);
    }
    return canonical_json({{"pmp_version", kPmpVersion},
                           {"step", kCheckStep},
                           {"tick", tick},
                           {"logical_time", logical_time},
                           {"inputs", std::move(inputs)}});
}

} // namespace praenora::detail
