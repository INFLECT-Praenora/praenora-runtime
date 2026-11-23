// The OTLP export of a Runtime and its workflows, white-box, over a fake transport that the test
// gives the runtime (set_otlp_transport_factory_for_testing()), without sockets: what a run with
// the fixture modules exports (the resource, the run's number and fingerprint on every point and
// record, every attribute key with the prefix praenora.), a failed export that changes no health
// and no snapshot (it is counted and warned about), the same outputs, trace, fingerprint and
// reproducibility section with and without the export, ignored settings logged as
// telemetry.setting_ignored, one exporter shared by copies of a Runtime, and an exporter that
// outlives its Runtime while a workflow is open.
#include "observability/otlp/http_client.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>
#include <praenora/runtime/workflow.hpp>

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Health;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::Workflow;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::detail::HttpEndpoint;
using praenora::detail::HttpHeaders;
using praenora::detail::HttpResponse;
using praenora::detail::OtlpTransport;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

struct Sent {
    std::string url, body;
};

// What the fake transports of a test record, and the status they answer.
struct Recorder {
    std::mutex mutex;
    std::vector<Sent> sent;
    std::atomic<int> status{200};
    std::atomic<int> transports{0};

    std::vector<Sent> to(const std::string& path) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<Sent> matching;
        for (const Sent& request : sent) {
            if (request.url.size() >= path.size() &&
                request.url.compare(request.url.size() - path.size(), path.size(), path) == 0) {
                matching.push_back(request);
            }
        }
        return matching;
    }
};

class RecordingTransport final : public OtlpTransport {
public:
    explicit RecordingTransport(std::shared_ptr<Recorder> recorder)
        : recorder_(std::move(recorder)) {}
    HttpResponse post(const HttpEndpoint& endpoint, const HttpHeaders&, const std::string& body,
                      std::chrono::milliseconds) override {
        {
            const std::lock_guard<std::mutex> lock(recorder_->mutex);
            recorder_->sent.push_back({endpoint.url, body});
        }
        const int status = recorder_->status.load();
        HttpResponse response;
        response.status = status;
        if (status == 0) {
            response.error = "connection refused";
        }
        return response;
    }

private:
    std::shared_ptr<Recorder> recorder_;
};

// Gives the Runtimes constructed during its lifetime transports that record into `recorder`.
class FakeTransports {
public:
    FakeTransports() : recorder(std::make_shared<Recorder>()) {
        std::shared_ptr<Recorder> shared = recorder;
        praenora::detail::set_otlp_transport_factory_for_testing([shared] {
            ++shared->transports;
            return std::make_unique<RecordingTransport>(shared);
        });
    }
    ~FakeTransports() { praenora::detail::set_otlp_transport_factory_for_testing({}); }
    FakeTransports(const FakeTransports&) = delete;
    FakeTransports& operator=(const FakeTransports&) = delete;

    std::shared_ptr<Recorder> recorder;
};

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The fixture modules and the test catalog, without the environment, logging JSON lines at debug
// to `log` only; exporting to two endpoints that the fake transports stand for, when `exporting`.
RuntimeOptions options_for(const fs::path& log, bool exporting) {
    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    options.logging.level = LogLevel::debug;
    options.logging.format = LogFormat::json;
    options.logging.file = utf8(log);
    if (exporting) {
        options.telemetry.metrics_endpoint = "http://127.0.0.1:9/v1/metrics";
        options.telemetry.logs_endpoint = "http://127.0.0.1:9/v1/logs";
    }
    return options;
}

// A configuration of test.chain (fx-echo, then fx-echo again) in `dir`, of `ticks` ticks.
std::string write_chain(const fs::path& dir, int ticks) {
    write_text_file(dir / "source.json", "{\"value\": 42}");
    const fs::path file = dir / "workflow.yaml";
    write_text_file(file, "kind: workflow-config\n"
                          "format_version: \"0.1.0\"\n"
                          "workflow: { name: test.chain, version: \"0.1.0\" }\n"
                          "run: { ticks: " +
                              std::to_string(ticks) +
                              ", seed: 7 }\n"
                              "inputs:\n"
                              "  source: { source: json-document, path: source.json }\n"
                              "steps:\n"
                              "  a:\n"
                              "    module: { name: fx-echo, version: \"0.1.0\" }\n"
                              "    parameters: {}\n"
                              "  b:\n"
                              "    module: { name: fx-echo, version: \"0.1.0\" }\n"
                              "    parameters: {}\n");
    return utf8(file);
}

