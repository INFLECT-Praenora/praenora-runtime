// The JSON encoding of OTLP export requests (observability/otlp/otlp_json.hpp), against golden
// documents: the metrics of a fixed snapshot (sums, summaries and gauges, aggregation temporality
// 2, 64-bit values as strings, quantiles, the last duration, the run's attributes, the resource
// and the exporter's own counters), runs that share metrics, and log records (a scope per logger,
// severities, every kind of attribute value, null, the run's attributes, bytes that are not UTF-8
// replaced).
#include "observability/metrics.hpp"
#include "observability/otlp/otlp_json.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using nlohmann::json;
using praenora::LogLevel;
using praenora::StatusSnapshot;
using praenora::StepStatus;
using praenora::detail::encode_otlp_logs;
using praenora::detail::encode_otlp_metrics;
using praenora::detail::LogAttribute;
using praenora::detail::otlp_any_value;
using praenora::detail::OtlpExporterCounters;
using praenora::detail::OtlpLogRecord;
using praenora::detail::OtlpRunMetrics;

const std::vector<LogAttribute> kResource = {
    {"service.name", "praenora-runtime"},
    {"service.version", "0.2.0"},
    {"service.instance.id", "00000000-0000-4000-8000-000000000000"},
    {"process.pid", 4242},
};

// A run of one step, estimate, after 3 ticks: 2 invocations succeeded and 1 failed, durations
// 100, 300 and 200 ns.
StatusSnapshot fixed_snapshot() {
    StatusSnapshot snapshot;
    snapshot.workflow_name = "example.estimate-predict";
    snapshot.workflow_version = "0.1.0";
    snapshot.ticks_completed = 3;
    snapshot.last_tick_duration_ns = 250;
    snapshot.uptime_ns = 1000;
    StepStatus step;
    step.step = "estimate";
    step.module_name = "example-estimator";
    step.module_version = "0.1.0";
    step.invocations = 3;
    step.succeeded = 2;
    step.failed = 1;
    step.total_duration_ns = 600;
    step.min_duration_ns = 100;
    step.max_duration_ns = 300;
    step.last_duration_ns = 200;
    snapshot.steps.push_back(step);
    return snapshot;
}

OtlpRunMetrics run_metrics(const StatusSnapshot& snapshot, std::uint64_t number) {
    OtlpRunMetrics run;
    run.run_number = number;
    run.fingerprint = "fingerprint-" + std::to_string(number);
    run.start_unix_ns = 1000000000;
    run.time_unix_ns = 2000000000;
    run.metrics_json = praenora::detail::metrics_json(snapshot);
    return run;
}

// The compact form of a document written out for reading.
std::string canonical(const char* text) { return json::parse(text).dump(); }

} // namespace

