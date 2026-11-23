// The Prometheus text of the control API's GET /metrics (src/server/prometheus.hpp): metric names,
// labels, escaping, the summary's quantiles, and what the encoder leaves out.
#include "prometheus.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using praenora::server::prometheus_text;
using praenora::server::RunMetrics;

namespace {

// The metrics document of a run with one step `step` of module `module` and workflow `workflow`,
// in the form of Workflow::metrics_json().
std::string metrics_document(const std::string& step, const std::string& module,
                             const std::string& workflow, int succeeded, int failed, int count,
                             int sum, int min, int max, int last, int ticks, int tick_duration,
                             int uptime) {
    const std::string labels =
        R"("praenora.module":")" + module + R"(","praenora.step":")" + step + R"(")";
    std::string points;
    const std::pair<const char*, int> outcomes[] = {
        {"succeeded", succeeded}, {"failed", failed}, {"timed_out", 0}, {"cancelled", 0}};
    for (const auto& [outcome, value] : outcomes) {
        points += R"({"attributes":{)" + labels + R"(,"praenora.outcome":")" + outcome +
                  R"("},"kind":"counter","name":"praenora.step.invocations","unit":"{invocation}",)"
                  R"("value":)" +
                  std::to_string(value) + "},";
    }
    points += R"({"attributes":{)" + labels +
              R"(},"kind":"summary","name":"praenora.step.invocation.duration","summary":{)"
              R"("count":)" +
              std::to_string(count) + R"(,"last":)" + std::to_string(last) + R"(,"max":)" +
              std::to_string(max) + R"(,"min":)" + std::to_string(min) + R"(,"sum":)" +
              std::to_string(sum) + R"(},"unit":"ns"},)";
    const std::string workflow_label = R"({"praenora.workflow":")" + workflow + R"("})";
    points += R"({"attributes":)" + workflow_label +
              R"(,"kind":"counter","name":"praenora.workflow.ticks","unit":"{tick}","value":)" +
              std::to_string(ticks) + "},";
    points += R"({"attributes":)" + workflow_label +
              R"(,"kind":"gauge","name":"praenora.workflow.tick.duration","unit":"ns","value":)" +
              std::to_string(tick_duration) + "},";
    points += R"({"attributes":)" + workflow_label +
              R"(,"kind":"gauge","name":"praenora.workflow.uptime","unit":"ns","value":)" +
              std::to_string(uptime) + "}";
    return R"({"metrics":[)" + points + "]}";
}

} // namespace

