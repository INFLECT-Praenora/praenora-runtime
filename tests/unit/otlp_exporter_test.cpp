// The OTLP exporter (observability/otlp/exporter.hpp) over a fake transport, without sockets: its
// limits (batches of 512, 1 s, a queue of 2048 and the count of the records dropped beyond it),
// retries (a 503 and then a 200, statuses that are not retried, three attempts at most,
// Retry-After), the warning at a limited rate, the flush of its destructor with one attempt per
// signal and no wait, a stop during a wait between attempts, an end bounded by one export timeout
// against a collector that never answers (a request that fails at the end is not sent again), the
// queue's limit kept when a batch goes back, an error on the exporter's thread counted and not
// fatal, the metrics of runs (every interval, and the last snapshot when a run finishes), the
// run's attributes of log records, and a record that is not UTF-8 in a valid request.
#include "observability/otlp/exporter.hpp"

#include "log_capture.hpp"
#include "observability/logging.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;
using praenora::LogLevel;
using praenora::LogOptions;
using praenora::StatusSnapshot;
using praenora::detail::HttpEndpoint;
using praenora::detail::HttpHeaders;
using praenora::detail::HttpResponse;
using praenora::detail::LogHub;
using praenora::detail::OtlpExporter;
using praenora::detail::OtlpRun;
using praenora::detail::OtlpSettings;
using praenora::detail::OtlpSignal;
using praenora::detail::OtlpTiming;
using praenora::detail::OtlpTransport;
using praenora::test::LogCapture;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

constexpr milliseconds kPatience(10000);

struct SentRequest {
    std::string url;
    HttpHeaders headers;
    std::string body;
    milliseconds timeout{0};
};

// What a FakeTransport records and answers, shared with the test.
struct Exchange {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<SentRequest> requests;
    std::deque<HttpResponse> script; // the answers to the next requests
    HttpResponse fallback{200, "", std::nullopt};
    bool held = false; // a request waits, recorded, until the test lets go
    // A collector that never answers: a request waits its whole timeout, then gets no answer.
    bool hangs = false;

    void hold() {
        const std::lock_guard<std::mutex> lock(mutex);
        held = true;
    }
    void release() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            held = false;
        }
        changed.notify_all();
    }
    void answer(std::vector<HttpResponse> answers) {
        const std::lock_guard<std::mutex> lock(mutex);
        script.assign(answers.begin(), answers.end());
    }
    void answer_always(HttpResponse response) {
        const std::lock_guard<std::mutex> lock(mutex);
        fallback = std::move(response);
    }
    std::vector<SentRequest> sent() {
        const std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }
    std::vector<SentRequest> sent_to(const std::string& path) {
        std::vector<SentRequest> matching;
        for (SentRequest& request : sent()) {
            if (request.url.size() >= path.size() &&
                request.url.compare(request.url.size() - path.size(), path.size(), path) == 0) {
                matching.push_back(std::move(request));
            }
        }
        return matching;
    }
    // Waits until at least `count` requests to `path` were sent; whether they were.
    bool wait_for(std::size_t count, const std::string& path, milliseconds timeout = kPatience) {
        const auto deadline = Clock::now() + timeout;
        while (sent_to(path).size() < count) {
            std::unique_lock<std::mutex> lock(mutex);
            if (changed.wait_until(lock, deadline) == std::cv_status::timeout) {
                lock.unlock();
                return sent_to(path).size() >= count;
            }
        }
        return true;
    }
};

class FakeTransport final : public OtlpTransport {
public:
    explicit FakeTransport(std::shared_ptr<Exchange> exchange) : exchange_(std::move(exchange)) {}

    HttpResponse post(const HttpEndpoint& endpoint, const HttpHeaders& headers,
                      const std::string& body, milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(exchange_->mutex);
        exchange_->requests.push_back({endpoint.url, headers, body, timeout});
        exchange_->changed.notify_all();
        exchange_->changed.wait(lock, [&] { return !exchange_->held; });
        if (exchange_->hangs) {
            lock.unlock();
            std::this_thread::sleep_for(timeout);
            return HttpResponse{0, "timed out", std::nullopt};
        }
        if (!exchange_->script.empty()) {
            HttpResponse response = std::move(exchange_->script.front());
            exchange_->script.pop_front();
            return response;
        }
        return exchange_->fallback;
    }

private:
    std::shared_ptr<Exchange> exchange_;
};

