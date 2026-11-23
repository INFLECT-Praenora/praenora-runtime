// The OTLP export against a fake collector of the test's own (tests/support/listener_support.hpp),
// over real connections on 127.0.0.1 (label server): the HTTP client (a POST of JSON with the
// configured headers, the status and Retry-After of an answer, a refused port, a collector that
// never answers, and, on POSIX, a collector that resets the connection while the request is
// written, which fails the request without SIGPIPE ending the process, in the child process of a
// death test), the exporter's retry of a 503 through it, and runs that export: the example
// workflow's metrics and logs (every attribute key with the prefix praenora., the runtime's
// process.pid, also with a step in a module host), a collector that answers 500 (the run completes
// and stays healthy once the export has failed), a collector that is down or never answers (the
// run stays healthy, and its end waits about one export timeout, not one per signal or retry),
// and the same results with and without the export. Where
// a listener is refused (a sandbox's EPERM) each test skips, saying "local listener refused:", or
// fails with PRAENORA_TESTS_REQUIRE_LISTENERS=1.
#include "listener_support.hpp"

#include "observability/logging.hpp"
#include "observability/otlp/exporter.hpp"
#include "observability/otlp/http_client.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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
using praenora::detail::HttpResponse;
using praenora::test::ClosedPort;
using praenora::test::CollectorAnswer;
using praenora::test::FakeCollector;
using praenora::test::ReceivedRequest;
#ifndef _WIN32
using praenora::test::ResettingListener;
#endif
using praenora::test::ScratchDir;
using praenora::test::SilentListener;
using praenora::test::write_text_file;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

constexpr milliseconds kPatience(15000);

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

HttpEndpoint loopback(int port, const std::string& path) {
    HttpEndpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = static_cast<std::uint16_t>(port);
    endpoint.path = path;
    endpoint.url = "http://127.0.0.1:" + std::to_string(port) + path;
    return endpoint;
}

// Logging as JSON lines at debug to `log` only, without the environment.
RuntimeOptions quiet_options(const fs::path& log) {
    RuntimeOptions options;
    options.use_environment = false;
    options.logging.to_stderr = false;
    options.logging.level = LogLevel::debug;
    options.logging.format = LogFormat::json;
    options.logging.file = utf8(log);
    return options;
}

// The fixture modules and the test catalog, exporting to `port` of 127.0.0.1 when it is not 0.
RuntimeOptions fixture_options(const fs::path& log, int port, std::int64_t timeout_ms = 10000) {
    RuntimeOptions options = quiet_options(log);
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    if (port != 0) {
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        options.telemetry.metrics_endpoint = base + "/v1/metrics";
        options.telemetry.logs_endpoint = base + "/v1/logs";
        options.telemetry.export_timeout_ms = timeout_ms;
    }
    return options;
}