TEST(Prometheus, TextOfAFixedSnapshot) {
    std::vector<RunMetrics> runs;
    runs.push_back({1, "f1",
                    metrics_document("a", "fx-echo@0.1.0", "test.single@0.1.0", 2, 1, 3, 60, 10, 30,
                                     20, 3, 25, 1000)});
    // A step whose name needs escaping, and no invocation yet: no quantiles.
    runs.push_back(
        {12, "f2", metrics_document(R"(q\"x\\y\n)", "m@1", "w@1", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)});
    const std::string labels1 = R"(praenora_module="fx-echo@0.1.0",praenora_step="a",)"
                                R"(praenora_run_number="1",praenora_run_fingerprint="f1")";
    const std::string labels2 = R"(praenora_module="m@1",praenora_step="q\"x\\y\n",)"
                                R"(praenora_run_number="12",praenora_run_fingerprint="f2")";
    const std::string expected =
        "# HELP praenora_step_invocations_total The invocations of a step's module, by outcome.\n"
        "# TYPE praenora_step_invocations_total counter\n"
        R"(praenora_step_invocations_total{praenora_module="fx-echo@0.1.0",)"
        R"(praenora_outcome="succeeded",praenora_step="a",praenora_run_number="1",)"
        R"(praenora_run_fingerprint="f1"} 2)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="fx-echo@0.1.0",)"
        R"(praenora_outcome="failed",praenora_step="a",praenora_run_number="1",)"
        R"(praenora_run_fingerprint="f1"} 1)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="fx-echo@0.1.0",)"
        R"(praenora_outcome="timed_out",praenora_step="a",praenora_run_number="1",)"
        R"(praenora_run_fingerprint="f1"} 0)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="fx-echo@0.1.0",)"
        R"(praenora_outcome="cancelled",praenora_step="a",praenora_run_number="1",)"
        R"(praenora_run_fingerprint="f1"} 0)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="m@1",praenora_outcome="succeeded",)"
        R"(praenora_step="q\"x\\y\n",praenora_run_number="12",praenora_run_fingerprint="f2"} 0)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="m@1",praenora_outcome="failed",)"
        R"(praenora_step="q\"x\\y\n",praenora_run_number="12",praenora_run_fingerprint="f2"} 0)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="m@1",praenora_outcome="timed_out",)"
        R"(praenora_step="q\"x\\y\n",praenora_run_number="12",praenora_run_fingerprint="f2"} 0)"
        "\n"
        R"(praenora_step_invocations_total{praenora_module="m@1",praenora_outcome="cancelled",)"
        R"(praenora_step="q\"x\\y\n",praenora_run_number="12",praenora_run_fingerprint="f2"} 0)"
        "\n"
        "# HELP praenora_step_invocation_duration_nanoseconds The durations of a step's "
        "invocations: the minimum (quantile 0), the maximum (quantile 1), their sum and their "
        "count.\n"
        "# TYPE praenora_step_invocation_duration_nanoseconds summary\n"
        "praenora_step_invocation_duration_nanoseconds{" +
        labels1 + R"(,quantile="0"} 10)" + "\n" + "praenora_step_invocation_duration_nanoseconds{" +
        labels1 + R"(,quantile="1"} 30)" + "\n" +
        "praenora_step_invocation_duration_nanoseconds_sum{" + labels1 + "} 60\n" +
        "praenora_step_invocation_duration_nanoseconds_count{" + labels1 + "} 3\n" +
        "praenora_step_invocation_duration_nanoseconds_sum{" + labels2 + "} 0\n" +
        "praenora_step_invocation_duration_nanoseconds_count{" + labels2 + "} 0\n" +
        "# HELP praenora_step_invocation_last_duration_nanoseconds The duration of a step's last "
        "invocation.\n"
        "# TYPE praenora_step_invocation_last_duration_nanoseconds gauge\n"
        "praenora_step_invocation_last_duration_nanoseconds{" +
        labels1 + "} 20\n" + "praenora_step_invocation_last_duration_nanoseconds{" + labels2 +
        "} 0\n" +
        "# HELP praenora_workflow_ticks_total The ticks the run completed.\n"
        "# TYPE praenora_workflow_ticks_total counter\n"
        R"(praenora_workflow_ticks_total{praenora_workflow="test.single@0.1.0",)"
        R"(praenora_run_number="1",praenora_run_fingerprint="f1"} 3)"
        "\n"
        R"(praenora_workflow_ticks_total{praenora_workflow="w@1",praenora_run_number="12",)"
        R"(praenora_run_fingerprint="f2"} 0)"
        "\n"
        "# HELP praenora_workflow_tick_duration_nanoseconds The duration of the run's last "
        "tick.\n"
        "# TYPE praenora_workflow_tick_duration_nanoseconds gauge\n"
        R"(praenora_workflow_tick_duration_nanoseconds{praenora_workflow="test.single@0.1.0",)"
        R"(praenora_run_number="1",praenora_run_fingerprint="f1"} 25)"
        "\n"
        R"(praenora_workflow_tick_duration_nanoseconds{praenora_workflow="w@1",)"
        R"(praenora_run_number="12",praenora_run_fingerprint="f2"} 0)"
        "\n"
        "# HELP praenora_workflow_uptime_nanoseconds The time since the run started, frozen "
        "when it ends.\n"
        "# TYPE praenora_workflow_uptime_nanoseconds gauge\n"
        R"(praenora_workflow_uptime_nanoseconds{praenora_workflow="test.single@0.1.0",)"
        R"(praenora_run_number="1",praenora_run_fingerprint="f1"} 1000)"
        "\n"
        R"(praenora_workflow_uptime_nanoseconds{praenora_workflow="w@1",praenora_run_number="12",)"
        R"(praenora_run_fingerprint="f2"} 0)"
        "\n";
    EXPECT_EQ(prometheus_text(runs), expected);
}

TEST(Prometheus, NoRunsIsEmpty) { EXPECT_EQ(prometheus_text({}), ""); }

TEST(Prometheus, LeavesOutWhatItDoesNotKnow) {
    // A point of another name, a counter without a value, a summary without its fields, label
    // keys with characters that labels cannot hold, and a document that is not JSON.
    const std::vector<RunMetrics> runs = {
        {1, "f",
         R"({"metrics":[{"name":"praenora.other","kind":"gauge","unit":"1","attributes":{},)"
         R"("value":1},)"
         R"({"name":"praenora.workflow.ticks","kind":"counter","unit":"{tick}",)"
         R"("attributes":{"praenora.workflow":"w@1"}},)"
         R"({"name":"praenora.step.invocation.duration","kind":"summary","unit":"ns",)"
         R"("attributes":{"praenora.step":"a"},"summary":{"count":1}},)"
         R"({"name":"praenora.workflow.uptime","kind":"gauge","unit":"ns",)"
         R"("attributes":{"praenora.odd-key/x":"v"},"value":5}]})"},
        {2, "g", "not json"},
    };
    EXPECT_EQ(prometheus_text(runs),
              "# HELP praenora_workflow_uptime_nanoseconds The time since the run started, "
              "frozen when it ends.\n"
              "# TYPE praenora_workflow_uptime_nanoseconds gauge\n"
              R"(praenora_workflow_uptime_nanoseconds{praenora_odd_key_x="v",)"
              R"(praenora_run_number="1",praenora_run_fingerprint="f"} 5)"
              "\n");
}