HttpEndpoint test_endpoint(const std::string& path) {
    HttpEndpoint result;
    result.host = "127.0.0.1";
    result.port = 9;
    result.path = path;
    result.url = "http://127.0.0.1:9" + path;
    return result;
}

OtlpSettings settings(bool metrics, bool logs) {
    OtlpSettings result;
    if (metrics) {
        result.metrics.endpoint = test_endpoint("/v1/metrics");
        result.metrics.headers = {{"x-signal", "metrics"}};
    }
    if (logs) {
        result.logs.endpoint = test_endpoint("/v1/logs");
        result.logs.headers = {{"x-signal", "logs"}};
    }
    result.metrics.timeout = milliseconds(200);
    result.logs.timeout = milliseconds(300);
    result.metric_export_interval = std::chrono::hours(1);
    return result;
}

// Short waits, so that the tests take little time; the batch delay and the interval are long
// unless a test shortens them.
OtlpTiming quick_timing() {
    OtlpTiming timing;
    timing.batch_delay = std::chrono::hours(1);
    timing.retry_delays = {milliseconds(10), milliseconds(20)};
    timing.retry_after_unit = milliseconds(1);
    return timing;
}

LogOptions quiet() {
    LogOptions options;
    options.level = LogLevel::trace;
    options.to_stderr = false;
    return options;
}

// An exporter over a fake transport, a hub whose records it exports (the records of `run`), and a
// hub of the exporter's own warnings, captured.
struct Bench {
    explicit Bench(OtlpSettings with, OtlpTiming timing = quick_timing(),
                   std::shared_ptr<const OtlpRun> run = nullptr)
        : exchange(std::make_shared<Exchange>()), records(quiet()), warnings(quiet()),
          capture(warnings) {
        exporter = std::make_unique<OtlpExporter>(
            std::move(with), std::vector<praenora::detail::LogAttribute>{{"service.name", "t"}},
            "0.2.0", std::make_unique<FakeTransport>(exchange), warnings.logger("praenora.runtime"),
            std::move(timing));
        if (auto sink = exporter->log_sink(std::move(run))) {
            records.add_sink(sink);
        }
        logger = records.logger("praenora.engine");
    }

    void log(std::size_t count, const std::string& message = "record") {
        for (std::size_t i = 0; i < count; ++i) {
            praenora::detail::log_event(*logger, LogLevel::info, message, {{"praenora.index", i}});
        }
    }

    std::vector<json> export_failures() const {
        std::vector<json> found;
        for (const json& line : capture.json_lines()) {
            if (line.at("attributes").value("praenora.event", "") == "telemetry.export_failed") {
                found.push_back(line);
            }
        }
        return found;
    }

    std::shared_ptr<Exchange> exchange;
    LogHub records;
    LogHub warnings;
    LogCapture capture;
    std::unique_ptr<OtlpExporter> exporter;
    std::shared_ptr<spdlog::logger> logger;
};

std::size_t record_count(const std::string& body) {
    std::size_t count = 0;
    const json document = json::parse(body);
    for (const json& scope : document.at("resourceLogs").at(0).at("scopeLogs")) {
        count += scope.at("logRecords").size();
    }
    return count;
}