TEST(OtlpJson, MetricsOfAFixedSnapshot) {
    OtlpExporterCounters counters;
    counters.start_unix_ns = 500;
    counters.time_unix_ns = 2000000001;
    counters.metrics_failures = 1;
    counters.logs_failures = 2;
    counters.dropped_log_records = 3;
    const std::string encoded =
        encode_otlp_metrics(kResource, "0.2.0", {run_metrics(fixed_snapshot(), 7)}, counters);
    EXPECT_EQ(encoded, canonical(R"json({
      "resourceMetrics": [{
        "resource": {"attributes": [
          {"key": "service.name", "value": {"stringValue": "praenora-runtime"}},
          {"key": "service.version", "value": {"stringValue": "0.2.0"}},
          {"key": "service.instance.id",
           "value": {"stringValue": "00000000-0000-4000-8000-000000000000"}},
          {"key": "process.pid", "value": {"intValue": "4242"}}]},
        "scopeMetrics": [{
          "scope": {"name": "praenora.runtime", "version": "0.2.0"},
          "metrics": [
            {"name": "praenora.step.invocations", "unit": "{invocation}",
             "sum": {"aggregationTemporality": 2, "isMonotonic": true, "dataPoints": [
               {"attributes": [
                  {"key": "praenora.module", "value": {"stringValue": "example-estimator@0.1.0"}},
                  {"key": "praenora.outcome", "value": {"stringValue": "succeeded"}},
                  {"key": "praenora.step", "value": {"stringValue": "estimate"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "startTimeUnixNano": "1000000000", "timeUnixNano": "2000000000", "asInt": "2"},
               {"attributes": [
                  {"key": "praenora.module", "value": {"stringValue": "example-estimator@0.1.0"}},
                  {"key": "praenora.outcome", "value": {"stringValue": "failed"}},
                  {"key": "praenora.step", "value": {"stringValue": "estimate"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "startTimeUnixNano": "1000000000", "timeUnixNano": "2000000000", "asInt": "1"},
               {"attributes": [
                  {"key": "praenora.module", "value": {"stringValue": "example-estimator@0.1.0"}},
                  {"key": "praenora.outcome", "value": {"stringValue": "timed_out"}},
                  {"key": "praenora.step", "value": {"stringValue": "estimate"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "startTimeUnixNano": "1000000000", "timeUnixNano": "2000000000", "asInt": "0"},
               {"attributes": [
                  {"key": "praenora.module", "value": {"stringValue": "example-estimator@0.1.0"}},
                  {"key": "praenora.outcome", "value": {"stringValue": "cancelled"}},
                  {"key": "praenora.step", "value": {"stringValue": "estimate"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "startTimeUnixNano": "1000000000", "timeUnixNano": "2000000000", "asInt": "0"}]}},
            {"name": "praenora.step.invocation.duration", "unit": "ns",
             "summary": {"dataPoints": [
               {"attributes": [
                  {"key": "praenora.module", "value": {"stringValue": "example-estimator@0.1.0"}},
                  {"key": "praenora.step", "value": {"stringValue": "estimate"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "startTimeUnixNano": "1000000000", "timeUnixNano": "2000000000",
                "count": "3", "sum": 600.0,
                "quantileValues": [{"quantile": 0.0, "value": 100.0},
                                   {"quantile": 1.0, "value": 300.0}]}]}},
            {"name": "praenora.step.invocation.last_duration", "unit": "ns",
             "gauge": {"dataPoints": [
               {"attributes": [
                  {"key": "praenora.module", "value": {"stringValue": "example-estimator@0.1.0"}},
                  {"key": "praenora.step", "value": {"stringValue": "estimate"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "timeUnixNano": "2000000000", "asInt": "200"}]}},
            {"name": "praenora.workflow.ticks", "unit": "{tick}",
             "sum": {"aggregationTemporality": 2, "isMonotonic": true, "dataPoints": [
               {"attributes": [
                  {"key": "praenora.workflow",
                   "value": {"stringValue": "example.estimate-predict@0.1.0"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "startTimeUnixNano": "1000000000", "timeUnixNano": "2000000000", "asInt": "3"}]}},
            {"name": "praenora.workflow.tick.duration", "unit": "ns",
             "gauge": {"dataPoints": [
               {"attributes": [
                  {"key": "praenora.workflow",
                   "value": {"stringValue": "example.estimate-predict@0.1.0"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "timeUnixNano": "2000000000", "asInt": "250"}]}},
            {"name": "praenora.workflow.uptime", "unit": "ns",
             "gauge": {"dataPoints": [
               {"attributes": [
                  {"key": "praenora.workflow",
                   "value": {"stringValue": "example.estimate-predict@0.1.0"}},
                  {"key": "praenora.run.number", "value": {"intValue": "7"}},
                  {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint-7"}}],
                "timeUnixNano": "2000000000", "asInt": "1000"}]}},
            {"name": "praenora.telemetry.exports.failed", "unit": "{export}",
             "sum": {"aggregationTemporality": 2, "isMonotonic": true, "dataPoints": [
               {"attributes": [{"key": "praenora.signal", "value": {"stringValue": "metrics"}}],
                "startTimeUnixNano": "500", "timeUnixNano": "2000000001", "asInt": "1"},
               {"attributes": [{"key": "praenora.signal", "value": {"stringValue": "logs"}}],
                "startTimeUnixNano": "500", "timeUnixNano": "2000000001", "asInt": "2"}]}},
            {"name": "praenora.telemetry.log_records.dropped", "unit": "{record}",
             "sum": {"aggregationTemporality": 2, "isMonotonic": true, "dataPoints": [
               {"attributes": [],
                "startTimeUnixNano": "500", "timeUnixNano": "2000000001", "asInt": "3"}]}}]}]}]
    })json"));
}

TEST(OtlpJson, RunsShareTheirMetricsAndASummaryOfNoInvocationsHasNoQuantiles) {
    StatusSnapshot idle = fixed_snapshot();
    idle.steps[0] = StepStatus{};
    idle.steps[0].step = "estimate";
    idle.steps[0].module_name = "example-estimator";
    idle.steps[0].module_version = "0.1.0";
    const json document = json::parse(encode_otlp_metrics(
        kResource, "0.2.0", {run_metrics(fixed_snapshot(), 1), run_metrics(idle, 2)},
        std::nullopt));
    const json& metrics =
        document.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics");
    std::vector<std::string> names;
    for (const json& metric : metrics) {
        names.push_back(metric.at("name"));
    }
    EXPECT_EQ(names, (std::vector<std::string>{
                         "praenora.step.invocations", "praenora.step.invocation.duration",
                         "praenora.step.invocation.last_duration", "praenora.workflow.ticks",
                         "praenora.workflow.tick.duration", "praenora.workflow.uptime"}));
    EXPECT_EQ(metrics.at(0).at("sum").at("dataPoints").size(), 8u);
    const json& summaries = metrics.at(1).at("summary").at("dataPoints");
    ASSERT_EQ(summaries.size(), 2u);
    EXPECT_TRUE(summaries.at(0).contains("quantileValues"));
    EXPECT_FALSE(summaries.at(1).contains("quantileValues"));
    EXPECT_EQ(summaries.at(1).at("count"), "0");
    EXPECT_EQ(summaries.at(1).at("attributes").at(2).at("value").at("intValue"), "2");
    const json& last = metrics.at(2).at("gauge").at("dataPoints");
    ASSERT_EQ(last.size(), 2u);
    EXPECT_EQ(last.at(1).at("asInt"), "0");
}

TEST(OtlpJson, AMetricsDocumentThatIsNotOneAddsNothing) {
    OtlpRunMetrics broken = run_metrics(fixed_snapshot(), 1);
    broken.metrics_json = "not json";
    const json document =
        json::parse(encode_otlp_metrics(kResource, "0.2.0", {broken}, std::nullopt));
    EXPECT_TRUE(
        document.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics").empty());
}

TEST(OtlpJson, LogsOfAFixedBatch) {
    std::vector<OtlpLogRecord> records(4);
    records[0].time_unix_ns = 1000;
    records[0].level = LogLevel::info;
    records[0].logger = "praenora.engine";
    records[0].message = "run started";
    records[0].attributes = {{"praenora.run.fingerprint", "fp"},
                             {"praenora.event", "run.started"},
                             {"praenora.workflow", "w@0.1.0"}};
    records[0].run_number = 1;
    records[0].run_fingerprint = "fp";
    records[1].time_unix_ns = 2000;
    records[1].level = LogLevel::debug;
    records[1].logger = "module.estimate";
    records[1].message = "values";
    records[1].attributes = {{"praenora.tick", 3},
                             {"praenora.ratio", 0.5},
                             {"praenora.big", std::numeric_limits<std::uint64_t>::max()},
                             {"praenora.flag", true},
                             {"praenora.none", nullptr},
                             {"praenora.list", json::array({1, "x"})},
                             {"praenora.object", json{{"b", 1}, {"a", "z"}}},
                             {"praenora.nan", std::nan("")},
                             {"logger", "left out"}};
    records[1].run_number = 1;
    records[1].run_fingerprint = "fp";
    records[2].time_unix_ns = 3000;
    records[2].level = LogLevel::warn;
    records[2].logger = "praenora.engine";
    records[2].message = "late";
    records[2].run_number = 1;
    records[2].run_fingerprint = "fp";
    records[3].time_unix_ns = 4000;
    records[3].level = LogLevel::critical;
    records[3].logger = "praenora.runtime";
    records[3].message = "the runtime's own";
    EXPECT_EQ(encode_otlp_logs(kResource, "0.2.0", records), canonical(R"json({
      "resourceLogs": [{
        "resource": {"attributes": [
          {"key": "service.name", "value": {"stringValue": "praenora-runtime"}},
          {"key": "service.version", "value": {"stringValue": "0.2.0"}},
          {"key": "service.instance.id",
           "value": {"stringValue": "00000000-0000-4000-8000-000000000000"}},
          {"key": "process.pid", "value": {"intValue": "4242"}}]},
        "scopeLogs": [
          {"scope": {"name": "praenora.engine", "version": "0.2.0"},
           "logRecords": [
             {"timeUnixNano": "1000", "observedTimeUnixNano": "1000",
              "severityNumber": 9, "severityText": "INFO", "body": {"stringValue": "run started"},
              "attributes": [
                {"key": "praenora.run.fingerprint", "value": {"stringValue": "fp"}},
                {"key": "praenora.event", "value": {"stringValue": "run.started"}},
                {"key": "praenora.workflow", "value": {"stringValue": "w@0.1.0"}},
                {"key": "praenora.run.number", "value": {"intValue": "1"}}]},
             {"timeUnixNano": "3000", "observedTimeUnixNano": "3000",
              "severityNumber": 13, "severityText": "WARN", "body": {"stringValue": "late"},
              "attributes": [
                {"key": "praenora.run.number", "value": {"intValue": "1"}},
                {"key": "praenora.run.fingerprint", "value": {"stringValue": "fp"}}]}]},
          {"scope": {"name": "module.estimate", "version": "0.2.0"},
           "logRecords": [
             {"timeUnixNano": "2000", "observedTimeUnixNano": "2000",
              "severityNumber": 5, "severityText": "DEBUG", "body": {"stringValue": "values"},
              "attributes": [
                {"key": "praenora.tick", "value": {"intValue": "3"}},
                {"key": "praenora.ratio", "value": {"doubleValue": 0.5}},
                {"key": "praenora.big", "value": {"stringValue": "18446744073709551615"}},
                {"key": "praenora.flag", "value": {"boolValue": true}},
                {"key": "praenora.none", "value": {}},
                {"key": "praenora.list", "value": {"arrayValue": {"values": [
                  {"intValue": "1"}, {"stringValue": "x"}]}}},
                {"key": "praenora.object", "value": {"kvlistValue": {"values": [
                  {"key": "a", "value": {"stringValue": "z"}},
                  {"key": "b", "value": {"intValue": "1"}}]}}},
                {"key": "praenora.nan", "value": {"doubleValue": "NaN"}},
                {"key": "praenora.run.number", "value": {"intValue": "1"}},
                {"key": "praenora.run.fingerprint", "value": {"stringValue": "fp"}}]}]},
          {"scope": {"name": "praenora.runtime", "version": "0.2.0"},
           "logRecords": [
             {"timeUnixNano": "4000", "observedTimeUnixNano": "4000",
              "severityNumber": 21, "severityText": "CRITICAL",
              "body": {"stringValue": "the runtime's own"}, "attributes": []}]}]}]
    })json"));
}

TEST(OtlpJson, SeveritiesOfEveryLevel) {
    const std::pair<LogLevel, std::pair<int, const char*>> levels[] = {
        {LogLevel::trace, {1, "TRACE"}},  {LogLevel::debug, {5, "DEBUG"}},
        {LogLevel::info, {9, "INFO"}},    {LogLevel::warn, {13, "WARN"}},
        {LogLevel::error, {17, "ERROR"}}, {LogLevel::critical, {21, "CRITICAL"}}};
    for (const auto& [level, severity] : levels) {
        OtlpLogRecord record;
        record.level = level;
        record.logger = "l";
        const json document = json::parse(encode_otlp_logs(kResource, "0.2.0", {record}));
        const json& sent =
            document.at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords").at(0);
        EXPECT_EQ(sent.at("severityNumber"), severity.first);
        EXPECT_EQ(sent.at("severityText"), severity.second);
    }
}

TEST(OtlpJson, BytesThatAreNotUtf8AreReplaced) {
    OtlpLogRecord record;
    record.logger = "module.scribble";
    record.message = std::string("a\xff\xfe"
                                 "b");
    record.attributes = {{"praenora.text", std::string("c\xc3")}};
    const std::string encoded = encode_otlp_logs(kResource, "0.2.0", {record});
    json document;
    ASSERT_NO_THROW(document = json::parse(encoded)) << "not valid UTF-8 JSON";
    const json& sent =
        document.at("resourceLogs").at(0).at("scopeLogs").at(0).at("logRecords").at(0);
    const std::string body = sent.at("body").at("stringValue");
    EXPECT_EQ(body.front(), 'a');
    EXPECT_EQ(body.back(), 'b');
    EXPECT_NE(body.find("\xEF\xBF\xBD"), std::string::npos) << body;
    const std::string text = sent.at("attributes").at(0).at("value").at("stringValue");
    EXPECT_NE(text.find("\xEF\xBF\xBD"), std::string::npos) << text;
}

TEST(OtlpJson, AnyValueKinds) {
    EXPECT_EQ(otlp_any_value(nullptr), json::object());
    EXPECT_EQ(otlp_any_value(-3), (json{{"intValue", "-3"}}));
    EXPECT_EQ(otlp_any_value(std::numeric_limits<std::int64_t>::min()),
              (json{{"intValue", "-9223372036854775808"}}));
    EXPECT_EQ(otlp_any_value(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())),
              (json{{"intValue", "9223372036854775807"}}));
    EXPECT_EQ(otlp_any_value(std::numeric_limits<double>::infinity()),
              (json{{"doubleValue", "Infinity"}}));
    EXPECT_EQ(otlp_any_value(-std::numeric_limits<double>::infinity()),
              (json{{"doubleValue", "-Infinity"}}));
    EXPECT_EQ(otlp_any_value(json::array()), (json{{"arrayValue", {{"values", json::array()}}}}));
    EXPECT_EQ(otlp_any_value(json::object()), (json{{"kvlistValue", {{"values", json::array()}}}}));
    EXPECT_EQ(otlp_any_value(false), (json{{"boolValue", false}}));
}
