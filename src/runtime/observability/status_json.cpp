// StatusSnapshot::to_json(): the snapshot as a status-snapshot document. Instants become RFC 3339
// UTC strings, durations stay integers of nanoseconds (fields ending in _ns).
#include <praenora/runtime/status.hpp>

#include "core/location_json.hpp"
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

// The JSON object of `text`, {"text": text} for any other text, or null when `text` is empty or,
// unless `keep_empty`, an empty object.
json object_or_text(const std::string& text, bool keep_empty) {
    if (text.empty()) {
        return nullptr;
    }
    json value = json::parse(text, nullptr, false);
    if (!value.is_object()) {
        return json{{"text", text}};
    }
    if (value.empty() && !keep_empty) {
        return nullptr;
    }
    return value;
}

// The parts of `location` that are set, or null when none is.
json location_json(const SourceLocation& location) {
    json parts = detail::location_parts<json>(location);
    return parts.empty() ? json(nullptr) : parts;
}

} // namespace

namespace detail {

ErrorRecord error_record_of(const Error& error, std::optional<std::uint64_t> tick,
                            std::int64_t at_unix_ns) {
    ErrorRecord record;
    record.code = error.code();
    record.message = error.what();
    record.tick = tick;
    record.at_unix_ns = at_unix_ns;
    record.location = error.location();
    record.details_json = error.details_json();
    return record;
}

json error_record_json(const ErrorRecord& record) {
    return {{"code", static_cast<std::int32_t>(record.code)},
            {"name", std::string(error_code_info(record.code).name)},
            {"message", record.message},
            {"tick", record.tick ? json(*record.tick) : json(nullptr)},
            {"at", instant(record.at_unix_ns)},
            // The module's error document, which may be an empty object.
            {"cause", object_or_text(record.cause_json, true)},
            {"location", location_json(record.location)},
            // "{}" is how an error says it has no details.
            {"details", object_or_text(record.details_json, false)}};
}

json status_snapshot_json(const StatusSnapshot& snapshot) {
    json steps_json = json::array();
    for (const StepStatus& step : snapshot.steps) {
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
    const json workflow = {
        {"name", snapshot.workflow_name},
        {"version", snapshot.workflow_version},
        {"fingerprint", snapshot.fingerprint},
        {"state", praenora::to_string(snapshot.state)},
        {"health", praenora::to_string(snapshot.health)},
        {"started_at", instant(snapshot.started_at_unix_ns)},
        {"uptime_ns", snapshot.uptime_ns},
        {"ticks_completed", snapshot.ticks_completed},
        {"current_tick", snapshot.current_tick ? json(*snapshot.current_tick) : json(nullptr)},
        {"logical_time", snapshot.logical_time ? json(*snapshot.logical_time) : json(nullptr)},
        {"last_tick_duration_ns", snapshot.last_tick_duration_ns},
        {"last_error", error_record(snapshot.last_error)}};
    const VersionInfo versions = version_info();
    json generated_at = instant(snapshot.generated_at_unix_ns);
    if (generated_at.is_null()) {
        generated_at = format_rfc3339_utc(0); // required: never null
    }
    return {{"kind", "status-snapshot"},
            {"format_version", versions.formats.at("status-snapshot")},
            {"generated_at", std::move(generated_at)},
            {"runtime_version", snapshot.runtime_version},
            {"pid", snapshot.pid},
            {"workflow", workflow},
            {"steps", std::move(steps_json)}};
}

} // namespace detail

std::string StatusSnapshot::to_json() const {
    // Messages may come from modules: bytes that are not UTF-8 are replaced, not refused.
    return detail::status_snapshot_json(*this).dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace praenora