// Waits until `condition` holds, at most kPatience.
template <class Condition> bool eventually(Condition&& condition) {
    const auto deadline = Clock::now() + kPatience;
    while (!condition()) {
        if (Clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

StatusSnapshot snapshot_of(std::uint64_t ticks) {
    StatusSnapshot snapshot;
    snapshot.workflow_name = "test.single";
    snapshot.workflow_version = "0.1.0";
    snapshot.ticks_completed = ticks;
    snapshot.generated_at_unix_ns = 2000;
    praenora::StepStatus step;
    step.step = "a";
    step.module_name = "fx-echo";
    step.module_version = "0.1.0";
    snapshot.steps.push_back(step);
    return snapshot;
}

// The value of praenora.workflow.ticks in a metrics request's body, and its run number.
std::pair<std::string, std::string> ticks_of(const std::string& body) {
    const json document = json::parse(body);
    for (const json& metric :
         document.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics")) {
        if (metric.at("name") == "praenora.workflow.ticks") {
            const json& point = metric.at("sum").at("dataPoints").at(0);
            std::string number;
            for (const json& attribute : point.at("attributes")) {
                if (attribute.at("key") == "praenora.run.number") {
                    number = attribute.at("value").at("intValue");
                }
            }
            return {point.at("asInt"), number};
        }
    }
    return {"none", "none"};
}

} // namespace

TEST(OtlpExporter, TheDefaultsAreTheSpecifiedOnes) {
    const OtlpTiming timing;
    EXPECT_EQ(timing.max_batch, 512u);
    EXPECT_EQ(timing.max_queue, 2048u);
    EXPECT_EQ(timing.batch_delay, milliseconds(1000));
    EXPECT_EQ(timing.retry_delays,
              (std::vector<milliseconds>{milliseconds(1000), milliseconds(2000)}));
    EXPECT_EQ(timing.retry_after_unit, milliseconds(1000));
    EXPECT_EQ(timing.max_retry_after, 30);
    EXPECT_EQ(timing.warning_interval, milliseconds(60000));
}

TEST(OtlpExporter, SendsAFullBatchAtOnce) {
    Bench bench(settings(false, true));
    ASSERT_TRUE(bench.exporter->exports_logs());
    EXPECT_FALSE(bench.exporter->exports_metrics());
    bench.log(513);
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/logs"));
    std::this_thread::sleep_for(milliseconds(100));
    std::vector<SentRequest> sent = bench.exchange->sent();
    ASSERT_EQ(sent.size(), 1u) << "only a full batch goes before the delay";
    EXPECT_EQ(record_count(sent[0].body), 512u);
    EXPECT_EQ(sent[0].url, "http://127.0.0.1:9/v1/logs");
    EXPECT_EQ(sent[0].headers, (HttpHeaders{{"x-signal", "logs"}}));
    EXPECT_EQ(sent[0].timeout, milliseconds(300));
    bench.exporter.reset(); // the flush sends the rest
    sent = bench.exchange->sent();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(record_count(sent[1].body), 1u);
}

TEST(OtlpExporter, SendsWhatWaitsAfterTheBatchDelay) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(150);
    Bench bench(settings(false, true), timing);
    const auto start = Clock::now();
    bench.log(3);
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/logs"));
    EXPECT_GE(Clock::now() - start, milliseconds(140));
    EXPECT_EQ(record_count(bench.exchange->sent()[0].body), 3u);
}

TEST(OtlpExporter, DropsRecordsBeyondTheQueueAndCountsThem) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    Bench bench(settings(true, true), timing);
    auto run = std::make_shared<OtlpRun>("fp");
    run->set_number(1);
    OtlpExporter::Run added = bench.exporter->add_run(run, [] { return snapshot_of(1); }, 1000);
    bench.exchange->hold();
    bench.log(1);
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/logs")); // the exporter waits in it
    bench.log(3000);
    EXPECT_EQ(bench.exporter->dropped_log_records(), 3000u - 2048u);
    bench.exchange->release();
    ASSERT_TRUE(bench.exchange->wait_for(5, "/v1/logs"));
    const std::vector<SentRequest> sent = bench.exchange->sent_to("/v1/logs");
    ASSERT_EQ(sent.size(), 5u);
    EXPECT_EQ(record_count(sent[0].body), 1u);
    for (std::size_t i = 1; i < sent.size(); ++i) {
        EXPECT_EQ(record_count(sent[i].body), 512u) << i;
    }
    // The count goes out with the metrics.
    added.finish();
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/metrics"));
    const json metrics = json::parse(bench.exchange->sent_to("/v1/metrics").at(0).body)
                             .at("resourceMetrics")
                             .at(0)
                             .at("scopeMetrics")
                             .at(0)
                             .at("metrics");
    std::string dropped = "none";
    for (const json& metric : metrics) {
        if (metric.at("name") == "praenora.telemetry.log_records.dropped") {
            dropped = metric.at("sum").at("dataPoints").at(0).at("asInt");
        }
    }
    EXPECT_EQ(dropped, "952");
}

