#include "prometheus.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace praenora::server {

namespace {

using nlohmann::json;

// The metric families, in the order the text lists them.
enum Family : std::size_t {
    step_invocations,
    step_duration,
    step_last_duration,
    workflow_ticks,
    workflow_tick_duration,
    workflow_uptime,
    family_count
};

struct FamilyInfo {
    const char* name;
    const char* type;
    const char* help;
};

constexpr std::array<FamilyInfo, family_count> kFamilies = {{
    {"praenora_step_invocations_total", "counter",
     "The invocations of a step's module, by outcome."},
    {"praenora_step_invocation_duration_nanoseconds", "summary",
     "The durations of a step's invocations: the minimum (quantile 0), the maximum (quantile 1), "
     "their sum and their count."},
    {"praenora_step_invocation_last_duration_nanoseconds", "gauge",
     "The duration of a step's last invocation."},
    {"praenora_workflow_ticks_total", "counter", "The ticks the run completed."},
    {"praenora_workflow_tick_duration_nanoseconds", "gauge",
     "The duration of the run's last tick."},
    {"praenora_workflow_uptime_nanoseconds", "gauge",
     "The time since the run started, frozen when it ends."},
}};

// A label name: every character but A-Z a-z 0-9 _ replaced by '_'.
std::string label_name(const std::string& key) {
    std::string name = key;
    for (char& c : name) {
        const bool allowed =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            c = '_';
        }
    }
    return name;
}

// A label value with '\', '"' and the line feed escaped.
std::string label_value(const std::string& value) {
    std::string escaped;
    for (const char c : value) {
        if (c == '\\') {
            escaped += "\\\\";
        } else if (c == '"') {
            escaped += "\\\"";
        } else if (c == '\n') {
            escaped += "\\n";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

// HELP text with '\' and the line feed escaped.
std::string help_text(const std::string& text) {
    std::string escaped;
    for (const char c : text) {
        if (c == '\\') {
            escaped += "\\\\";
        } else if (c == '\n') {
            escaped += "\\n";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

// A sample's value: integers as they are, other numbers as JSON writes them; nullopt for anything
// that is not a number.
std::optional<std::string> sample_value(const json& value) {
    if (!value.is_number()) {
        return std::nullopt;
    }
    return value.dump();
}

// The labels of a point: its attributes (string values only), then the run's.
std::optional<std::string> point_labels(const json& point, const RunMetrics& run) {
    const auto attributes = point.find("attributes");
    if (attributes == point.end() || !attributes->is_object()) {
        return std::nullopt;
    }
    std::string labels;
    for (const auto& item : attributes->items()) {
        if (!item.value().is_string()) {
            return std::nullopt;
        }
        labels +=
            label_name(item.key()) + "=\"" + label_value(item.value().get<std::string>()) + "\",";
    }
    labels += "praenora_run_number=\"" + std::to_string(run.run_number) +
              "\",praenora_run_fingerprint=\"" + label_value(run.fingerprint) + "\"";
    return labels;
}

std::string sample(const char* name, const char* suffix, const std::string& labels,
                   const std::string& value) {
    return std::string(name) + suffix + "{" + labels + "} " + value + "\n";
}

// Appends the samples of `point` to the families they belong to.
void add_point(const json& point, const RunMetrics& run,
               std::array<std::string, family_count>& samples) {
    if (!point.is_object()) {
        return;
    }
    const auto name = point.find("name");
    if (name == point.end() || !name->is_string()) {
        return;
    }
    const std::optional<std::string> labels = point_labels(point, run);
    if (!labels) {
        return;
    }
    const std::string& metric = name->get_ref<const std::string&>();
    const auto value_of = [&](const json& object, const char* key) -> std::optional<std::string> {
        const auto found = object.find(key);
        if (found == object.end()) {
            return std::nullopt;
        }
        return sample_value(*found);
    };
    const auto simple = [&](Family family) {
        if (const auto value = value_of(point, "value")) {
            samples[family] += sample(kFamilies[family].name, "", *labels, *value);
        }
    };
    if (metric == "praenora.step.invocations") {
        simple(step_invocations);
    } else if (metric == "praenora.workflow.ticks") {
        simple(workflow_ticks);
    } else if (metric == "praenora.workflow.tick.duration") {
        simple(workflow_tick_duration);
    } else if (metric == "praenora.workflow.uptime") {
        simple(workflow_uptime);
    } else if (metric == "praenora.step.invocation.duration") {
        const auto summary = point.find("summary");
        if (summary == point.end() || !summary->is_object()) {
            return;
        }
        const auto count = value_of(*summary, "count");
        const auto sum = value_of(*summary, "sum");
        const auto min = value_of(*summary, "min");
        const auto max = value_of(*summary, "max");
        const auto last = value_of(*summary, "last");
        if (!count || !sum || !min || !max || !last) {
            return;
        }
        const char* family = kFamilies[step_duration].name;
        std::string& text = samples[step_duration];
        // Quantiles of no observation are left out.
        if (*count != "0") {
            text += sample(family, "", *labels + ",quantile=\"0\"", *min);
            text += sample(family, "", *labels + ",quantile=\"1\"", *max);
        }
        text += sample(family, "_sum", *labels, *sum);
        text += sample(family, "_count", *labels, *count);
        samples[step_last_duration] +=
            sample(kFamilies[step_last_duration].name, "", *labels, *last);
    }
}

} // namespace

std::string prometheus_text(const std::vector<RunMetrics>& runs) {
    std::array<std::string, family_count> samples;
    for (const RunMetrics& run : runs) {
        const json document = json::parse(run.metrics_json, nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            continue;
        }
        const auto points = document.find("metrics");
        if (points == document.end() || !points->is_array()) {
            continue;
        }
        for (const json& point : *points) {
            add_point(point, run, samples);
        }
    }
    std::string text;
    for (std::size_t family = 0; family < family_count; ++family) {
        if (samples[family].empty()) {
            continue;
        }
        text += std::string("# HELP ") + kFamilies[family].name + " " +
                help_text(kFamilies[family].help) + "\n";
        text +=
            std::string("# TYPE ") + kFamilies[family].name + " " + kFamilies[family].type + "\n";
        text += samples[family];
    }
    return text;
}

} // namespace praenora::server
