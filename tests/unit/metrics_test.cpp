// The metrics registry, status snapshots, health, the status-snapshot JSON document and the
// metrics export.
#include "observability/metrics.hpp"
#include "observability/status_json.hpp"

#include "core/semver.hpp"
#include "core/time.hpp"
#include "document/schema_registry.hpp"
#include "instance/module_instance.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using nlohmann::json;
using praenora::ErrorCode;
using praenora::ErrorRecord;
using praenora::Health;
using praenora::StatusSnapshot;
using praenora::StepState;
using praenora::StepStatus;
using praenora::WorkflowState;
using praenora::detail::CallOutcome;
using praenora::detail::compute_health;
using praenora::detail::metrics_json;
using praenora::detail::MetricsRegistry;

namespace {

const std::string kFingerprint(64, 'a');

// A registry for test.chain with steps a (fx-echo) and b (fx-slow).
void prepare(MetricsRegistry& metrics) {
    metrics.set_run_info("test.chain", "0.1.0", kFingerprint);
    metrics.register_step("a", "fx-echo", "0.1.0");
    metrics.register_step("b", "fx-slow", "0.1.0");
}

const StepStatus& step(const StatusSnapshot& snapshot, const std::string& id) {
    for (const auto& status : snapshot.steps) {
        if (status.step == id) {
            return status;
        }
    }
    throw std::runtime_error("no step " + id);
}

ErrorRecord error_record(ErrorCode code, std::string message, std::optional<std::uint64_t> tick,
                         std::int64_t at, std::string cause = {}) {
    ErrorRecord record;
    record.code = code;
    record.message = std::move(message);
    record.tick = tick;
    record.at_unix_ns = at;
    record.cause_json = std::move(cause);
    return record;
}

void expect_valid_status_document(const json& document) {
    const auto& schema = praenora::detail::SchemaRegistry::builtin().for_kind(
        "status-snapshot", praenora::detail::parse_semver("0.1.0"));
    for (const auto& issue : schema.validate(document)) {
        ADD_FAILURE() << issue.pointer << ": " << issue.message << "\n" << document.dump(2);
    }
}

} // namespace

TEST(Metrics, CountsOutcomes) {
    MetricsRegistry metrics;
    prepare(metrics);
    const std::int64_t at = 1700000000000000000;
    for (int i = 0; i < 3; ++i) {
        metrics.record_invocation("a", CallOutcome::succeeded, 1ms, at, std::nullopt);
    }
    metrics.record_invocation("a", CallOutcome::failed, 1ms, at, std::nullopt);
    metrics.record_invocation("a", CallOutcome::failed, 1ms, at, std::nullopt);
    metrics.record_invocation("a", CallOutcome::timed_out, 1ms, at, std::nullopt);
    metrics.record_invocation("a", CallOutcome::cancelled, 1ms, at, std::nullopt);
    // An unresponsive call passed its deadline too: it counts as timed out.
    metrics.record_invocation("a", CallOutcome::unresponsive, 1ms, at, std::nullopt);
    // A call whose module host crashed counts as failed.
    metrics.record_invocation("a", CallOutcome::crashed, 1ms, at, std::nullopt);
    const auto snapshot = metrics.snapshot();
    const auto& a = step(snapshot, "a");
    EXPECT_EQ(a.invocations, 9u);
    EXPECT_EQ(a.succeeded, 3u);
    EXPECT_EQ(a.failed, 3u);
    EXPECT_EQ(a.timed_out, 2u);
    EXPECT_EQ(a.cancelled, 1u);
    const auto& b = step(snapshot, "b");
    EXPECT_EQ(b.invocations, 0u);
    EXPECT_EQ(b.module_name, "fx-slow");
    EXPECT_EQ(b.module_version, "0.1.0");
    EXPECT_EQ(b.state, StepState::loaded);
    // Steps are reported in the order they were registered.
    ASSERT_EQ(snapshot.steps.size(), 2u);
    EXPECT_EQ(snapshot.steps[0].step, "a");
    EXPECT_EQ(snapshot.steps[1].step, "b");
    // A step must be registered first.
    try {
        metrics.record_invocation("zz", CallOutcome::succeeded, 1ms, at, std::nullopt);
        ADD_FAILURE() << "an unregistered step was accepted";
    } catch (const praenora::Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::INVALID_ARGUMENT);
    }
}