TEST(OtlpExporter, RetriesAFailureThatMayPass) {
    for (const int status : {0, 429, 502, 503, 504}) {
        std::vector<milliseconds> waits;
        std::mutex waits_mutex;
        OtlpTiming timing = quick_timing();
        timing.batch_delay = milliseconds(1);
        timing.on_retry_wait = [&](milliseconds wait) {
            const std::lock_guard<std::mutex> lock(waits_mutex);
            waits.push_back(wait);
        };
        Bench bench(settings(false, true), timing);
        bench.exchange->answer({HttpResponse{status, status == 0 ? "refused" : "", std::nullopt},
                                HttpResponse{200, "", std::nullopt}});
        bench.log(1);
        ASSERT_TRUE(bench.exchange->wait_for(2, "/v1/logs")) << status;
        const std::vector<SentRequest> sent = bench.exchange->sent();
        EXPECT_EQ(sent[0].body, sent[1].body) << status;
        bench.exporter.reset();
        EXPECT_EQ(bench.exchange->sent().size(), 2u) << status;
        const std::lock_guard<std::mutex> lock(waits_mutex);
        EXPECT_EQ(waits, std::vector<milliseconds>{milliseconds(10)}) << status;
        EXPECT_TRUE(bench.export_failures().empty()) << status;
    }
}

TEST(OtlpExporter, DoesNotRetryOtherStatuses) {
    for (const int status : {400, 401, 404, 413, 500}) {
        OtlpTiming timing = quick_timing();
        timing.batch_delay = milliseconds(1);
        Bench bench(settings(false, true), timing);
        bench.exchange->answer_always(HttpResponse{status, "", std::nullopt});
        bench.log(1);
        ASSERT_TRUE(eventually([&] { return bench.exporter->failures(OtlpSignal::logs) == 1; }))
            << status;
        std::this_thread::sleep_for(milliseconds(50));
        EXPECT_EQ(bench.exchange->sent().size(), 1u) << status;
        EXPECT_EQ(bench.exporter->failures(OtlpSignal::metrics), 0u);
        const std::vector<json> warnings = bench.export_failures();
        ASSERT_EQ(warnings.size(), 1u) << status;
        EXPECT_EQ(warnings[0].at("severity_text"), "WARN");
        EXPECT_EQ(warnings[0].at("attributes").at("logger"), "praenora.runtime");
        EXPECT_EQ(warnings[0].at("attributes").at("praenora.signal"), "logs");
        EXPECT_EQ(warnings[0].at("attributes").at("praenora.failures"), 1);
        const std::string reason = warnings[0].at("attributes").at("praenora.reason");
        EXPECT_NE(reason.find(std::to_string(status)), std::string::npos) << reason;
    }
}

TEST(OtlpExporter, GivesUpAfterThreeAttempts) {
    std::vector<milliseconds> waits;
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.on_retry_wait = [&](milliseconds wait) { waits.push_back(wait); };
    Bench bench(settings(false, true), timing);
    bench.exchange->answer_always(HttpResponse{503, "", std::nullopt});
    bench.log(1);
    ASSERT_TRUE(eventually([&] { return bench.exporter->failures(OtlpSignal::logs) == 1; }));
    EXPECT_EQ(bench.exchange->sent().size(), 3u);
    bench.exporter.reset();
    EXPECT_EQ(waits, (std::vector<milliseconds>{milliseconds(10), milliseconds(20)}));
}

TEST(OtlpExporter, WaitsWhatRetryAfterSays) {
    std::vector<milliseconds> waits;
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.on_retry_wait = [&](milliseconds wait) { waits.push_back(wait); };
    Bench bench(settings(false, true), timing);
    bench.exchange->answer({HttpResponse{429, "", 3}, HttpResponse{503, "", 100},
                            HttpResponse{200, "", std::nullopt}});
    bench.log(1);
    ASSERT_TRUE(bench.exchange->wait_for(3, "/v1/logs"));
    bench.exporter.reset();
    EXPECT_EQ(waits, (std::vector<milliseconds>{milliseconds(3), milliseconds(30)}))
        << "Retry-After waits its seconds, at most 30";
    EXPECT_EQ(bench.exchange->sent().size(), 3u);
}