// A configuration of test.chain (two fx-echo steps) in `dir`, of `ticks` ticks.
std::string write_chain(const fs::path& dir, int ticks) {
    write_text_file(dir / "source.json", "{\"value\": 42}");
    const fs::path file = dir / "workflow.yaml";
    write_text_file(file, "kind: workflow-config\n"
                          "format_version: \"0.1.0\"\n"
                          "workflow: { name: test.chain, version: \"0.1.0\" }\n"
                          "run: { ticks: " +
                              std::to_string(ticks) +
                              ", seed: 3 }\n"
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

std::vector<json> events_named(const fs::path& file, const std::string& event) {
    std::vector<json> found;
    std::istringstream text(read_text(file));
    for (std::string line; std::getline(text, line);) {
        if (line.empty()) {
            continue;
        }
        const json parsed = json::parse(line);
        if (parsed.at("attributes").value("praenora.event", "") == event) {
            found.push_back(parsed);
        }
    }
    return found;
}

json attribute(const json& attributes, const std::string& key) {
    for (const json& entry : attributes) {
        if (entry.at("key") == key) {
            return entry.at("value");
        }
    }
    return nullptr;
}

bool has_requests_to(const std::vector<ReceivedRequest>& requests, const std::string& path) {
    return std::any_of(requests.begin(), requests.end(),
                       [&](const ReceivedRequest& request) { return request.path == path; });
}

// What the collector received, checked: every request is a POST of JSON whose resource names
// this process; every attribute key of a metric point and of a log record has the prefix
// praenora.; returns the events of the records and the scopes, in order.
struct Received {
    std::vector<std::string> events, scopes;
    std::vector<json> records;
    std::vector<std::string> record_scopes; // the scope of each record
};
Received check_requests(const std::vector<ReceivedRequest>& requests) {
    Received received;
    const json pid = {{"intValue", std::to_string(praenora::detail::current_process_id())}};
    for (const ReceivedRequest& request : requests) {
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(request.header("Content-Type"), "application/json") << request.path;
        const json document = json::parse(request.body);
        const bool metrics = request.path == "/v1/metrics";
        const json& resource = document.at(metrics ? "resourceMetrics" : "resourceLogs").at(0);
        EXPECT_EQ(attribute(resource.at("resource").at("attributes"), "process.pid"), pid);
        if (metrics) {
            for (const json& metric : resource.at("scopeMetrics").at(0).at("metrics")) {
                for (const char* kind : {"sum", "gauge", "summary"}) {
                    if (!metric.contains(kind)) {
                        continue;
                    }
                    for (const json& point : metric.at(kind).at("dataPoints")) {
                        for (const json& entry : point.at("attributes")) {
                            EXPECT_EQ(entry.at("key").get<std::string>().rfind("praenora.", 0), 0u)
                                << entry.dump();
                        }
                    }
                }
            }
            continue;
        }
        for (const json& scope : resource.at("scopeLogs")) {
            received.scopes.push_back(scope.at("scope").at("name"));
            for (const json& record : scope.at("logRecords")) {
                for (const json& entry : record.at("attributes")) {
                    EXPECT_EQ(entry.at("key").get<std::string>().rfind("praenora.", 0), 0u)
                        << entry.dump();
                }
                const json event = attribute(record.at("attributes"), "praenora.event");
                if (event.is_object()) {
                    received.events.push_back(event.at("stringValue"));
                }
                received.records.push_back(record);
                received.record_scopes.push_back(scope.at("scope").at("name"));
            }
        }
    }
    return received;
}

// Waits until `condition` holds, at most 20 s (an export that is retried fails after about 3 s);
// whether it holds.
template <class Condition> bool eventually(Condition&& condition) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!condition()) {
        if (Clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

std::map<std::string, std::string> files_below(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[utf8(fs::relative(entry.path(), dir))] = read_text(entry.path());
        }
    }
    return files;
}

} // namespace

TEST(OtlpHttp, PostsJsonToTheCollector) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    const auto transport = praenora::detail::make_http_transport();
    const HttpResponse response =
        transport->post(loopback(collector.port(), "/v1/logs"), {{"x-api-key", "k 1"}},
                        "{\"resourceLogs\":[]}", milliseconds(5000));
    EXPECT_EQ(response.status, 200) << response.error;
    const std::vector<ReceivedRequest> requests = collector.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].path, "/v1/logs");
    EXPECT_EQ(requests[0].header("Content-Type"), "application/json");
    EXPECT_EQ(requests[0].header("x-api-key"), "k 1");
    EXPECT_EQ(requests[0].header("Host"), "127.0.0.1:" + std::to_string(collector.port()));
    EXPECT_EQ(requests[0].body, "{\"resourceLogs\":[]}");
}

TEST(OtlpHttp, GivesTheStatusAndRetryAfter) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    collector.script({CollectorAnswer{503, {{"Retry-After", "7"}}, "{}"},
                      CollectorAnswer{400, {}, "{\"code\":3}"}});
    const auto transport = praenora::detail::make_http_transport();
    const HttpEndpoint endpoint = loopback(collector.port(), "/v1/metrics");
    const HttpResponse busy = transport->post(endpoint, {}, "{}", milliseconds(5000));
    EXPECT_EQ(busy.status, 503);
    EXPECT_EQ(busy.retry_after_seconds, std::optional<std::int64_t>(7));
    const HttpResponse bad = transport->post(endpoint, {}, "{}", milliseconds(5000));
    EXPECT_EQ(bad.status, 400);
    EXPECT_FALSE(bad.retry_after_seconds);
}

TEST(OtlpHttp, ARefusedConnectionHasNoStatus) {
    ClosedPort port;
    PRAENORA_START_LISTENER(port);
    const auto transport = praenora::detail::make_http_transport();
    const auto start = Clock::now();
    const HttpResponse response =
        transport->post(loopback(port.port(), "/v1/logs"), {}, "{}", milliseconds(200));
    EXPECT_EQ(response.status, 0);
    EXPECT_FALSE(response.error.empty());
    EXPECT_LT(Clock::now() - start, milliseconds(1000));
}