TEST(Metrics, StepErrorWithoutInvocation) {
    // A step that fails outside an invocation (loading, create, initialize, finalize) gets a last
    // error without anything being counted; the workflow's last error is separate.
    MetricsRegistry metrics;
    prepare(metrics);
    metrics.set_step_error("b", error_record(ErrorCode::STEP_FAILED, "b: initialize failed",
                                             std::nullopt, 1'700'000'000'000'000'000,
                                             R"({"code":11,"message":"no"})"));
    const StatusSnapshot snapshot = metrics.snapshot();
    const StepStatus& b = step(snapshot, "b");
    ASSERT_TRUE(b.last_error.has_value());
    EXPECT_EQ(b.last_error->code, ErrorCode::STEP_FAILED);
    EXPECT_EQ(b.last_error->message, "b: initialize failed");
    EXPECT_EQ(b.last_error->cause_json, R"({"code":11,"message":"no"})");
    EXPECT_EQ(b.invocations, 0u);
    EXPECT_EQ(b.failed, 0u);
    EXPECT_FALSE(b.last_invocation_at_unix_ns.has_value());
    EXPECT_FALSE(step(snapshot, "a").last_error.has_value());
    EXPECT_FALSE(snapshot.last_error.has_value());
    try {
        metrics.set_step_error("unknown", error_record(ErrorCode::STEP_FAILED, "x", 0, 0));
        ADD_FAILURE() << "an unregistered step was accepted";
    } catch (const praenora::Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
    }
}

TEST(Metrics, DurationStatistics) {
    MetricsRegistry metrics;
    prepare(metrics);
    {
        const auto before = metrics.snapshot();
        const auto& fresh = step(before, "a");
        EXPECT_EQ(fresh.last_duration_ns, 0);
        EXPECT_EQ(fresh.min_duration_ns, 0);
        EXPECT_EQ(fresh.mean_duration_ns, 0.0);
    }
    const std::int64_t at = 1700000000000000000;
    metrics.record_invocation("a", CallOutcome::succeeded, 30ns, at, std::nullopt);
    metrics.record_invocation("a", CallOutcome::failed, 10ns, at, std::nullopt);
    metrics.record_invocation("a", CallOutcome::succeeded, 20ns, at, std::nullopt);
    const auto snapshot = metrics.snapshot();
    const auto& a = step(snapshot, "a");
    EXPECT_EQ(a.last_duration_ns, 20);
    EXPECT_EQ(a.total_duration_ns, 60);
    EXPECT_EQ(a.min_duration_ns, 10);
    EXPECT_EQ(a.max_duration_ns, 30);
    EXPECT_DOUBLE_EQ(a.mean_duration_ns, 20.0);
}

TEST(Metrics, LastSuccessTimestampOnlyOnSuccess) {
    MetricsRegistry metrics;
    prepare(metrics);
    EXPECT_FALSE(step(metrics.snapshot(), "a").last_success_at_unix_ns.has_value());
    metrics.record_invocation("a", CallOutcome::succeeded, 1ms, 1000, std::nullopt);
    metrics.record_invocation(
        "a", CallOutcome::failed, 1ms, 2000,
        error_record(ErrorCode::STEP_FAILED, "a failed", 1, 2000, R"({"code":11})"));
    metrics.record_invocation("a", CallOutcome::timed_out, 1ms, 3000,
                              error_record(ErrorCode::STEP_TIMEOUT, "a timed out", 2, 3000));
    const auto snapshot = metrics.snapshot();
    const auto& a = step(snapshot, "a");
    ASSERT_TRUE(a.last_success_at_unix_ns.has_value());
    EXPECT_EQ(*a.last_success_at_unix_ns, 1000);
    ASSERT_TRUE(a.last_invocation_at_unix_ns.has_value());
    EXPECT_EQ(*a.last_invocation_at_unix_ns, 3000);
    ASSERT_TRUE(a.last_error.has_value());
    EXPECT_EQ(a.last_error->code, ErrorCode::STEP_TIMEOUT);
    // A later success keeps the last error: it is the last one, not a current one.
    metrics.record_invocation("a", CallOutcome::succeeded, 1ms, 4000, std::nullopt);
    const auto later = metrics.snapshot();
    EXPECT_EQ(*step(later, "a").last_success_at_unix_ns, 4000);
    EXPECT_TRUE(step(later, "a").last_error.has_value());
}