TEST(OtlpExporter, WarnsOnceWithinTheInterval) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.max_batch = 1;
    Bench bench(settings(false, true), timing);
    bench.exchange->answer_always(HttpResponse{400, "", std::nullopt});
    for (std::uint64_t failures = 1; failures <= 3; ++failures) {
        bench.log(1);
        ASSERT_TRUE(
            eventually([&] { return bench.exporter->failures(OtlpSignal::logs) == failures; }));
    }
    const std::vector<json> warnings = bench.export_failures();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].at("attributes").at("praenora.failures"), 1);
}

TEST(OtlpExporter, AWarningCountsTheFailuresSinceTheLastOne) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.max_batch = 1;
    timing.warning_interval = milliseconds(300);
    Bench bench(settings(false, true), timing);
    bench.exchange->answer_always(HttpResponse{400, "", std::nullopt});
    const auto fail_once = [&](std::uint64_t failures) {
        bench.log(1);
        return eventually([&] { return bench.exporter->failures(OtlpSignal::logs) == failures; });
    };
    ASSERT_TRUE(fail_once(1));
    ASSERT_TRUE(fail_once(2));
    ASSERT_TRUE(fail_once(3));
    std::this_thread::sleep_for(milliseconds(400));
    ASSERT_TRUE(fail_once(4));
    const std::vector<json> warnings = bench.export_failures();
    ASSERT_GE(warnings.size(), 2u);
    EXPECT_EQ(warnings.front().at("attributes").at("praenora.failures"), 1);
    std::int64_t counted = 0;
    for (const json& warning : warnings) {
        counted += warning.at("attributes").at("praenora.failures").get<std::int64_t>();
    }
    EXPECT_EQ(counted, 4) << "every failure is counted in exactly one warning";
}

TEST(OtlpExporter, FlushesOnceWithoutWaiting) {
    std::vector<milliseconds> waits;
    OtlpTiming timing = quick_timing();
    timing.retry_delays = {std::chrono::hours(1), std::chrono::hours(1)};
    timing.on_retry_wait = [&](milliseconds wait) { waits.push_back(wait); };
    Bench bench(settings(true, true), timing);
    auto run = std::make_shared<OtlpRun>("fp");
    run->set_number(1);
    OtlpExporter::Run added = bench.exporter->add_run(run, [] { return snapshot_of(2); }, 1000);
    bench.exchange->answer_always(HttpResponse{503, "", std::nullopt});
    bench.log(5);
    const auto start = Clock::now();
    bench.exporter.reset();
    EXPECT_LT(Clock::now() - start, milliseconds(5000));
    EXPECT_EQ(bench.exchange->sent_to("/v1/logs").size(), 1u);
    ASSERT_EQ(bench.exchange->sent_to("/v1/metrics").size(), 1u);
    EXPECT_EQ(ticks_of(bench.exchange->sent_to("/v1/metrics")[0].body),
              (std::pair<std::string, std::string>{"2", "1"}));
    EXPECT_TRUE(waits.empty());
}

TEST(OtlpExporter, AStopEndsAWaitBetweenAttempts) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.retry_delays = {std::chrono::hours(1), std::chrono::hours(1)};
    Bench bench(settings(false, true), timing);
    bench.exchange->answer_always(HttpResponse{503, "", std::nullopt});
    bench.log(2);
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/logs"));
    const auto start = Clock::now();
    bench.exporter.reset();
    EXPECT_LT(Clock::now() - start, milliseconds(5000));
    const std::vector<SentRequest> sent = bench.exchange->sent();
    ASSERT_EQ(sent.size(), 2u) << "the batch waiting for its retry goes once more, in the flush";
    EXPECT_EQ(record_count(sent[1].body), 2u);
}

