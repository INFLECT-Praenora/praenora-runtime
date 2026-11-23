#include "observability/otlp/otlp_json.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

namespace praenora::detail {

using nlohmann::json;

namespace {

json key_value(const std::string& key, json value) {
    json entry = json::object();
    entry["key"] = key;
    entry["value"] = std::move(value);
    return entry;
}

json string_value(const std::string& text) {
    json value = json::object();
    value["stringValue"] = text;
    return value;
}

json int_value(std::uint64_t number) {
    json value = json::object();
    value["intValue"] = std::to_string(number);
    return value;
}

// The decimal text of an integer value of a metrics document, or nothing.
std::optional<std::string> integer_text(const json& value) {
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    return std::nullopt;
}

std::optional<double> number_of(const json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    return std::nullopt;
}

// The metrics of one request, each name once, in the order of their first point.
class Metrics {
public:
    enum class Kind { sum, gauge, summary };

    json& points(const std::string& name, const std::string& unit, Kind kind) {
        const auto found = index_.find(name);
        if (found != index_.end()) {
            return metrics_[found->second].points;
        }
        index_.emplace(name, metrics_.size());
        metrics_.push_back({name, unit, kind, json::array()});
        return metrics_.back().points;
    }

    json to_json() const {
        json list = json::array();
        for (const Metric& metric : metrics_) {
            json entry = json::object();
            entry["name"] = metric.name;
            entry["unit"] = metric.unit;
            json data = json::object();
            if (metric.kind == Kind::sum) {
                data["aggregationTemporality"] = 2; // cumulative
                data["isMonotonic"] = true;
            }
            data["dataPoints"] = metric.points;
            entry[metric.kind == Kind::sum     ? "sum"
                  : metric.kind == Kind::gauge ? "gauge"
                                               : "summary"] = std::move(data);
            list.push_back(std::move(entry));
        }
        return list;
    }

private:
    struct Metric {
        std::string name, unit;
        Kind kind;
        json points;
    };
    std::vector<Metric> metrics_;
    std::map<std::string, std::size_t> index_;
};

json data_point(json attributes, const std::optional<std::int64_t>& start, std::int64_t time) {
    json point = json::object();
    point["attributes"] = std::move(attributes);
    if (start) {
        point["startTimeUnixNano"] = std::to_string(*start);
    }
    point["timeUnixNano"] = std::to_string(time);
    return point;
}

// One run's points of `document` (a metrics_json() document) added to `metrics`.
void add_run(Metrics& metrics, const json& document, const OtlpRunMetrics& run) {
    if (!document.is_object() || !document.contains("metrics") ||
        !document.at("metrics").is_array()) {
        return;
    }
    for (const json& point : document.at("metrics")) {
        if (!point.is_object() || !point.contains("name") || !point.at("name").is_string()) {
            continue;
        }
        const std::string name = point.at("name");
        const std::string unit = point.value("unit", "");
        const std::string kind = point.value("kind", "");
        json attributes = json::array();
        if (point.contains("attributes") && point.at("attributes").is_object()) {
            for (const auto& [key, value] : point.at("attributes").items()) {
                attributes.push_back(key_value(key, otlp_any_value(value)));
            }
        }
        attributes.push_back(key_value("praenora.run.number", int_value(run.run_number)));
        attributes.push_back(key_value("praenora.run.fingerprint", string_value(run.fingerprint)));

        if ((name == "praenora.step.invocations" || name == "praenora.workflow.ticks") &&
            kind == "counter") {
            const std::optional<std::string> value = integer_text(point.value("value", json()));
            if (!value) {
                continue;
            }
            json sum = data_point(std::move(attributes), run.start_unix_ns, run.time_unix_ns);
            sum["asInt"] = *value;
            metrics.points(name, unit, Metrics::Kind::sum).push_back(std::move(sum));
        } else if ((name == "praenora.workflow.tick.duration" ||
                    name == "praenora.workflow.uptime") &&
                   kind == "gauge") {
            const std::optional<std::string> value = integer_text(point.value("value", json()));
            if (!value) {
                continue;
            }
            json gauge = data_point(std::move(attributes), std::nullopt, run.time_unix_ns);
            gauge["asInt"] = *value;
            metrics.points(name, unit, Metrics::Kind::gauge).push_back(std::move(gauge));
        } else if (name == "praenora.step.invocation.duration" && kind == "summary") {
            const json summary = point.value("summary", json());
            if (!summary.is_object()) {
                continue;
            }
            const std::optional<std::string> count = integer_text(summary.value("count", json()));
            const std::optional<double> total = number_of(summary.value("sum", json()));
            const std::optional<double> min = number_of(summary.value("min", json()));
            const std::optional<double> max = number_of(summary.value("max", json()));
            const std::optional<std::string> last = integer_text(summary.value("last", json()));
            if (!count || !total || !min || !max || !last) {
                continue;
            }
            json last_point = data_point(attributes, std::nullopt, run.time_unix_ns);
            last_point["asInt"] = *last;
            json summary_point =
                data_point(std::move(attributes), run.start_unix_ns, run.time_unix_ns);
            summary_point["count"] = *count;
            summary_point["sum"] = *total;
            if (*count != "0") {
                json quantiles = json::array();
                json lowest = json::object();
                lowest["quantile"] = 0.0;
                lowest["value"] = *min;
                json highest = json::object();
                highest["quantile"] = 1.0;
                highest["value"] = *max;
                quantiles.push_back(std::move(lowest));
                quantiles.push_back(std::move(highest));
                summary_point["quantileValues"] = std::move(quantiles);
            }
            metrics.points(name, unit, Metrics::Kind::summary).push_back(std::move(summary_point));
            metrics.points("praenora.step.invocation.last_duration", "ns", Metrics::Kind::gauge)
                .push_back(std::move(last_point));
        }
    }
}

std::string canonical(const json& document) {
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

json resource_json(const std::vector<LogAttribute>& resource) {
    json value = json::object();
    value["attributes"] = otlp_attributes(resource);
    return value;
}

json scope_json(const std::string& name, const std::string& version) {
    json scope = json::object();
    scope["name"] = name;
    scope["version"] = version;
    return scope;
}

} // namespace

json otlp_any_value(const json& value) {
    json any = json::object();
    switch (value.type()) {
    case json::value_t::boolean:
        any["boolValue"] = value.get<bool>();
        break;
    case json::value_t::number_integer:
        any["intValue"] = std::to_string(value.get<std::int64_t>());
        break;
    case json::value_t::number_unsigned: {
        const auto number = value.get<std::uint64_t>();
        constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        any[number <= kMax ? "intValue" : "stringValue"] = std::to_string(number);
        break;
    }
    case json::value_t::number_float: {
        const double number = value.get<double>();
        if (std::isnan(number)) {
            any["doubleValue"] = "NaN";
        } else if (std::isinf(number)) {
            any["doubleValue"] = number > 0 ? "Infinity" : "-Infinity";
        } else {
            any["doubleValue"] = number;
        }
        break;
    }
    case json::value_t::string:
        any["stringValue"] = value.get<std::string>();
        break;
    case json::value_t::array: {
        json values = json::array();
        for (const json& item : value) {
            values.push_back(otlp_any_value(item));
        }
        json array = json::object();
        array["values"] = std::move(values);
        any["arrayValue"] = std::move(array);
        break;
    }
    case json::value_t::object: {
        json values = json::array();
        for (const auto& [key, item] : value.items()) {
            values.push_back(key_value(key, otlp_any_value(item)));
        }
        json list = json::object();
        list["values"] = std::move(values);
        any["kvlistValue"] = std::move(list);
        break;
    }
    case json::value_t::null:
    case json::value_t::binary:
    case json::value_t::discarded:
        break;
    }
    return any;
}

json otlp_attributes(const std::vector<LogAttribute>& attributes) {
    json list = json::array();
    for (const LogAttribute& attribute : attributes) {
        list.push_back(key_value(attribute.key, otlp_any_value(attribute.value)));
    }
    return list;
}

std::string encode_otlp_metrics(const std::vector<LogAttribute>& resource,
                                const std::string& scope_version,
                                const std::vector<OtlpRunMetrics>& runs,
                                const std::optional<OtlpExporterCounters>& counters) {
    Metrics metrics;
    for (const OtlpRunMetrics& run : runs) {
        add_run(metrics, json::parse(run.metrics_json, nullptr, false), run);
    }
    if (counters) {
        const std::pair<const char*, const std::optional<std::uint64_t>*> failures[] = {
            {"metrics", &counters->metrics_failures}, {"logs", &counters->logs_failures}};
        for (const auto& [signal, count] : failures) {
            if (!*count) {
                continue;
            }
            json attributes = json::array();
            attributes.push_back(key_value("praenora.signal", string_value(signal)));
            json point =
                data_point(std::move(attributes), counters->start_unix_ns, counters->time_unix_ns);
            point["asInt"] = std::to_string(**count);
            metrics.points("praenora.telemetry.exports.failed", "{export}", Metrics::Kind::sum)
                .push_back(std::move(point));
        }
        json dropped = data_point(json::array(), counters->start_unix_ns, counters->time_unix_ns);
        dropped["asInt"] = std::to_string(counters->dropped_log_records);
        metrics.points("praenora.telemetry.log_records.dropped", "{record}", Metrics::Kind::sum)
            .push_back(std::move(dropped));
    }
    json scope_metrics = json::object();
    scope_metrics["scope"] = scope_json("praenora.runtime", scope_version);
    scope_metrics["metrics"] = metrics.to_json();
    json resource_metrics = json::object();
    resource_metrics["resource"] = resource_json(resource);
    resource_metrics["scopeMetrics"] = json::array({std::move(scope_metrics)});
    json document = json::object();
    document["resourceMetrics"] = json::array({std::move(resource_metrics)});
    return canonical(document);
}

std::string encode_otlp_logs(const std::vector<LogAttribute>& resource,
                             const std::string& scope_version,
                             const std::vector<OtlpLogRecord>& records) {
    std::vector<std::pair<std::string, json>> scopes; // logger, its records
    std::map<std::string, std::size_t> index;
    for (const OtlpLogRecord& record : records) {
        json attributes = json::array();
        bool has_number = false;
        bool has_fingerprint = false;
        for (const LogAttribute& attribute : record.attributes) {
            if (attribute.key == "logger") {
                continue;
            }
            has_number = has_number || attribute.key == "praenora.run.number";
            has_fingerprint = has_fingerprint || attribute.key == "praenora.run.fingerprint";
            attributes.push_back(key_value(attribute.key, otlp_any_value(attribute.value)));
        }
        if (record.run_number != 0 && !has_number) {
            attributes.push_back(key_value("praenora.run.number", int_value(record.run_number)));
        }
        if (!record.run_fingerprint.empty() && !has_fingerprint) {
            attributes.push_back(
                key_value("praenora.run.fingerprint", string_value(record.run_fingerprint)));
        }
        const LogSeverity severity = log_severity(record.level);
        json entry = json::object();
        entry["timeUnixNano"] = std::to_string(record.time_unix_ns);
        entry["observedTimeUnixNano"] = std::to_string(record.time_unix_ns);
        entry["severityNumber"] = severity.number;
        entry["severityText"] = std::string(severity.text);
        entry["body"] = string_value(record.message);
        entry["attributes"] = std::move(attributes);
        auto found = index.find(record.logger);
        if (found == index.end()) {
            found = index.emplace(record.logger, scopes.size()).first;
            scopes.emplace_back(record.logger, json::array());
        }
        scopes[found->second].second.push_back(std::move(entry));
    }
    json scope_logs = json::array();
    for (auto& [logger, logged] : scopes) {
        json scope = json::object();
        scope["scope"] = scope_json(logger, scope_version);
        scope["logRecords"] = std::move(logged);
        scope_logs.push_back(std::move(scope));
    }
    json resource_logs = json::object();
    resource_logs["resource"] = resource_json(resource);
    resource_logs["scopeLogs"] = std::move(scope_logs);
    json document = json::object();
    document["resourceLogs"] = json::array({std::move(resource_logs)});
    return canonical(document);
}

} // namespace praenora::detail