TEST(Metrics, WorkflowProgressAndUptime) {
    MetricsRegistry metrics;
    prepare(metrics);
    auto snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.state, WorkflowState::created);
    EXPECT_FALSE(snapshot.started_at_unix_ns.has_value());
    EXPECT_EQ(snapshot.uptime_ns, 0);
    EXPECT_FALSE(snapshot.current_tick.has_value());
    EXPECT_FALSE(snapshot.logical_time.has_value());
    EXPECT_EQ(snapshot.workflow_name, "test.chain");
    EXPECT_EQ(snapshot.workflow_version, "0.1.0");
    EXPECT_EQ(snapshot.fingerprint, kFingerprint);
    EXPECT_EQ(snapshot.runtime_version, praenora::version_info().runtime);
    EXPECT_EQ(snapshot.pid, praenora::detail::current_process_id());
    EXPECT_GT(snapshot.generated_at_unix_ns, 1700000000000000000);

    metrics.mark_started(1700000000000000000);
    metrics.set_workflow_state(WorkflowState::running);
    metrics.set_step_state("a", StepState::ready);
    metrics.record_tick(0, 1.0, 3ms);
    metrics.record_tick(1, 2.0, 5ms);
    std::this_thread::sleep_for(2ms);
    snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.state, WorkflowState::running);
    EXPECT_EQ(*snapshot.started_at_unix_ns, 1700000000000000000);
    EXPECT_GE(snapshot.uptime_ns, std::chrono::nanoseconds(2ms).count());
    EXPECT_EQ(snapshot.ticks_completed, 2u);
    EXPECT_EQ(*snapshot.current_tick, 1u);
    EXPECT_EQ(*snapshot.logical_time, 2.0);
    EXPECT_EQ(snapshot.last_tick_duration_ns, std::chrono::nanoseconds(5ms).count());
    EXPECT_EQ(step(snapshot, "a").state, StepState::ready);

    // Uptime stops when the run ends.
    metrics.set_workflow_state(WorkflowState::completed);
    const auto ended = metrics.snapshot().uptime_ns;
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(metrics.snapshot().uptime_ns, ended);
}

TEST(Metrics, SnapshotIsConsistentUnderConcurrency) {
    MetricsRegistry metrics;
    prepare(metrics);
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 2500; // 10k operations in all
    std::atomic<int> running{kWriters};
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&metrics, &running, w] {
            const std::string id = w % 2 == 0 ? "a" : "b";
            for (int i = 0; i < kPerWriter; ++i) {
                const auto outcome = static_cast<CallOutcome>(i % 4);
                metrics.record_invocation(id, outcome, std::chrono::nanoseconds(i % 7 + 1), i,
                                          std::nullopt);
                if (i % 100 == 0) {
                    metrics.set_step_state(id, StepState::busy);
                    metrics.record_tick(static_cast<std::uint64_t>(i), i * 0.5, 1us);
                }
            }
            --running;
        });
    }
    std::uint64_t last_seen = 0;
    int snapshots = 0;
    while (running.load() > 0 || snapshots == 0) {
        const auto snapshot = metrics.snapshot();
        std::uint64_t total = 0;
        for (const auto& status : snapshot.steps) {
            EXPECT_EQ(status.invocations,
                      status.succeeded + status.failed + status.timed_out + status.cancelled);
            EXPECT_GE(status.total_duration_ns, static_cast<std::int64_t>(status.invocations));
            if (status.invocations > 0) {
                EXPECT_LE(status.min_duration_ns, status.max_duration_ns);
                EXPECT_NEAR(status.mean_duration_ns,
                            static_cast<double>(status.total_duration_ns) /
                                static_cast<double>(status.invocations),
                            1e-6);
            }
            total += status.invocations;
        }
        EXPECT_GE(total, last_seen);
        last_seen = total;
        (void)snapshot.to_json();
        ++snapshots;
    }
    for (auto& writer : writers) {
        writer.join();
    }
    const auto final_snapshot = metrics.snapshot();
    std::int64_t expected_duration = 0;
    for (int i = 0; i < kPerWriter; ++i) {
        expected_duration += i % 7 + 1;
    }
    for (const auto& status : final_snapshot.steps) {
        EXPECT_EQ(status.invocations, 2u * kPerWriter);
        EXPECT_EQ(status.succeeded, 2u * kPerWriter / 4);
        EXPECT_EQ(status.failed, 2u * kPerWriter / 4);
        EXPECT_EQ(status.timed_out, 2u * kPerWriter / 4);
        EXPECT_EQ(status.cancelled, 2u * kPerWriter / 4);
        EXPECT_EQ(status.total_duration_ns, 2 * expected_duration);
        EXPECT_EQ(status.min_duration_ns, 1);
        EXPECT_EQ(status.max_duration_ns, 7);
    }
    EXPECT_GT(snapshots, 0);
}