TEST(OtlpExporter, ExportsTheMetricsOfARunEveryInterval) {
    OtlpSettings with = settings(true, false);
    with.metric_export_interval = milliseconds(100);
    Bench bench(with);
    auto run = std::make_shared<OtlpRun>("fp");
    std::atomic<std::uint64_t> ticks{0};
    OtlpExporter::Run added =
        bench.exporter->add_run(run, [&] { return snapshot_of(ticks.load()); }, 1000);
    std::this_thread::sleep_for(milliseconds(250));
    EXPECT_TRUE(bench.exchange->sent().empty()) << "a run without its number is not exported";
    run->set_number(3);
    ticks = 5;
    ASSERT_TRUE(bench.exchange->wait_for(2, "/v1/metrics"));
    const std::vector<SentRequest> sent = bench.exchange->sent_to("/v1/metrics");
    EXPECT_EQ(ticks_of(sent[1].body), (std::pair<std::string, std::string>{"5", "3"}));
    EXPECT_EQ(sent[0].headers, (HttpHeaders{{"x-signal", "metrics"}}));
    EXPECT_EQ(sent[0].timeout, milliseconds(200));
}

TEST(OtlpExporter, AFinishedRunSendsItsLastSnapshotAndTheLogs) {
    auto run = std::make_shared<OtlpRun>("fingerprint");
    run->set_number(4);
    Bench bench(settings(true, true), quick_timing(), run);
    std::atomic<int> calls{0};
    OtlpExporter::Run added = bench.exporter->add_run(
        run,
        [&] {
            ++calls;
            return snapshot_of(9);
        },
        1000);
    bench.log(2);
    added.finish();
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/metrics"));
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/logs"));
    EXPECT_EQ(ticks_of(bench.exchange->sent_to("/v1/metrics")[0].body),
              (std::pair<std::string, std::string>{"9", "4"}));
    EXPECT_EQ(record_count(bench.exchange->sent_to("/v1/logs")[0].body), 2u);
    const int after_finish = calls.load();
    added.finish(); // idempotent
    bench.exporter.reset();
    EXPECT_EQ(calls.load(), after_finish) << "a finished run's snapshot is not taken again";
    EXPECT_EQ(bench.exchange->sent_to("/v1/metrics").size(), 1u);
}

TEST(OtlpExporter, RecordsCarryTheirRunsAttributes) {
    auto run = std::make_shared<OtlpRun>("fingerprint");
    run->set_number(5);
    Bench bench(settings(false, true), quick_timing(), run);
    praenora::detail::log_event(*bench.logger, LogLevel::warn, "with a run",
                                {{"praenora.event", "x.y"}, {"praenora.tick", 2}});
    bench.exporter.reset();
    ASSERT_EQ(bench.exchange->sent().size(), 1u);
    const json scope =
        json::parse(bench.exchange->sent()[0].body).at("resourceLogs").at(0).at("scopeLogs").at(0);
    EXPECT_EQ(scope.at("scope").at("name"), "praenora.engine");
    const json& record = scope.at("logRecords").at(0);
    EXPECT_EQ(record.at("severityText"), "WARN");
    EXPECT_EQ(record.at("body").at("stringValue"), "with a run");
    EXPECT_EQ(record.at("attributes"), json::parse(R"([
        {"key": "praenora.event", "value": {"stringValue": "x.y"}},
        {"key": "praenora.tick", "value": {"intValue": "2"}},
        {"key": "praenora.run.number", "value": {"intValue": "5"}},
        {"key": "praenora.run.fingerprint", "value": {"stringValue": "fingerprint"}}])"));
}

TEST(OtlpExporter, ASinkOutlivingItsExporterDropsWhatItGets) {
    Bench bench(settings(false, true));
    bench.exporter.reset();
    bench.log(3); // no crash, nothing sent
    EXPECT_TRUE(bench.exchange->sent().empty());
}

TEST(OtlpExporter, NoSinkWithoutLogExport) {
    Bench bench(settings(true, false));
    EXPECT_EQ(bench.exporter->log_sink(nullptr), nullptr);
}