// Every JSON line of a log file.
std::vector<json> log_lines(const fs::path& file) {
    std::vector<json> lines;
    std::istringstream text(read_text(file));
    for (std::string line; std::getline(text, line);) {
        if (!line.empty()) {
            lines.push_back(json::parse(line));
        }
    }
    return lines;
}

std::vector<json> events_named(const fs::path& file, const std::string& event) {
    std::vector<json> found;
    for (const json& line : log_lines(file)) {
        if (line.at("attributes").value("praenora.event", "") == event) {
            found.push_back(line);
        }
    }
    return found;
}

// The attribute `key` of an OTLP attribute list, or null.
json attribute(const json& attributes, const std::string& key) {
    for (const json& entry : attributes) {
        if (entry.at("key") == key) {
            return entry.at("value");
        }
    }
    return nullptr;
}

// The files below `dir`, relative path to contents.
std::map<std::string, std::string> files_below(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[utf8(fs::relative(entry.path(), dir))] = read_text(entry.path());
        }
    }
    return files;
}

struct RunResult {
    std::string fingerprint;
    praenora::StatusSnapshot status;
    std::string metrics;
    json reproducibility;
    std::map<std::string, std::string> outputs, trace;
};

// Waits until `condition` holds, at most 20 s (a failure that is retried is counted after about
// 3 s); whether it holds.
template <class Condition> bool eventually(Condition&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Runs test.chain for 3 ticks with the message trace, into `dir`/out, and closes everything. The
// queries come after `after_close()`, while the workflow is still open, so a test can wait there
// until what it checks has happened (an export that failed).
RunResult run_chain(const fs::path& dir, bool exporting,
                    const std::function<void()>& after_close = {}) {
    RunResult result;
    const Runtime runtime(options_for(dir / "runtime.log", exporting));
    WorkflowOptions run;
    run.output_dir = utf8(dir / "out");
    run.trace_messages = true;
    std::unique_ptr<Workflow> workflow = runtime.open(write_chain(dir / "config", 3), run);
    EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
    workflow->close();
    if (after_close) {
        after_close();
    }
    result.fingerprint = workflow->fingerprint();
    result.status = workflow->status();
    result.metrics = workflow->metrics_json();
    result.reproducibility =
        json::parse(read_text(dir / "out" / "run-record.json")).at("reproducibility");
    result.outputs = files_below(dir / "out" / "outputs");
    result.trace = files_below(dir / "out" / "trace");
    return result;
}

} // namespace