TEST(Health, Rules) {
    const auto with = [](WorkflowState state, auto&& change) {
        StatusSnapshot snapshot;
        snapshot.state = state;
        StepStatus status;
        status.step = "a";
        status.module_name = "fx-echo";
        status.module_version = "0.1.0";
        status.state = StepState::ready;
        snapshot.steps.push_back(status);
        change(snapshot);
        return compute_health(snapshot);
    };
    const auto unchanged = [](StatusSnapshot&) {};
    struct Case {
        const char* name;
        Health expected;
        Health actual;
    };
    const Case cases[] = {
        {"created", Health::unknown, with(WorkflowState::created, unchanged)},
        {"starting", Health::unknown, with(WorkflowState::starting, unchanged)},
        {"ready", Health::healthy, with(WorkflowState::ready, unchanged)},
        {"running", Health::healthy, with(WorkflowState::running, unchanged)},
        {"completed", Health::healthy, with(WorkflowState::completed, unchanged)},
        {"after a timeout", Health::degraded,
         with(WorkflowState::running, [](StatusSnapshot& s) { s.steps[0].timed_out = 1; })},
        {"after a cancellation", Health::degraded,
         with(WorkflowState::running, [](StatusSnapshot& s) { s.steps[0].cancelled = 1; })},
        {"after a counted failure", Health::degraded,
         with(WorkflowState::completed, [](StatusSnapshot& s) { s.steps[0].failed = 1; })},
        {"stopping", Health::degraded, with(WorkflowState::stopping, unchanged)},
        {"stopped", Health::degraded, with(WorkflowState::stopped, unchanged)},
        {"after a status-file write error", Health::degraded,
         with(WorkflowState::running,
              [](StatusSnapshot& s) {
                  s.last_error = error_record(ErrorCode::IO_ERROR, "cannot write", {}, 1);
              })},
        {"failed", Health::unhealthy, with(WorkflowState::failed, unchanged)},
        {"failed after a timeout", Health::unhealthy,
         with(WorkflowState::failed, [](StatusSnapshot& s) { s.steps[0].timed_out = 1; })},
        {"an instance failed", Health::unhealthy,
         with(WorkflowState::running,
              [](StatusSnapshot& s) { s.steps[0].state = StepState::failed; })},
        {"an instance is unresponsive", Health::unhealthy,
         with(WorkflowState::stopping,
              [](StatusSnapshot& s) { s.steps[0].state = StepState::unresponsive; })},
        {"an instance crashed", Health::unhealthy,
         with(WorkflowState::stopping,
              [](StatusSnapshot& s) { s.steps[0].state = StepState::crashed; })},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(praenora::to_string(c.actual), praenora::to_string(c.expected)) << c.name;
    }
    // Snapshots carry their health.
    MetricsRegistry metrics;
    prepare(metrics);
    EXPECT_EQ(metrics.snapshot().health, Health::unknown);
    metrics.set_workflow_state(WorkflowState::running);
    EXPECT_EQ(metrics.snapshot().health, Health::healthy);
    metrics.record_invocation("b", CallOutcome::timed_out, 1ms, 1, std::nullopt);
    EXPECT_EQ(metrics.snapshot().health, Health::degraded);
    metrics.set_workflow_state(WorkflowState::failed);
    EXPECT_EQ(metrics.snapshot().health, Health::unhealthy);
}