TEST(OtlpHttp, RetryAfterIsDelaySeconds) {
    using praenora::detail::parse_retry_after;
    EXPECT_EQ(parse_retry_after("7"), std::optional<std::int64_t>(7));
    EXPECT_EQ(parse_retry_after(" 120 "), std::optional<std::int64_t>(120));
    EXPECT_EQ(parse_retry_after("0"), std::optional<std::int64_t>(0));
    EXPECT_FALSE(parse_retry_after("Wed, 21 Oct 2015 07:28:00 GMT"));
    EXPECT_FALSE(parse_retry_after("-1"));
    EXPECT_FALSE(parse_retry_after("1.5"));
    EXPECT_FALSE(parse_retry_after(""));
    EXPECT_EQ(parse_retry_after("99999999999999999999"),
              std::optional<std::int64_t>(std::numeric_limits<std::int64_t>::max()))
        << "a huge delay is as long as the longest; the exporter waits 30 s at most";
}

// A collector that never answers: the request in flight when the exporter ends fails within its
// timeout and is its signal's attempt at the end, never sent again, and the flush has no time left
// for the logs. The fake transport holds the request until the destructor has asked the thread to
// stop (OtlpTiming::on_stop), then lets it wait its whole timeout, so the stop always comes while
// the request is in flight, however the threads are scheduled. Before, the end posted the metrics
// again and then the logs, each with a whole timeout: three timeouts in all.
TEST(OtlpExporter, TheEndTakesOneExportTimeoutAtMost) {
    OtlpSettings with = settings(true, true);
    with.metrics.timeout = milliseconds(300);
    with.logs.timeout = milliseconds(300);
    OtlpTiming timing = quick_timing();
    timing.retry_delays = {std::chrono::hours(1), std::chrono::hours(1)};
    std::shared_ptr<Exchange> held; // the bench's exchange, set below, before the end
    timing.on_stop = [&held] { held->release(); };
    Bench bench(with, timing);
    held = bench.exchange;
    bench.exchange->hangs = true;
    bench.exchange->hold();
    auto run = std::make_shared<OtlpRun>("fp");
    run->set_number(1);
    OtlpExporter::Run added = bench.exporter->add_run(run, [] { return snapshot_of(1); }, 1000);
    bench.log(3); // they wait: the batch delay is long
    added.finish();
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/metrics")); // in flight, held
    const auto start = Clock::now();
    bench.exporter.reset();
    const auto took = Clock::now() - start;
    EXPECT_LT(took, milliseconds(600)) << std::chrono::duration_cast<milliseconds>(took).count()
                                       << " ms: about one export timeout, not two or three";
    EXPECT_EQ(bench.exchange->sent_to("/v1/metrics").size(), 1u)
        << "the request that failed at the end is not sent again";
    EXPECT_TRUE(bench.exchange->sent_to("/v1/logs").empty())
        << "the request in flight took the end's whole timeout: none was left for the logs";
}

// The flush alone: each attempt gets what is left of one export timeout, and an attempt for which
// nothing is left is not made.
TEST(OtlpExporter, TheFlushSharesOneExportTimeout) {
    OtlpSettings with = settings(true, true);
    with.metrics.timeout = milliseconds(250);
    with.logs.timeout = milliseconds(250);
    Bench bench(with);
    bench.exchange->hangs = true;
    auto run = std::make_shared<OtlpRun>("fp");
    run->set_number(1);
    OtlpExporter::Run added = bench.exporter->add_run(run, [] { return snapshot_of(1); }, 1000);
    bench.log(2);
    const auto start = Clock::now();
    bench.exporter.reset();
    const auto took = Clock::now() - start;
    EXPECT_LT(took, milliseconds(500)) << std::chrono::duration_cast<milliseconds>(took).count();
    ASSERT_EQ(bench.exchange->sent_to("/v1/metrics").size(), 1u);
    EXPECT_LE(bench.exchange->sent_to("/v1/metrics")[0].timeout, milliseconds(250));
    EXPECT_TRUE(bench.exchange->sent_to("/v1/logs").empty())
        << "the metrics took the whole timeout: no time was left for the logs";
}