TEST(OtlpHttp, ACollectorThatNeverAnswersTimesOut) {
    SilentListener silent;
    PRAENORA_START_LISTENER(silent);
    const auto transport = praenora::detail::make_http_transport();
    const auto start = Clock::now();
    const HttpResponse response =
        transport->post(loopback(silent.port(), "/v1/logs"), {}, "{}", milliseconds(200));
    const auto took = Clock::now() - start;
    EXPECT_EQ(response.status, 0);
    EXPECT_FALSE(response.error.empty());
    EXPECT_LT(took, milliseconds(1000));
    EXPECT_GE(took, milliseconds(150));
}

#ifndef _WIN32
// A collector that resets the connection while the request is written fails the request, and
// never ends the process with SIGPIPE, whatever its disposition (plain `praenora run`, and an
// application that embeds the runtime, keep the default, which ends a process). In the child
// process of a death test, with SIGPIPE at its default disposition: 16 MiB to a listener that
// reads the start of the request and resets the connection. cpp-httplib's client waits for
// "100 Continue" before such a body, and the reset comes during that wait: the body's first write
// meets the reset connection.
TEST(OtlpHttp, AResetConnectionFailsTheRequestNotTheProcess) {
    SilentListener probe;
    PRAENORA_START_LISTENER(probe);
    probe.close();
    praenora::test::expect_failure_without_sigpipe([]() -> std::optional<std::string> {
        ResettingListener collector;
        if (const std::optional<std::string> refusal = collector.start()) {
            std::fprintf(stderr, "the child's listener was refused: %s\n", refusal->c_str());
            return std::nullopt;
        }
        const auto transport = praenora::detail::make_http_transport();
        const HttpResponse response =
            transport->post(loopback(collector.port(), "/v1/logs"), {},
                            std::string(std::size_t{16} << 20, 'x'), milliseconds(10000));
        if (response.status != 0 || response.error.empty()) {
            std::fprintf(stderr, "status %d, error '%s'\n", response.status,
                         response.error.c_str());
            return std::nullopt;
        }
        return response.error;
    });
}
#endif

TEST(OtlpExport, RetriesA503ThroughTheHttpClient) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    collector.script({CollectorAnswer{503, {}, "{}"}, CollectorAnswer{200, {}, "{}"}});
    praenora::detail::OtlpSettings settings;
    settings.logs.endpoint = loopback(collector.port(), "/v1/logs");
    settings.logs.timeout = milliseconds(5000);
    praenora::detail::OtlpTiming timing;
    timing.batch_delay = milliseconds(1);
    timing.retry_delays = {milliseconds(10), milliseconds(20)};
    praenora::LogOptions quiet;
    quiet.to_stderr = false;
    praenora::detail::LogHub hub(quiet);
    {
        praenora::detail::OtlpExporter exporter(std::move(settings), {{"service.name", "t"}},
                                                "0.2.0", praenora::detail::make_http_transport(),
                                                nullptr, timing);
        hub.add_sink(exporter.log_sink(nullptr));
        praenora::detail::log_event(*hub.logger("praenora.runtime"), LogLevel::info, "one");
        ASSERT_TRUE(collector.wait_for(
            [](const std::vector<ReceivedRequest>& requests) { return requests.size() >= 2; },
            kPatience));
        EXPECT_EQ(exporter.failures(praenora::detail::OtlpSignal::logs), 0u);
    }
    const std::vector<ReceivedRequest> requests = collector.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].body, requests[1].body);
}