TEST(StatusJson, ValidatesAgainstSchema) {
    // Before anything happened.
    MetricsRegistry fresh;
    fresh.set_run_info("test.chain", "0.1.0", "");
    const json minimal = json::parse(fresh.snapshot().to_json());
    expect_valid_status_document(minimal);
    EXPECT_EQ(minimal.at("kind"), "status-snapshot");
    EXPECT_EQ(minimal.at("format_version"), "0.1.0");
    EXPECT_EQ(minimal.at("steps"), json::array());
    const json& idle = minimal.at("workflow");
    EXPECT_EQ(idle.at("state"), "created");
    EXPECT_EQ(idle.at("health"), "unknown");
    EXPECT_EQ(idle.at("started_at"), nullptr);
    EXPECT_EQ(idle.at("current_tick"), nullptr);
    EXPECT_EQ(idle.at("logical_time"), nullptr);
    EXPECT_EQ(idle.at("last_error"), nullptr);

    // In the middle of a run with errors.
    MetricsRegistry metrics;
    prepare(metrics);
    const std::int64_t started = 1700000000123456789;
    metrics.mark_started(started);
    metrics.set_workflow_state(WorkflowState::running);
    metrics.set_step_state("a", StepState::ready);
    metrics.set_step_state("b", StepState::busy);
    metrics.record_invocation("a", CallOutcome::succeeded, 1500ns, started + 10, std::nullopt);
    metrics.record_invocation("a", CallOutcome::failed, 2500ns, started + 20,
                              error_record(ErrorCode::STEP_FAILED, "a failed: r\xC3\xA9sum\xC3\xA9",
                                           3, started + 20,
                                           R"({"code":10001,"message":"bad row","details":{}})"));
    metrics.record_invocation(
        "b", CallOutcome::failed, 1ns, started + 30,
        error_record(ErrorCode::STEP_FAILED, "not JSON", std::nullopt, 0, "{oops"));
    metrics.record_tick(3, 4.0, 1200us);
    metrics.set_last_error(error_record(ErrorCode::IO_ERROR, "cannot write status", {}, started));
    const auto snapshot = metrics.snapshot();
    const json document = json::parse(snapshot.to_json());
    expect_valid_status_document(document);

    const std::regex rfc3339(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z$)");
    EXPECT_TRUE(std::regex_match(document.at("generated_at").get<std::string>(), rfc3339));
    EXPECT_EQ(document.at("runtime_version"), praenora::version_info().runtime);
    EXPECT_EQ(document.at("pid"), praenora::detail::current_process_id());
    const json& workflow = document.at("workflow");
    EXPECT_EQ(workflow.at("name"), "test.chain");
    EXPECT_EQ(workflow.at("fingerprint"), kFingerprint);
    EXPECT_EQ(workflow.at("state"), "running");
    EXPECT_EQ(workflow.at("health"), "degraded");
    EXPECT_EQ(workflow.at("started_at"), "2023-11-14T22:13:20.123456789Z");
    EXPECT_EQ(workflow.at("ticks_completed"), 4);
    EXPECT_EQ(workflow.at("current_tick"), 3);
    EXPECT_EQ(workflow.at("logical_time"), 4.0);
    EXPECT_EQ(workflow.at("last_tick_duration_ns"), 1200000);
    EXPECT_EQ(workflow.at("last_error").at("name"), "IO_ERROR");
    EXPECT_EQ(workflow.at("last_error").at("code"), 9);

    const json& a = document.at("steps").at(0);
    EXPECT_EQ(a.at("step"), "a");
    EXPECT_EQ(a.at("state"), "ready");
    EXPECT_EQ(a.at("invocations"), 2);
    EXPECT_EQ(a.at("total_duration_ns"), 4000);
    EXPECT_EQ(a.at("mean_duration_ns"), 2000.0);
    EXPECT_EQ(a.at("last_success_at"), "2023-11-14T22:13:20.123456799Z");
    json expected_error = json::parse(R"({"code":700,"name":"STEP_FAILED","tick":3,
                                          "at":"2023-11-14T22:13:20.123456809Z",
                                          "cause":{"code":10001,"message":"bad row",
                                                   "details":{}},
                                          "location":null,"details":null})");
    expected_error["message"] = "a failed: r\xC3\xA9sum\xC3\xA9";
    EXPECT_EQ(a.at("last_error"), expected_error);
    const json& b = document.at("steps").at(1);
    EXPECT_EQ(b.at("state"), "busy");
    EXPECT_EQ(b.at("last_success_at"), nullptr);
    // A cause that is not a JSON object is kept as text; an unset time is null.
    EXPECT_EQ(b.at("last_error").at("cause"), json::parse(R"({"text":"{oops"})"));
    EXPECT_EQ(b.at("last_error").at("at"), nullptr);
    EXPECT_EQ(b.at("last_error").at("tick"), nullptr);

    // A step whose module host crashed.
    metrics.set_step_state("b", StepState::crashed);
    const json crashed = json::parse(metrics.snapshot().to_json());
    expect_valid_status_document(crashed);
    EXPECT_EQ(crashed.at("steps").at(1).at("state"), "crashed");
}

