// StatusSnapshot::to_json(): the snapshot as a status-snapshot document. Instants become RFC 3339
// UTC strings, durations stay integers of nanoseconds (fields ending in _ns).
#include <praenora/runtime/status.hpp>

#include "core/time.hpp"
#include "observability/status_json.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace praenora {

namespace {

using nlohmann::json;

// An instant as RFC 3339, or null when it is not set (0) or cannot be written.
json instant(std::int64_t unix_ns) {
    if (unix_ns == 0) {
        return nullptr;
    }
    try {
        return detail::format_rfc3339_utc(unix_ns);
    } catch (const Error&) {
        return nullptr;
    }
}

json instant(const std::optional<std::int64_t>& unix_ns) {
    return unix_ns ? instant(*unix_ns) : json(nullptr);
}

json error_record(const std::optional<ErrorRecord>& record) {
    return record ? detail::error_record_json(*record) : json(nullptr);
}

} // namespace

namespace detail {

json error_record_json(const ErrorRecord& record) {
    json cause = nullptr;
    if (!record.cause_json.empty()) {
        // The module's error document; anything that is not a JSON object is kept as text.
        cause = json::parse(record.cause_json, nullptr, false);
        if (!cause.is_object()) {
            cause = json{{"text", record.cause_json}};
        }
    }
    return {{"code", static_cast<std::int32_t>(record.code)},
            {"name", std::string(error_code_info(record.code).name)},
            {"message", record.message},
            {"tick", record.tick ? json(*record.tick) : json(nullptr)},
            {"at", instant(record.at_unix_ns)},
            {"cause", std::move(cause)}};
}

} // namespace detail

std::string StatusSnapshot::to_json() const {
    json steps_json = json::array();
    for (const StepStatus& step : steps) {
        steps_json.push_back({{"step", step.step},
                              {"module_name", step.module_name},
                              {"module_version", step.module_version},
                              {"state", praenora::to_string(step.state)},
                              {"invocations", step.invocations},
                              {"succeeded", step.succeeded},
                              {"failed", step.failed},
                              {"timed_out", step.timed_out},
                              {"cancelled", step.cancelled},
                              {"last_duration_ns", step.last_duration_ns},
                              {"total_duration_ns", step.total_duration_ns},
                              {"min_duration_ns", step.min_duration_ns},
                              {"max_duration_ns", step.max_duration_ns},
                              {"mean_duration_ns", step.mean_duration_ns},
                              {"last_invocation_at", instant(step.last_invocation_at_unix_ns)},
                              {"last_success_at", instant(step.last_success_at_unix_ns)},
                              {"last_error", error_record(step.last_error)}});
    }
    const json workflow = {{"name", workflow_name},
                           {"version", workflow_version},
                           {"fingerprint", fingerprint},
                           {"state", praenora::to_string(state)},
                           {"health", praenora::to_string(health)},
                           {"started_at", instant(started_at_unix_ns)},
                           {"uptime_ns", uptime_ns},
                           {"ticks_completed", ticks_completed},
                           {"current_tick", current_tick ? json(*current_tick) : json(nullptr)},
                           {"logical_time", logical_time ? json(*logical_time) : json(nullptr)},
                           {"last_tick_duration_ns", last_tick_duration_ns},
                           {"last_error", error_record(last_error)}};
    const VersionInfo versions = version_info();
    json generated_at = instant(generated_at_unix_ns);
    if (generated_at.is_null()) {
        generated_at = detail::format_rfc3339_utc(0); // required: never null
    }
    const json document = {{"kind", "status-snapshot"},
                           {"format_version", versions.formats.at("status-snapshot")},
                           {"generated_at", std::move(generated_at)},
                           {"runtime_version", runtime_version},
                           {"pid", pid},
                           {"workflow", workflow},
                           {"steps", std::move(steps_json)}};
    // Messages may come from modules: bytes that are not UTF-8 are replaced, not refused.
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace praenora