TEST(OtlpExporter, ABatchPutBackKeepsTheQueuesLimit) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.max_batch = 4;
    timing.max_queue = 4;
    timing.retry_delays = {std::chrono::hours(1), std::chrono::hours(1)};
    Bench bench(settings(true, true), timing);
    auto run = std::make_shared<OtlpRun>("fp");
    run->set_number(1);
    OtlpExporter::Run added = bench.exporter->add_run(run, [] { return snapshot_of(1); }, 1000);
    bench.exchange->answer_always(HttpResponse{503, "", std::nullopt});
    bench.log(4);
    ASSERT_TRUE(bench.exchange->wait_for(1, "/v1/logs")); // then it waits an hour to retry
    bench.log(5);                                         // four fit, one is dropped
    EXPECT_EQ(bench.exporter->dropped_log_records(), 1u);
    bench.exporter.reset(); // the stop puts the batch back: the queue is full, so it is dropped
    const std::vector<SentRequest> logs = bench.exchange->sent_to("/v1/logs");
    ASSERT_EQ(logs.size(), 2u);
    EXPECT_EQ(record_count(logs[1].body), 4u) << "never more than the queue's limit";
    const std::vector<SentRequest> metrics = bench.exchange->sent_to("/v1/metrics");
    ASSERT_EQ(metrics.size(), 1u);
    const json document = json::parse(metrics[0].body);
    std::string dropped = "none";
    for (const json& metric :
         document.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics")) {
        if (metric.at("name") == "praenora.telemetry.log_records.dropped") {
            dropped = metric.at("sum").at("dataPoints").at(0).at("asInt");
        }
    }
    EXPECT_EQ(dropped, "5") << "the one beyond the limit and the four of the batch put back";
}

TEST(OtlpExporter, AnErrorOnItsThreadIsCountedAndNotFatal) {
    OtlpTiming timing = quick_timing();
    timing.batch_delay = milliseconds(1);
    timing.max_batch = 1;
    std::atomic<int> thrown{0};
    timing.on_retry_wait = [&](milliseconds) {
        ++thrown;
        throw std::runtime_error("an error on the exporter's thread");
    };
    Bench bench(settings(false, true), timing);
    bench.exchange->answer({HttpResponse{503, "", std::nullopt}});
    bench.log(1);
    ASSERT_TRUE(eventually([&] { return bench.exporter->failures(OtlpSignal::logs) == 1; }));
    EXPECT_EQ(thrown.load(), 1);
    bench.log(1); // the thread still runs
    ASSERT_TRUE(bench.exchange->wait_for(2, "/v1/logs"));
    const std::vector<json> warnings = bench.export_failures();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].at("attributes").at("praenora.signal"), "logs");
}

// A module's message that is not UTF-8, logged on a run's logger, goes through the hub's sink and
// the queue into the request of the destructor's flush (the batch delay is long), which a
// collector parses, with the bytes replaced.
TEST(OtlpExporter, ARecordThatIsNotUtf8StillMakesAValidRequest) {
    auto run = std::make_shared<OtlpRun>("fp");
    run->set_number(2);
    Bench bench(settings(false, true), quick_timing(), run);
    praenora::detail::log_event(*bench.records.logger("module.scribble"), LogLevel::warn,
                                std::string("a\xff b"),
                                {{"praenora.text", std::string("c\xfe"
                                                               "d")}});
    bench.exporter.reset();
    ASSERT_EQ(bench.exchange->sent().size(), 1u);
    json document;
    ASSERT_NO_THROW(document = json::parse(bench.exchange->sent()[0].body));
    const json scope = document.at("resourceLogs").at(0).at("scopeLogs").at(0);
    EXPECT_EQ(scope.at("scope").at("name"), "module.scribble");
    const json& record = scope.at("logRecords").at(0);
    EXPECT_EQ(record.at("body").at("stringValue"), "a\xEF\xBF\xBD b");
    EXPECT_EQ(record.at("attributes").at(0).at("key"), "praenora.text");
    EXPECT_EQ(record.at("attributes").at(0).at("value").at("stringValue"), "c\xEF\xBF\xBD"
                                                                           "d");
}