TEST(StatusJson, ErrorRecordKeepsTheLocationAndDetailsOfItsError) {
    using praenora::Error;
    using praenora::SourceLocation;
    using praenora::detail::error_record_json;
    using praenora::detail::error_record_of;

    SourceLocation where;
    where.file = "out/outputs/result.jsonl";
    where.line = 4;
    where.column = 2;
    where.pointer = "/data/0";
    const Error error(ErrorCode::IO_ERROR, "cannot write 'result'", where,
                      R"({"path":"out/outputs/result.jsonl","bytes":12})");
    const std::int64_t at = 1700000000123456789;
    const ErrorRecord record = error_record_of(error, 7, at);
    EXPECT_EQ(record.code, ErrorCode::IO_ERROR);
    EXPECT_EQ(record.message, "cannot write 'result'");
    EXPECT_EQ(record.tick, std::optional<std::uint64_t>(7));
    EXPECT_EQ(record.at_unix_ns, at);
    EXPECT_EQ(record.cause_json, "");
    EXPECT_EQ(record.location.file, where.file);
    EXPECT_EQ(record.location.line, 4);
    EXPECT_EQ(record.location.column, 2);
    EXPECT_EQ(record.location.pointer, "/data/0");
    EXPECT_EQ(record.details_json, error.details_json());

    EXPECT_EQ(error_record_json(record),
              json::parse(R"({"code":9,"name":"IO_ERROR","message":"cannot write 'result'",
                              "tick":7,"at":"2023-11-14T22:13:20.123456789Z","cause":null,
                              "location":{"file":"out/outputs/result.jsonl","line":4,
                                          "column":2,"pointer":"/data/0"},
                              "details":{"path":"out/outputs/result.jsonl","bytes":12}})"));

    // Only the parts of a location that are set are written (a column only with a line), and a
    // location with none is null; so are details that are empty or an empty object, while text
    // that is not a JSON object is kept as text.
    const auto shown = [](SourceLocation location, std::string details) {
        ErrorRecord partial;
        partial.code = ErrorCode::INTERNAL;
        partial.location = std::move(location);
        partial.details_json = std::move(details);
        const json document = error_record_json(partial);
        return std::make_pair(document.at("location"), document.at("details"));
    };
    SourceLocation column_only;
    column_only.column = 3;
    column_only.pointer = "/a";
    EXPECT_EQ(shown(column_only, "{}"), std::make_pair(json({{"pointer", "/a"}}), json(nullptr)));
    EXPECT_EQ(shown({}, ""), std::make_pair(json(nullptr), json(nullptr)));
    EXPECT_EQ(shown({}, "[1]"), std::make_pair(json(nullptr), json({{"text", "[1]"}})));
    EXPECT_EQ(shown({}, "{oops"), std::make_pair(json(nullptr), json({{"text", "{oops"}})));
    // A record built by hand (not from an Error) has neither.
    EXPECT_EQ(shown(ErrorRecord{}.location, ErrorRecord{}.details_json),
              std::make_pair(json(nullptr), json(nullptr)));

    // Both appear in the status snapshot, the workflow's last error and a step's alike.
    MetricsRegistry metrics;
    prepare(metrics);
    metrics.set_step_error("a", record);
    metrics.set_last_error(record);
    const json document = json::parse(metrics.snapshot().to_json());
    expect_valid_status_document(document);
    EXPECT_EQ(document.at("workflow").at("last_error"), error_record_json(record));
    EXPECT_EQ(document.at("steps").at(0).at("last_error"), error_record_json(record));
}