#ifdef PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR
TEST(TelemetryRun, ExportsTheExampleWorkflow) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    ScratchDir dir;
    RuntimeOptions options = quiet_options(dir.path() / "runtime.log");
    options.module_paths = {PRAENORA_EXAMPLE_MODULES_DIR};
    options.telemetry.metrics_endpoint = collector.url("/v1/metrics");
    options.telemetry.logs_endpoint = collector.url("/v1/logs");
    std::string fingerprint;
    {
        const Runtime runtime(options);
        WorkflowOptions run;
        run.output_dir = utf8(dir.path() / "out");
        run.ticks = 3;
        run.log_level = LogLevel::debug; // over the configuration's info
        const auto workflow = runtime.open(
            utf8(praenora::test::utf8_path(PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR) / "workflow.yaml"),
            run);
        ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
        fingerprint = workflow->fingerprint();
    }
    const std::vector<ReceivedRequest> requests = collector.requests();
    ASSERT_TRUE(has_requests_to(requests, "/v1/metrics"));
    ASSERT_TRUE(has_requests_to(requests, "/v1/logs"));
    const Received received = check_requests(requests);
    EXPECT_TRUE(contains(received.events, "run.started"));
    EXPECT_TRUE(contains(received.events, "invocation.completed"));
    EXPECT_TRUE(contains(received.scopes, "praenora.engine"));
    EXPECT_TRUE(contains(received.scopes, "module.predict"));
    // Every record of the run (all but the Runtime's own, on praenora.runtime) names it.
    std::size_t engine_records = 0;
    for (std::size_t i = 0; i < received.records.size(); ++i) {
        if (received.record_scopes[i] == "praenora.runtime") {
            continue;
        }
        engine_records += received.record_scopes[i] == "praenora.engine" ? 1 : 0;
        const json& attributes = received.records[i].at("attributes");
        EXPECT_EQ(attribute(attributes, "praenora.run.number"), (json{{"intValue", "1"}}))
            << received.record_scopes[i] << ": " << received.records[i].dump();
        EXPECT_EQ(attribute(attributes, "praenora.run.fingerprint"),
                  (json{{"stringValue", fingerprint}}))
            << received.record_scopes[i];
    }
    EXPECT_GT(engine_records, 0u);
}

#ifndef _WIN32
TEST(TelemetryRun, TheRuntimesProcessExportsAStepInAModuleHost) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    ScratchDir dir;
    // The example's configuration with its step predict in a module host, and its data named by
    // an absolute path.
    const fs::path example = praenora::test::utf8_path(PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR);
    std::string config = read_text(example / "workflow.yaml");
    const std::string data = "path: data/observations.csv";
    const std::string level = "    log_level: debug\n";
    ASSERT_NE(config.find(data), std::string::npos);
    ASSERT_NE(config.find(level), std::string::npos);
    config.replace(config.find(data), data.size(),
                   "path: " + json(utf8(example / "data" / "observations.csv")).dump());
    config.insert(config.find(level) + level.size(), "    isolation: { mode: process }\n");
    write_text_file(dir.path() / "workflow.yaml", config);

    RuntimeOptions options = quiet_options(dir.path() / "runtime.log");
    options.module_paths = {PRAENORA_EXAMPLE_MODULES_DIR};
    options.telemetry.metrics_endpoint = collector.url("/v1/metrics");
    options.telemetry.logs_endpoint = collector.url("/v1/logs");
    {
        const Runtime runtime(options);
        WorkflowOptions run;
        run.output_dir = utf8(dir.path() / "out");
        run.ticks = 2;
        run.log_level = LogLevel::debug; // module_host.started is a debug event
        const auto workflow = runtime.open(utf8(dir.path() / "workflow.yaml"), run);
        ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
    }
    const json isolation = json::parse(read_text(dir.path() / "out" / "run-record.json"))
                               .at("execution")
                               .at("isolation");
    ASSERT_EQ(isolation.at("predict").at("mode"), "process");
    const Received received = check_requests(collector.requests()); // process.pid: this one's
    ASSERT_TRUE(contains(received.events, "module_host.started"));
    const std::string own = std::to_string(praenora::detail::current_process_id());
    for (const json& record : received.records) {
        const json event = attribute(record.at("attributes"), "praenora.event");
        if (event == json{{"stringValue", "module_host.started"}}) {
            const json pid = attribute(record.at("attributes"), "praenora.pid");
            ASSERT_TRUE(pid.is_object()) << record.dump();
            EXPECT_NE(pid.at("intValue"), own) << "the module host is a process of its own";
        }
    }
    EXPECT_TRUE(contains(received.scopes, "module.predict"));
}
#endif
#endif