TEST(OtlpRuntime, ExportsTheMetricsAndLogsOfARun) {
    ScratchDir dir;
    FakeTransports fakes;
    std::string fingerprint;
    {
        const Runtime runtime(options_for(dir.path() / "runtime.log", true));
        std::unique_ptr<Workflow> workflow =
            runtime.open(write_chain(dir.path() / "config", 2), WorkflowOptions{});
        ASSERT_EQ(workflow->run_number(), 1u);
        ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
        fingerprint = workflow->fingerprint();
        workflow->close();
    } // the workflow and the runtime are gone: the exporter has flushed
    EXPECT_EQ(fakes.recorder->transports.load(), 1);
    const std::vector<Sent> metrics = fakes.recorder->to("/v1/metrics");
    const std::vector<Sent> logs = fakes.recorder->to("/v1/logs");
    ASSERT_FALSE(metrics.empty());
    ASSERT_FALSE(logs.empty());

    const json last = json::parse(metrics.back().body).at("resourceMetrics").at(0);
    const json& resource = last.at("resource").at("attributes");
    EXPECT_EQ(attribute(resource, "service.name"), (json{{"stringValue", "praenora-runtime"}}));
    EXPECT_EQ(attribute(resource, "service.version"),
              (json{{"stringValue", praenora::version_info().runtime}}));
    EXPECT_EQ(attribute(resource, "process.pid"),
              (json{{"intValue", std::to_string(praenora::detail::current_process_id())}}));
    const std::string instance = attribute(resource, "service.instance.id").at("stringValue");
    EXPECT_TRUE(std::regex_match(instance, std::regex("^[0-9a-f-]{36}$"))) << instance;
    const json& scope = last.at("scopeMetrics").at(0);
    EXPECT_EQ(scope.at("scope").at("name"), "praenora.runtime");
    bool saw_ticks = false;
    for (const json& metric : scope.at("metrics")) {
        const std::string name = metric.at("name");
        const bool own = name.rfind("praenora.telemetry.", 0) == 0;
        for (const char* kind : {"sum", "gauge", "summary"}) {
            if (!metric.contains(kind)) {
                continue;
            }
            for (const json& point : metric.at(kind).at("dataPoints")) {
                for (const json& entry : point.at("attributes")) {
                    EXPECT_EQ(entry.at("key").get<std::string>().rfind("praenora.", 0), 0u)
                        << name << ": " << entry.dump();
                }
                if (!own) {
                    EXPECT_EQ(attribute(point.at("attributes"), "praenora.run.number"),
                              (json{{"intValue", "1"}}))
                        << name;
                    EXPECT_EQ(attribute(point.at("attributes"), "praenora.run.fingerprint"),
                              (json{{"stringValue", fingerprint}}))
                        << name;
                }
                if (name == "praenora.workflow.ticks") {
                    saw_ticks = true;
                    EXPECT_EQ(point.at("asInt"), "2") << "the last export has the final state";
                }
            }
        }
    }
    EXPECT_TRUE(saw_ticks);

    std::vector<std::string> events, scopes;
    for (const Sent& request : logs) {
        const json document = json::parse(request.body).at("resourceLogs").at(0);
        EXPECT_EQ(attribute(document.at("resource").at("attributes"), "service.instance.id"),
                  (json{{"stringValue", instance}}));
        for (const json& logged : document.at("scopeLogs")) {
            scopes.push_back(logged.at("scope").at("name"));
            for (const json& record : logged.at("logRecords")) {
                for (const json& entry : record.at("attributes")) {
                    EXPECT_EQ(entry.at("key").get<std::string>().rfind("praenora.", 0), 0u)
                        << entry.dump();
                }
                const json event = attribute(record.at("attributes"), "praenora.event");
                if (event.is_object()) {
                    events.push_back(event.at("stringValue"));
                }
                if (logged.at("scope").at("name") == "praenora.engine") {
                    EXPECT_EQ(attribute(record.at("attributes"), "praenora.run.number"),
                              (json{{"intValue", "1"}}));
                    EXPECT_EQ(attribute(record.at("attributes"), "praenora.run.fingerprint"),
                              (json{{"stringValue", fingerprint}}));
                }
            }
        }
    }
    EXPECT_NE(std::find(events.begin(), events.end(), "runtime.ready"), events.end());
    EXPECT_NE(std::find(events.begin(), events.end(), "run.started"), events.end());
    EXPECT_NE(std::find(events.begin(), events.end(), "run.completed"), events.end());
    EXPECT_NE(std::find(scopes.begin(), scopes.end(), "praenora.runtime"), scopes.end());
    EXPECT_NE(std::find(scopes.begin(), scopes.end(), "praenora.engine"), scopes.end());
    EXPECT_TRUE(events_named(dir.path() / "runtime.log", "telemetry.export_failed").empty());
}

TEST(OtlpRuntime, AFailedExportChangesNoHealthAndNoSnapshot) {
    for (const int status : {500, 0}) {
        ScratchDir dir;
        FakeTransports fakes;
        fakes.recorder->status = status;
        // The health and the snapshot are read once an export has failed: the final metrics, which
        // close() hands to the exporter, were sent and refused, and the failure was warned about
        // (after the retries, for a connection that fails).
        const fs::path log = dir.path() / "failing" / "runtime.log";
        bool failed = false;
        const RunResult failing = run_chain(dir.path() / "failing", true, [&] {
            failed = eventually([&] {
                return !fakes.recorder->to("/v1/metrics").empty() &&
                       !events_named(log, "telemetry.export_failed").empty();
            });
        });
        ASSERT_TRUE(failed) << status << ": no export failed while the workflow was open";
        EXPECT_EQ(failing.status.health, Health::healthy) << status;
        EXPECT_FALSE(failing.status.last_error) << status;
        for (const praenora::StepStatus& step : failing.status.steps) {
            EXPECT_FALSE(step.last_error) << status << ": " << step.step;
        }
        EXPECT_FALSE(fakes.recorder->to("/v1/metrics").empty()) << status;
        // Counted and warned about, on the runtime's logger.
        const std::vector<json> warnings = events_named(log, "telemetry.export_failed");
        ASSERT_FALSE(warnings.empty()) << status;
        EXPECT_EQ(warnings[0].at("severity_text"), "WARN");
        EXPECT_EQ(warnings[0].at("attributes").at("logger"), "praenora.runtime");
        // The snapshot says nothing of the export: the same points as a run without it.
        const RunResult plain = run_chain(dir.path() / "plain", false);
        json failing_points = json::parse(failing.metrics).at("metrics");
        json plain_points = json::parse(plain.metrics).at("metrics");
        ASSERT_EQ(failing_points.size(), plain_points.size()) << status;
        for (std::size_t i = 0; i < failing_points.size(); ++i) {
            EXPECT_EQ(failing_points[i].at("name"), plain_points[i].at("name"));
            EXPECT_EQ(failing_points[i].at("attributes"), plain_points[i].at("attributes"));
        }
        EXPECT_EQ(plain.status.health, Health::healthy);
    }
}