TEST(MetricsJson, HasOtelStyleNames) {
    MetricsRegistry metrics;
    prepare(metrics);
    metrics.mark_started(1700000000000000000);
    metrics.set_workflow_state(WorkflowState::running);
    metrics.record_invocation("a", CallOutcome::succeeded, 30ns, 1, std::nullopt);
    metrics.record_invocation("a", CallOutcome::succeeded, 10ns, 2, std::nullopt);
    metrics.record_invocation("a", CallOutcome::timed_out, 20ns, 3, std::nullopt);
    metrics.record_tick(0, 1.0, 7us);
    const json document = json::parse(metrics_json(metrics.snapshot()));
    ASSERT_TRUE(document.at("metrics").is_array());

    const std::regex otel_name(R"(^praenora(\.[a-z][a-z0-9_]*)+$)");
    std::set<std::string> names;
    for (const json& point : document.at("metrics")) {
        const std::string name = point.at("name").get<std::string>();
        names.insert(name);
        EXPECT_TRUE(std::regex_match(name, otel_name)) << name;
        EXPECT_TRUE(point.at("unit").is_string()) << point.dump();
        const std::string kind = point.at("kind").get<std::string>();
        EXPECT_TRUE(kind == "counter" || kind == "gauge" || kind == "summary") << point.dump();
        EXPECT_TRUE(point.at("attributes").is_object()) << point.dump();
        EXPECT_NE(point.contains("value"), point.contains("summary")) << point.dump();
    }
    EXPECT_EQ(names, (std::set<std::string>{
                         "praenora.step.invocations", "praenora.step.invocation.duration",
                         "praenora.workflow.ticks", "praenora.workflow.tick.duration",
                         "praenora.workflow.uptime"}));

    // One invocation counter per step and outcome, zeros included.
    std::map<std::string, json> counters;
    json summary_a;
    for (const json& point : document.at("metrics")) {
        const json& attributes = point.at("attributes");
        if (point.at("name") == "praenora.step.invocations") {
            EXPECT_EQ(point.at("kind"), "counter");
            counters[attributes.at("step").get<std::string>() + "/" +
                     attributes.at("outcome").get<std::string>()] = point.at("value");
            EXPECT_EQ(attributes.at("module"),
                      attributes.at("step") == "a" ? "fx-echo@0.1.0" : "fx-slow@0.1.0");
        }
        if (point.at("name") == "praenora.step.invocation.duration" &&
            attributes.at("step") == "a") {
            EXPECT_EQ(point.at("unit"), "ns");
            EXPECT_EQ(point.at("kind"), "summary");
            summary_a = point.at("summary");
        }
        if (point.at("name") == "praenora.workflow.ticks") {
            EXPECT_EQ(point.at("value"), 1);
        }
        if (point.at("name") == "praenora.workflow.tick.duration") {
            EXPECT_EQ(point.at("value"), 7000);
        }
    }
    EXPECT_EQ(counters.size(), 8u);
    EXPECT_EQ(counters.at("a/succeeded"), 2);
    EXPECT_EQ(counters.at("a/timed_out"), 1);
    EXPECT_EQ(counters.at("a/failed"), 0);
    EXPECT_EQ(counters.at("b/cancelled"), 0);
    EXPECT_EQ(summary_a, json::parse(R"({"count":3,"sum":60,"min":10,"max":30,"last":20})"));
}