TEST(TelemetryRun, ACollectorThatFailsChangesNothing) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    collector.set_default_answer(CollectorAnswer{500, {}, "{}"});
    ScratchDir dir;
    const fs::path log = dir.path() / "runtime.log";
    praenora::StatusSnapshot status;
    std::string metrics;
    {
        const Runtime runtime(fixture_options(log, collector.port()));
        const auto workflow = runtime.open(write_chain(dir.path() / "config", 3), {});
        ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
        workflow->close();
        // The health and the snapshot are read once an export has failed, the workflow still
        // open: the final metrics that close() hands over were answered 500, and warned about.
        ASSERT_TRUE(collector.wait_for(
            [](const std::vector<ReceivedRequest>& received) {
                return has_requests_to(received, "/v1/metrics");
            },
            kPatience));
        ASSERT_TRUE(
            eventually([&] { return !events_named(log, "telemetry.export_failed").empty(); }));
        status = workflow->status();
        metrics = workflow->metrics_json();
    }
    EXPECT_EQ(status.health, Health::healthy);
    EXPECT_FALSE(status.last_error);
    for (const praenora::StepStatus& step : status.steps) {
        EXPECT_FALSE(step.last_error) << step.step;
    }
    EXPECT_EQ(metrics.find("telemetry"), std::string::npos) << "the run's metrics name no export";
    const std::vector<ReceivedRequest> requests = collector.requests();
    EXPECT_TRUE(has_requests_to(requests, "/v1/metrics"));
    EXPECT_TRUE(has_requests_to(requests, "/v1/logs"));
    const std::vector<json> warnings =
        events_named(dir.path() / "runtime.log", "telemetry.export_failed");
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings[0].at("attributes").at("praenora.reason").get<std::string>().find("500"),
              std::string::npos);
}

// A collector that is down or never answers: the run stays healthy once its exports have failed,
// and the end of a run (close(), then the workflow and the Runtime gone) takes about one export
// timeout, 500 ms here, not one per signal or retry: well under two (the end used to post the
// final metrics again after the request in flight failed, then the logs: three timeouts).
TEST(TelemetryRun, ADeadCollectorDoesNotHoldUpTheEnd) {
    for (const bool answers_never : {false, true}) {
        const char* const kind = answers_never ? "never answers" : "refused";
        SilentListener silent;
        ClosedPort closed;
        int port = 0;
        if (answers_never) {
            PRAENORA_START_LISTENER(silent);
            port = silent.port();
        } else {
            PRAENORA_START_LISTENER(closed);
            port = closed.port();
        }
        ScratchDir dir;
        // The health, read once an export has failed (after its retries), the workflow still open.
        {
            const fs::path log = dir.path() / "health.log";
            const Runtime runtime(fixture_options(log, port, 200));
            const auto workflow = runtime.open(write_chain(dir.path() / "health", 3), {});
            ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
            workflow->close();
            ASSERT_TRUE(eventually([&] {
                return !events_named(log, "telemetry.export_failed").empty();
            })) << kind;
            const praenora::StatusSnapshot status = workflow->status();
            EXPECT_EQ(status.health, Health::healthy) << kind;
            EXPECT_FALSE(status.last_error) << kind;
        }
        // The end, with the final metrics in flight when it starts.
        Clock::time_point start;
        {
            const Runtime runtime(fixture_options(dir.path() / "end.log", port, 500));
            const auto workflow = runtime.open(write_chain(dir.path() / "end", 3), {});
            ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
            start = Clock::now();
            workflow->close();
        }
        const auto took = Clock::now() - start;
        EXPECT_LT(took, milliseconds(1000))
            << kind << ": " << std::chrono::duration_cast<milliseconds>(took).count()
            << " ms for an export timeout of 500 ms";
    }
}

TEST(TelemetryRun, TheSameResultsWithAndWithoutTheExport) {
    FakeCollector collector;
    PRAENORA_START_LISTENER(collector);
    ScratchDir dir;
    struct Result {
        std::string fingerprint;
        json reproducibility;
        std::map<std::string, std::string> outputs, trace;
    };
    const auto run_once = [&](const fs::path& at, int port) {
        Result result;
        const Runtime runtime(fixture_options(at / "runtime.log", port));
        WorkflowOptions run;
        run.output_dir = utf8(at / "out");
        run.trace_messages = true;
        const auto workflow = runtime.open(write_chain(at / "config", 3), run);
        EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
        workflow->close();
        result.fingerprint = workflow->fingerprint();
        result.reproducibility =
            json::parse(read_text(at / "out" / "run-record.json")).at("reproducibility");
        result.outputs = files_below(at / "out" / "outputs");
        result.trace = files_below(at / "out" / "trace");
        return result;
    };
    const Result exporting = run_once(dir.path() / "exporting", collector.port());
    const Result plain = run_once(dir.path() / "plain", 0);
    ASSERT_TRUE(has_requests_to(collector.requests(), "/v1/logs"));
    EXPECT_EQ(exporting.fingerprint, plain.fingerprint);
    EXPECT_EQ(exporting.reproducibility, plain.reproducibility);
    ASSERT_FALSE(plain.outputs.empty());
    EXPECT_EQ(exporting.outputs, plain.outputs);
    ASSERT_FALSE(plain.trace.empty());
    EXPECT_EQ(exporting.trace, plain.trace);
}
