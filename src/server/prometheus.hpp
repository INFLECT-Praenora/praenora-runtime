// The Prometheus text of GET /metrics: the metric points of every run the server holds
// (Workflow::metrics_json()) in the text exposition format 0.0.4.
#ifndef PRAENORA_SERVER_PROMETHEUS_HPP
#define PRAENORA_SERVER_PROMETHEUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::server {

inline constexpr std::string_view kPrometheusContentType =
    "text/plain; version=0.0.4; charset=utf-8";

// One run's metrics: its run number and fingerprint, which label each of its samples, and its
// metrics document, Workflow::metrics_json().
struct RunMetrics {
    std::uint64_t run_number = 0;
    std::string fingerprint;
    std::string metrics_json;
};

// The metric families of `runs`, in a fixed order, each with one # HELP and one # TYPE line and
// then the samples of every run in the order given:
//   praenora.step.invocations          praenora_step_invocations_total (counter)
//   praenora.step.invocation.duration  praenora_step_invocation_duration_nanoseconds (summary:
//                                      {quantile="0"} the minimum and {quantile="1"} the maximum,
//                                      left out at count 0, then _sum and _count)
//   its "last"                         praenora_step_invocation_last_duration_nanoseconds (gauge)
//   praenora.workflow.ticks            praenora_workflow_ticks_total (counter)
//   praenora.workflow.tick.duration    praenora_workflow_tick_duration_nanoseconds (gauge)
//   praenora.workflow.uptime           praenora_workflow_uptime_nanoseconds (gauge)
// A sample's labels are its point's attributes, each key with every character but A-Z a-z 0-9 _
// replaced by '_' (praenora.step is praenora_step), then praenora_run_number and
// praenora_run_fingerprint (and quantile); label values escape '\', '"' and the line feed. A
// family without samples is left out, and so are points of other names or shapes and documents
// that are not JSON. Every line ends in "\n".
std::string prometheus_text(const std::vector<RunMetrics>& runs);

} // namespace praenora::server

#endif // PRAENORA_SERVER_PROMETHEUS_HPP
