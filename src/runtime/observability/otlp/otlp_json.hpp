// The JSON encoding of OTLP/HTTP export requests (docs/specs/observability.md#otlp-export): the
// metrics of runs, as Workflow::metrics_json() gives them, and log records. Pure functions; the
// output is canonical JSON (nlohmann dump() without indentation), with bytes that are not UTF-8
// replaced, lowerCamelCase field names, 64-bit integers as decimal strings and enumerations as
// integers.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_OTLP_OTLP_JSON_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_OTLP_OTLP_JSON_HPP

#include "observability/logging.hpp"

#include <praenora/runtime/options.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// The metrics of one run at one moment.
struct OtlpRunMetrics {
    std::uint64_t run_number = 0;
    std::string fingerprint;
    std::int64_t start_unix_ns = 0; // when cumulative points start: the run's start, else its open
    std::int64_t time_unix_ns = 0;  // the snapshot's generated_at
    std::string metrics_json;       // the snapshot's metrics_json() document
};

// The exporter's own counters, exported with the runs' metrics (scope praenora.runtime, without
// run attributes): failed exports of the signals that are exported, and dropped log records.
struct OtlpExporterCounters {
    std::int64_t start_unix_ns = 0; // when the exporter was created
    std::int64_t time_unix_ns = 0;
    std::optional<std::uint64_t> metrics_failures, logs_failures; // a point for each one present
    std::uint64_t dropped_log_records = 0;
};

// An ExportMetricsServiceRequest: one resource (`resource`, in order), one scope praenora.runtime
// of version `scope_version`, and its metrics, each name once, in the order of their first point:
//   praenora.step.invocations and praenora.workflow.ticks: sums, monotonic and cumulative
//     (aggregationTemporality 2), asInt;
//   praenora.step.invocation.duration: summaries with count, sum and, when the count is not 0,
//     quantileValues 0 (min) and 1 (max);
//   praenora.step.invocation.last_duration (unit ns): a gauge of each summary's last, asInt;
//   praenora.workflow.tick.duration and praenora.workflow.uptime: gauges, asInt;
// then, with `counters`, praenora.telemetry.exports.failed (unit {export}, attribute
// praenora.signal metrics or logs) and praenora.telemetry.log_records.dropped (unit {record}),
// cumulative sums. A run's points carry their attributes (in the order of metrics_json(), which
// sorts them), then praenora.run.number and praenora.run.fingerprint; sums and summaries start at
// start_unix_ns, and every point is stamped time_unix_ns. A run whose metrics_json is not a
// metrics document adds nothing, and so does a point of another name or without its fields.
std::string encode_otlp_metrics(const std::vector<LogAttribute>& resource,
                                const std::string& scope_version,
                                const std::vector<OtlpRunMetrics>& runs,
                                const std::optional<OtlpExporterCounters>& counters);

// One log record as the exporter sends it.
struct OtlpLogRecord {
    std::int64_t time_unix_ns = 0;
    LogLevel level = LogLevel::info;
    std::string logger; // the scope's name
    std::string message;
    std::vector<LogAttribute> attributes; // the event's, in order
    // The run's attributes, added after the event's own: praenora.run.number (when not 0) and
    // praenora.run.fingerprint (when not empty, and the event has none of its own).
    std::uint64_t run_number = 0;
    std::string run_fingerprint;
};

// An ExportLogsServiceRequest: one resource, one scope per logger (named after it, of version
// `scope_version`), in the order of their first record, and each scope's records in order, with
// timeUnixNano and observedTimeUnixNano the record's time, severityNumber and severityText those
// of the JSON lines (logging.hpp), body.stringValue the message and the attributes as
// OtlpLogRecord says. The attribute "logger" is left out: the scope names it.
std::string encode_otlp_logs(const std::vector<LogAttribute>& resource,
                             const std::string& scope_version,
                             const std::vector<OtlpLogRecord>& records);

// An AnyValue of OTLP's JSON encoding for `value`: stringValue, boolValue, intValue (a decimal
// string; an unsigned value beyond the range of a signed 64-bit integer becomes a stringValue),
// doubleValue (NaN and the infinities as the strings "NaN", "Infinity" and "-Infinity"),
// arrayValue {values}, kvlistValue {values: [{key, value}]}; null is the empty value {}.
nlohmann::json otlp_any_value(const nlohmann::json& value);

// OTLP's KeyValue list for `attributes`, in order.
nlohmann::json otlp_attributes(const std::vector<LogAttribute>& attributes);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_OTLP_OTLP_JSON_HPP
