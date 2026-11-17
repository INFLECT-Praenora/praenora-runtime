// What the two front ends of the runtime, the praenora tool (src/cli) and the Python extension
// (src/python), share so that they report the same documents and failures: the JSON of a module,
// of a service provider, of a catalog entry and of a run's summary, the failure of a validation
// and of the contract checks, the catalog kinds and graph formats by the names that they take, the
// names of the log levels and formats, and the limit of seeds and tick counts. Header-only and
// internal: the target praenora_frontend is neither installed nor exported.
#ifndef PRAENORA_FRONTEND_FRONTEND_HPP
#define PRAENORA_FRONTEND_FRONTEND_HPP

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace praenora::frontend {

// A module: {"name", "version", "manifest_path", "library_path", "os", "arch", "capabilities"}
// and, when it was loaded, "describe" (its describe() document) and "library_sha256".
inline nlohmann::json module_json(const ModuleInfo& module) {
    nlohmann::json item = {{"name", module.name},
                           {"version", module.version},
                           {"manifest_path", module.manifest_path},
                           {"library_path", module.library_path},
                           {"os", module.os},
                           {"arch", module.arch},
                           {"capabilities", module.capabilities}};
    if (!module.describe_json.empty()) {
        item["describe"] = nlohmann::json::parse(module.describe_json);
        item["library_sha256"] = module.library_sha256;
    }
    return item;
}

// A service as a provider offers it, "<name>@<major>.<minor>" (ProviderInfo::services), as
// {"name", "major", "minor"}. Throws std::invalid_argument for text of another form.
inline nlohmann::json service_json(std::string_view service) {
    const auto malformed = [service] {
        return std::invalid_argument("'" + std::string(service) +
                                     "' is not a service as <name>@<major>.<minor>");
    };
    const std::size_t at = service.find('@');
    if (at == 0 || at == std::string_view::npos) {
        throw malformed();
    }
    const std::size_t dot = service.find('.', at);
    if (dot == std::string_view::npos) {
        throw malformed();
    }
    // The decimal number service[begin, end), which fits in 32 bits.
    const auto number = [&](std::size_t begin, std::size_t end) {
        std::uint32_t value = 0;
        const char* last = service.data() + end;
        const auto [next, error] = std::from_chars(service.data() + begin, last, value);
        if (begin == end || error != std::errc() || next != last) {
            throw malformed();
        }
        return value;
    };
    return {{"name", std::string(service.substr(0, at))},
            {"major", number(at + 1, dot)},
            {"minor", number(dot + 1, service.size())}};
}

// A service provider: {"name", "version", "manifest", "library", "platform": {"os", "arch"},
// "services": [{"name", "major", "minor"}], "engine", "usable"} and, when it is not usable,
// "problem" (why not).
inline nlohmann::json provider_json(const ProviderInfo& provider) {
    nlohmann::json services = nlohmann::json::array();
    for (const std::string& service : provider.services) {
        services.push_back(service_json(service));
    }
    nlohmann::json item = {{"name", provider.name},
                           {"version", provider.version},
                           {"manifest", provider.manifest_path},
                           {"library", provider.library_path},
                           {"platform", {{"os", provider.os}, {"arch", provider.arch}}},
                           {"services", std::move(services)},
                           {"engine", nlohmann::json::parse(provider.engine_json)},
                           {"usable", provider.usable}};
    if (!provider.usable) {
        item["problem"] = provider.problem;
    }
    return item;
}

// A catalog entry: {"kind", "id", "version", "status", "summary"}.
inline nlohmann::json catalog_entry_json(const CatalogEntryInfo& entry) {
    return {{"kind", entry.kind},
            {"id", entry.id},
            {"version", entry.version},
            {"status", entry.status},
            {"summary", entry.summary}};
}

// The summary of a run: {"state", "ticks_completed", "fingerprint"}. (The praenora tool adds the
// run's "output_dir".)
inline nlohmann::json run_summary_json(const RunSummary& summary) {
    return {{"state", to_string(summary.final_state)},
            {"ticks_completed", summary.ticks_completed},
            {"fingerprint", summary.fingerprint}};
}

// The failure of the validation of `what` (a file) whose `report` holds an error: the first
// error's code and location, the message "<what>: validation failed: N error(s)" and
// `details_json` as the details (INTERNAL, without a location, should the report hold none).
inline Error validation_failure(const ValidationReport& report, const std::string& what,
                                std::string details_json = "{}") {
    const Diagnostic* first = report.first_error();
    return Error(first != nullptr ? first->code : ErrorCode::INTERNAL,
                 what + ": validation failed: " + std::to_string(report.error_count()) +
                     " error(s)",
                 first != nullptr ? first->location : SourceLocation{}, std::move(details_json));
}

// The failure of contract checks of which some failed: FAILED, the message "N of M contract
// checks failed: <ids>" (the failed checks, in report order) and the report as the details.
inline Error check_failure(const ModuleCheckReport& report) {
    std::size_t failed = 0;
    std::string ids;
    for (const CheckResult& check : report.checks) {
        if (check.status == "fail") {
            ids += (ids.empty() ? "" : ", ") + check.id;
            ++failed;
        }
    }
    return Error(ErrorCode::FAILED,
                 std::to_string(failed) + " of " + std::to_string(report.checks.size()) +
                     " contract checks failed: " + ids,
                 {}, report.to_json());
}

// The largest integer a JSON number holds exactly: seeds and tick counts go no higher.
inline constexpr std::uint64_t kMaxCount = 9007199254740991ULL;

// The kinds of catalog entries by the names that `praenora catalog list --kind` and
// list_catalog() take: workflows, capabilities and messages.
inline constexpr std::string_view kCatalogKindNames = "workflows, capabilities or messages";

// The entry kind ("workflow-definition", "capability-definition" or "message-type") that `name`
// stands for, or nullopt.
inline std::optional<std::string> catalog_kind(std::string_view name) {
    if (name == "workflows") {
        return "workflow-definition";
    }
    if (name == "capabilities") {
        return "capability-definition";
    }
    if (name == "messages") {
        return "message-type";
    }
    return std::nullopt;
}

// The graph formats by the names that `praenora graph --format` and graph() take.
inline constexpr std::string_view kGraphFormatNames = "text, json, dot or asyncapi";

// The graph format that `name` stands for (text, json, dot or asyncapi), or nullopt.
inline std::optional<GraphFormat> graph_format(std::string_view name) {
    if (name == "text") {
        return GraphFormat::text;
    }
    if (name == "json") {
        return GraphFormat::json;
    }
    if (name == "dot") {
        return GraphFormat::dot;
    }
    if (name == "asyncapi") {
        return GraphFormat::asyncapi;
    }
    return std::nullopt;
}

// The names of the log levels and formats, as messages list them.
inline constexpr std::string_view kLogLevelNames =
    "trace, debug, info, warn, error, critical or off";
inline constexpr std::string_view kLogFormatNames = "text or json";

// The canonical names: trace, debug, info, warn, error, critical and off; text and json.
inline std::string_view log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    case LogLevel::critical:
        return "critical";
    case LogLevel::off:
        return "off";
    }
    return "info";
}

inline std::string_view log_format_name(LogFormat format) {
    return format == LogFormat::json ? "json" : "text";
}

// `text` with the ASCII letters A-Z in lower case and every other byte as it is, whatever the
// process's locale (std::tolower would follow LC_CTYPE, which the Python interpreter takes from
// the environment).
inline std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

// The level or format that `name` names, in any case, or nullopt.
inline std::optional<LogLevel> log_level_from_name(std::string_view name) {
    constexpr LogLevel kLevels[] = {LogLevel::trace, LogLevel::debug, LogLevel::info,
                                    LogLevel::warn,  LogLevel::error, LogLevel::critical,
                                    LogLevel::off};
    const std::string wanted = lower(name);
    for (const LogLevel level : kLevels) {
        if (wanted == log_level_name(level)) {
            return level;
        }
    }
    return std::nullopt;
}

inline std::optional<LogFormat> log_format_from_name(std::string_view name) {
    const std::string wanted = lower(name);
    for (const LogFormat format : {LogFormat::text, LogFormat::json}) {
        if (wanted == log_format_name(format)) {
            return format;
        }
    }
    return std::nullopt;
}

} // namespace praenora::frontend

#endif // PRAENORA_FRONTEND_FRONTEND_HPP