TEST(OtlpRuntime, ARunGivesTheSameResultsWithAndWithoutTheExport) {
    ScratchDir dir;
    FakeTransports fakes;
    const RunResult exporting = run_chain(dir.path() / "exporting", true);
    const RunResult plain = run_chain(dir.path() / "plain", false);
    ASSERT_FALSE(fakes.recorder->to("/v1/logs").empty());
    EXPECT_EQ(exporting.fingerprint, plain.fingerprint);
    EXPECT_EQ(exporting.reproducibility, plain.reproducibility);
    ASSERT_FALSE(plain.outputs.empty());
    EXPECT_EQ(exporting.outputs, plain.outputs);
    ASSERT_FALSE(plain.trace.empty());
    EXPECT_EQ(exporting.trace, plain.trace);
}

TEST(OtlpRuntime, IgnoredSettingsAreLogged) {
    ScratchDir dir;
    FakeTransports fakes;
    RuntimeOptions options = options_for(dir.path() / "runtime.log", false);
    options.telemetry.metrics_endpoint = "https://127.0.0.1:9/v1/metrics";
    options.telemetry.export_timeout_ms = -5;
    {
        const Runtime runtime(options);
    }
    EXPECT_EQ(fakes.recorder->transports.load(), 0) << "no endpoint left: no exporter";
    const std::vector<json> ignored =
        events_named(dir.path() / "runtime.log", "telemetry.setting_ignored");
    ASSERT_EQ(ignored.size(), 2u);
    EXPECT_EQ(ignored[0].at("severity_text"), "WARN");
    EXPECT_EQ(ignored[0].at("attributes").at("logger"), "praenora.runtime");
    EXPECT_EQ(ignored[0].at("attributes").at("praenora.variable"),
              "TelemetryOptions::metrics_endpoint");
    EXPECT_EQ(ignored[1].at("attributes").at("praenora.variable"),
              "TelemetryOptions::export_timeout_ms");
    EXPECT_TRUE(ignored[0].at("attributes").contains("praenora.reason"));
}

TEST(OtlpRuntime, CopiesOfARuntimeShareOneExporter) {
    ScratchDir dir;
    FakeTransports fakes;
    {
        const Runtime runtime(options_for(dir.path() / "runtime.log", true));
        const Runtime copy = runtime; // NOLINT(performance-unnecessary-copy-initialization)
        std::unique_ptr<Workflow> first = runtime.open(write_chain(dir.path() / "a", 1), {});
        std::unique_ptr<Workflow> second = copy.open(write_chain(dir.path() / "b", 1), {});
        EXPECT_EQ(first->run_number(), 1u);
        EXPECT_EQ(second->run_number(), 2u);
        first->run();
        second->run();
    }
    EXPECT_EQ(fakes.recorder->transports.load(), 1);
    std::vector<std::string> numbers;
    for (const Sent& request : fakes.recorder->to("/v1/metrics")) {
        const json document = json::parse(request.body);
        for (const json& metric :
             document.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics")) {
            if (metric.at("name") != "praenora.workflow.ticks") {
                continue;
            }
            for (const json& point : metric.at("sum").at("dataPoints")) {
                numbers.push_back(
                    attribute(point.at("attributes"), "praenora.run.number").at("intValue"));
            }
        }
    }
    EXPECT_NE(std::find(numbers.begin(), numbers.end(), "1"), numbers.end());
    EXPECT_NE(std::find(numbers.begin(), numbers.end(), "2"), numbers.end());
}

TEST(OtlpRuntime, TheExportOutlivesItsRuntimeWhileAWorkflowIsOpen) {
    ScratchDir dir;
    FakeTransports fakes;
    std::unique_ptr<Workflow> workflow;
    {
        const Runtime runtime(options_for(dir.path() / "runtime.log", true));
        workflow = runtime.open(write_chain(dir.path() / "config", 2), {});
    }
    ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
    workflow.reset();
    bool final_state = false;
    for (const Sent& request : fakes.recorder->to("/v1/metrics")) {
        final_state = final_state || request.body.find("\"asInt\":\"2\"") != std::string::npos;
    }
    EXPECT_TRUE(final_state);
    EXPECT_FALSE(fakes.recorder->to("/v1/logs").empty());
}
