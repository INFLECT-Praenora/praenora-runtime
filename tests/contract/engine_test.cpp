// The execution engine: fixture workflows (test.single, test.loop, test.chain) of the test catalog
// run with fixture modules, through validate_workflow(). Covers envelopes, message routing (delay 0
// and delay 1), the checks of module results, fail-fast failures with cleanup in reverse order,
// timeouts, unresponsive modules, stop requests, and the files a run writes (run record, outputs
// and message trace).
#include "engine/engine.hpp"
#include "engine/envelope.hpp"

#include "api/validation.hpp"
#include "catalog/catalog.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "instance/services.hpp"
#include "observability/metrics.hpp"

#include "instance_fixtures.hpp"
#include "module_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Health;
using praenora::RunSummary;
using praenora::StatusSnapshot;
using praenora::StepState;
using praenora::StepStatus;
using praenora::TickResult;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::detail::Engine;
using praenora::detail::EngineSettings;
using praenora::detail::MetricsRegistry;
using praenora::detail::ServiceRegistry;
using praenora::test::CapturedLogs;
using praenora::test::ScratchDir;
using praenora::test::TestGate;
using praenora::test::write_text_file;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The praenora::Error that `call` throws; fails the test when it throws none.
template <class Call> Error error_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "no praenora::Error was thrown";
    return Error(ErrorCode::OK, "nothing thrown");
}

json details_of(const Error& error) { return json::parse(error.details_json()); }

// A configuration of `workflow` with the given sections (each a complete YAML block).
std::string config(const std::string& workflow, const std::string& run, const std::string& inputs,
                   const std::string& steps,
                   const std::string& outputs = "outputs: { directory: out, trace_messages: "
                                                "true }\n") {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: " +
           workflow + ", version: \"0.1.0\" }\n" + run + inputs + steps + outputs;
}

// The json-document input `source` of test.single and test.chain (source.json, written by
// EngineFixture::open()).
const char* const kJsonSource = "inputs:\n"
                                "  source: { source: json-document, path: source.json }\n";

// A step block binding `step` to fixture module `module` with `parameters`, plus `extra` lines
// (indented by four spaces).
std::string step(const std::string& id, const std::string& module,
                 const std::string& parameters = "{}", const std::string& extra = "") {
    return "  " + id + ":\n    module: { name: " + module +
           ", version: \"0.1.0\" }\n    parameters: " + parameters + "\n" + extra;
}

// test.single (inputs.source -> a -> outputs.result), three ticks, with step a bound to `module`.
std::string single(const std::string& module, const std::string& parameters = "{}",
                   const std::string& extra = "") {
    return config("test.single", "run: { ticks: 3, seed: 7 }\n", kJsonSource,
                  "steps:\n" + step("a", module, parameters, extra));
}

// Everything an engine test uses: a scratch directory holding the configuration and its data, the
// captured logs, the metrics and the engine itself.
struct EngineFixture {
    ScratchDir dir;
    CapturedLogs logs;
    MetricsRegistry metrics;
    std::unique_ptr<Engine> engine;

    // Writes source.json and `yaml`, validates it and builds the engine. Throws when validation
    // fails (after reporting why).
    Engine& open(const std::string& yaml, const WorkflowOptions& options = {},
                 const ServiceRegistry* services = nullptr, bool validate_payloads = true) {
        // The name holds a non-ASCII character (a JSON escape in the file).
        write_text_file(dir.path() / "source.json", "{\"value\": 42, \"name\": \"s\\u00ebd\"}");
        auto validated = praenora::test::validate_text(dir.path(), yaml, options);
        if (!validated.workflow) {
            ADD_FAILURE() << validated.report.to_text();
            throw std::runtime_error("validation failed");
        }
        EngineSettings settings;
        settings.logs = &logs.hub();
        settings.metrics = &metrics;
        settings.services = services;
        settings.validate_payloads = validate_payloads;
        engine = std::make_unique<Engine>(std::move(*validated.workflow), settings);
        return *engine;
    }

    fs::path out() const { return dir.path() / "out"; }

    const StepStatus& step_status(const StatusSnapshot& snapshot, const std::string& id) const {
        for (const StepStatus& status : snapshot.steps) {
            if (status.step == id) {
                return status;
            }
        }
        throw std::runtime_error("no step " + id + " in the status");
    }

    // (step, lifecycle call) for every "lifecycle:<call>" a module logged, in order.
    std::vector<std::string> lifecycle() const {
        std::vector<std::string> calls;
        for (const json& event : logs.events()) {
            const std::string body = event.value("body", std::string());
            if (body.rfind("lifecycle:", 0) == 0) {
                calls.push_back(event.at("attributes").at("step").get<std::string>() + ":" +
                                body.substr(10));
            }
        }
        return calls;
    }
};

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The JSON lines of `file`, parsed.
std::vector<json> json_lines(const fs::path& file) {
    std::vector<json> lines;
    std::istringstream text(read_file(file));
    for (std::string line; std::getline(text, line);) {
        lines.push_back(json::parse(line));
    }
    return lines;
}

// The schema of `kind` at the format this runtime writes (the protocol's version for the pmp-*
// documents).
const praenora::detail::CompiledSchema& schema_of(const char* kind) {
    const auto& formats = praenora::version_info().formats;
    const auto format = formats.find(kind);
    return praenora::detail::SchemaRegistry::builtin().for_kind(
        kind,
        praenora::detail::parse_semver(format != formats.end() ? format->second : "0.1.0"));
}

// Waits (up to 10 s) until step `id` is busy in the metrics.
void wait_until_busy(const EngineFixture& fixture, const std::string& id) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
        if (fixture.step_status(fixture.metrics.snapshot(), id).state == StepState::busy) {
            return;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
}

// The warnings (event teardown.skipped) about instances the teardown left alone: step -> message.
std::map<std::string, std::string> skipped_in_teardown(const EngineFixture& fixture) {
    std::map<std::string, std::string> skipped;
    for (const json& event : fixture.logs.events()) {
        const json& attributes = event.at("attributes");
        if (attributes.value("event", std::string()) == "teardown.skipped") {
            EXPECT_EQ(event.at("severity_text"), "WARN") << event.dump();
            skipped[attributes.value("step", std::string())] = event.value("body", std::string());
        }
    }
    return skipped;
}

} // namespace

TEST(Envelope, BuildsTheEnvelopeOfAMessage) {
    const json envelope = praenora::detail::make_envelope(
        "9f2c4e1a7b3d5e60", 3, 4.0, praenora::detail::Endpoint::parse("estimate.state"),
        "example.state-estimate", praenora::detail::parse_semver("0.1.0"), json{{"x", 1}});
    EXPECT_EQ(envelope, json({{"pmp_version", "0.1.0"},
                              {"id", "9f2c4e1a7b3d5e60:3:estimate.state"},
                              {"type", "example.state-estimate"},
                              {"type_version", "0.1.0"},
                              {"source", "estimate.state"},
                              {"tick", 3},
                              {"logical_time", 4.0},
                              {"data", {{"x", 1}}}}));
    EXPECT_TRUE(praenora::detail::envelope_schema().validate(envelope).empty());

    const json input = praenora::detail::make_envelope(
        "0123456789abcdef", 0, 1.0, praenora::detail::Endpoint::parse("inputs.observations"),
        "pmp.observation-batch", praenora::detail::parse_semver("0.1.0"), json::object());
    EXPECT_EQ(input.at("id"), "0123456789abcdef:0:inputs.observations");
    EXPECT_EQ(input.at("source"), "inputs.observations");
    EXPECT_TRUE(praenora::detail::envelope_schema().validate(input).empty());
}

TEST(Engine, SingleEchoRunsThreeTicks) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-echo"));
    EXPECT_EQ(engine.state(), WorkflowState::created);
    EXPECT_TRUE(engine.run_record().is_null()); // nothing is recorded before the run starts
    engine.start();
    EXPECT_EQ(engine.state(), WorkflowState::ready);
    ASSERT_EQ(engine.fingerprint().size(), 64u);
    const std::string fp16 = engine.fingerprint().substr(0, 16);

    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        EXPECT_FALSE(engine.finished());
        const TickResult result = engine.run_tick();
        EXPECT_EQ(result.tick, tick);
        EXPECT_EQ(result.logical_time, static_cast<double>(tick + 1)); // the window's end
        EXPECT_EQ(result.outputs, std::vector<std::string>{"result"});
        EXPECT_EQ(result.finished, tick == 2);

        const json latest = json::parse(engine.latest_outputs_json());
        ASSERT_TRUE(latest.contains("result")) << latest.dump();
        const json& envelope = latest.at("result");
        EXPECT_EQ(envelope.at("id"), fp16 + ":" + std::to_string(tick) + ":a.out");
        EXPECT_EQ(envelope.at("source"), "a.out");
        EXPECT_EQ(envelope.at("type"), "test.blob");
        EXPECT_EQ(envelope.at("type_version"), "0.1.0");
        EXPECT_EQ(envelope.at("pmp_version"), "0.1.0");
        EXPECT_EQ(envelope.at("tick"), tick);
        EXPECT_EQ(envelope.at("logical_time"), static_cast<double>(tick + 1));
        EXPECT_EQ(envelope.at("data"), json({{"value", 42},
                                             {"name", "s\xC3\xAB"
                                                      "d"}}));
        EXPECT_TRUE(schema_of("pmp-envelope").validate(envelope).empty()) << envelope.dump();
    }

    // The last tick completed the run: everything is torn down and the run record is final.
    EXPECT_TRUE(engine.finished());
    EXPECT_EQ(engine.state(), WorkflowState::completed);
    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.final_state, WorkflowState::completed);
    EXPECT_EQ(summary.ticks_completed, 3u);
    EXPECT_EQ(summary.fingerprint, engine.fingerprint());
    EXPECT_FALSE(summary.error.has_value());

    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(status.state, WorkflowState::completed);
    EXPECT_EQ(status.health, Health::healthy);
    EXPECT_EQ(status.ticks_completed, 3u);
    EXPECT_EQ(status.workflow_name, "test.single");
    EXPECT_EQ(status.fingerprint, engine.fingerprint());
    const StepStatus& a = fixture.step_status(status, "a");
    EXPECT_EQ(a.module_name, "fx-echo");
    EXPECT_EQ(a.invocations, 3u);
    EXPECT_EQ(a.succeeded, 3u);
    EXPECT_EQ(a.state, StepState::destroyed);

    EXPECT_EQ(engine.run_record().at("execution").at("status"), "completed");
    // The run is over: no more ticks.
    EXPECT_EQ(error_of([&] { engine.run_tick(); }).code(), ErrorCode::INVALID_STATE);
}

TEST(Engine, ControlCallsOutOfOrderAreInvalidState) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-echo"));
    EXPECT_EQ(error_of([&] { engine.run_tick(); }).code(), ErrorCode::INVALID_STATE);
    engine.start();
    EXPECT_EQ(error_of([&] { engine.start(); }).code(), ErrorCode::INVALID_STATE);
    EXPECT_EQ(engine.state(), WorkflowState::ready);
}

TEST(Engine, LoopFeedbackDeliversPreviousTick) {
    EngineFixture fixture;
    Engine& engine = fixture.open(config("test.loop", "run: { ticks: 3 }\n", "",
                                         "steps:\n" + step("a", "fx-echo") + step("b", "fx-echo")));
    engine.start();
    while (!engine.finished()) {
        engine.run_tick();
    }
    ASSERT_EQ(engine.state(), WorkflowState::completed);
    const std::string fp16 = engine.fingerprint().substr(0, 16);

    // What each step received, from the message trace.
    std::vector<json> a_in, b_in;
    for (const json& line : json_lines(fixture.out() / "trace" / "messages.jsonl")) {
        if (line.at("direction") == "in") {
            (line.at("step") == "a" ? a_in : b_in).push_back(line);
        }
    }
    // a.in is fed by b.out with delay 1: absent at tick 0, then the previous tick's message.
    ASSERT_EQ(a_in.size(), 2u);
    for (std::uint64_t tick = 1; tick < 3; ++tick) {
        const json& line = a_in[tick - 1];
        EXPECT_EQ(line.at("tick"), tick);
        EXPECT_EQ(line.at("port"), "in");
        const json& envelope = line.at("envelope");
        EXPECT_EQ(envelope.at("source"), "b.out");
        EXPECT_EQ(envelope.at("tick"), tick - 1);
        EXPECT_EQ(envelope.at("id"), fp16 + ":" + std::to_string(tick - 1) + ":b.out");
    }
    // b.in is fed by a.out with delay 0: the same tick's message, every tick.
    ASSERT_EQ(b_in.size(), 3u);
    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        EXPECT_EQ(b_in[tick].at("tick"), tick);
        EXPECT_EQ(b_in[tick].at("envelope").at("source"), "a.out");
        EXPECT_EQ(b_in[tick].at("envelope").at("tick"), tick);
    }
}

TEST(Engine, ReplayInputEndsRunWhenExhausted) {
    EngineFixture fixture;
    write_text_file(fixture.dir.path() / "obs.csv", praenora::test::observations_csv());
    // No run.ticks: the run lasts until the observations (times 0.0, 0.5, 1.25 and 2.5) are
    // exhausted, which takes the windows [0, 1), [1, 2) and [2, 3).
    Engine& engine =
        fixture.open(config("test.single", "run: { time: { unit: d, start: 0.0, period: 1.0 } }\n",
                            "inputs:\n  source: { source: observation-csv, path: obs.csv }\n",
                            "steps:\n" + step("a", "fx-echo")));
    engine.start();
    std::vector<json> batches;
    while (!engine.finished()) {
        const TickResult result = engine.run_tick();
        batches.push_back(json::parse(engine.latest_outputs_json()).at("result").at("data"));
        EXPECT_EQ(result.finished, result.tick == 2);
    }
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(engine.state(), WorkflowState::completed);
    const std::vector<std::size_t> rows = {2, 1, 1};
    for (std::size_t tick = 0; tick < 3; ++tick) {
        EXPECT_EQ(batches[tick].at("window"),
                  json({{"start", static_cast<double>(tick)}, {"end", tick + 1.0}}));
        EXPECT_EQ(batches[tick].at("time_unit"), "d");
        EXPECT_EQ(batches[tick].at("observations").size(), rows[tick]);
    }
}

TEST(Engine, MalformedOutputIsOutputInvalid) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ mode: malformed_json }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::OUTPUT_INVALID) << error.to_json();
    const json details = details_of(error);
    EXPECT_EQ(details.at("raw_prefix"), R"({"outputs":{"out":{"data":)");
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_EQ(details.at("tick"), 0);
    EXPECT_EQ(details.at("module"), "fx-fail@0.1.0");
    EXPECT_TRUE(contains(error.what(), "'a'")) << error.what();

    // Fail-fast: the run failed and its instance was finalized and destroyed.
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    EXPECT_TRUE(engine.finished());
    EXPECT_EQ(fixture.lifecycle(), (std::vector<std::string>{"a:create", "a:initialize", "a:invoke",
                                                             "a:finalize", "a:destroy"}));
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(status.health, Health::unhealthy);
    const StepStatus& a = fixture.step_status(status, "a");
    EXPECT_EQ(a.failed, 1u);
    EXPECT_EQ(a.state, StepState::failed);
    ASSERT_TRUE(a.last_error.has_value());
    EXPECT_EQ(a.last_error->code, ErrorCode::OUTPUT_INVALID);
    EXPECT_EQ(error_of([&] { engine.run_tick(); }).code(), ErrorCode::INVALID_STATE);
}

TEST(Engine, SchemaViolatingOutputIsOutputInvalidWithPointer) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ mode: schema_violation }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::OUTPUT_INVALID) << error.to_json();
    EXPECT_EQ(error.location().pointer, "/outputs/out/data");
    const json details = details_of(error);
    EXPECT_EQ(details.at("port"), "out");
    EXPECT_EQ(details.at("type"), "test.blob@0.1.0");
    EXPECT_EQ(engine.state(), WorkflowState::failed);
}

TEST(Engine, OutputValidationOffAcceptsSchemaViolatingData) {
    // With payload validation off, data is not checked against its message type (the structure of
    // the result still is), and the run record says so.
    EngineFixture fixture;
    Engine& engine =
        fixture.open(single("fx-fail", "{ mode: schema_violation }"), {}, nullptr, false);
    engine.start();
    const TickResult result = engine.run_tick();
    EXPECT_EQ(result.outputs, std::vector<std::string>{"result"});
    EXPECT_EQ(json::parse(engine.latest_outputs_json()).at("result").at("data"),
              "fx-fail: not an object");
    EXPECT_EQ(engine.run_record().at("execution").at("validation"), "off");
}

TEST(Engine, UnknownPortIsOutputInvalid) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ mode: unknown_port }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::OUTPUT_INVALID) << error.to_json();
    EXPECT_EQ(error.location().pointer, "/outputs/undeclared");
    EXPECT_EQ(details_of(error).at("port"), "undeclared");
    EXPECT_TRUE(contains(error.what(), "undeclared")) << error.what();
}

TEST(Engine, MissingRequiredOutputIsOutputInvalid) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ mode: missing_required_output }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::OUTPUT_INVALID) << error.to_json();
    EXPECT_EQ(error.location().pointer, "/outputs");
    EXPECT_EQ(details_of(error).at("port"), "out");
    EXPECT_TRUE(contains(error.what(), "required")) << error.what();
}

TEST(Engine, ModuleDefinedStatusIsNamedSo) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ phase: invoke, status: 10001 }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.to_json();
    EXPECT_TRUE(contains(error.what(), "with status 10001 (module-defined): ")) << error.what();
    EXPECT_EQ(details_of(error).at("cause").at("code"), 10001);
}

TEST(Engine, ModuleFailureFailsFastAndCleansUp) {
    EngineFixture fixture;
    // test.chain: inputs.source -> a -> b -> outputs.result; a fails at its second invocation.
    Engine& engine = fixture.open(
        config("test.chain", "run: { ticks: 5 }\n", kJsonSource,
               "steps:\n" + step("a", "fx-fail", "{ phase: invoke, at_invocation: 2 }") +
                   step("b", "fx-lifecycle")));
    engine.start();
    EXPECT_EQ(engine.run_tick().tick, 0u);
    const Error error = error_of([&] { engine.run_tick(); });

    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.to_json();
    EXPECT_TRUE(contains(error.what(), "failed at tick 1 with status 11 (FAILED): "))
        << error.what();
    const json details = details_of(error);
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_EQ(details.at("tick"), 1);
    EXPECT_EQ(details.at("phase"), "invoke");
    EXPECT_EQ(details.at("cause").at("code"), 11);
    EXPECT_EQ(details.at("cause").at("message"),
              "fx-fail: injected failure in invoke at invocation 2");
    // fx-fail declares diagnostics; they were collected before the teardown.
    EXPECT_EQ(details.at("diagnostics"), json({{"invocations", 2}}));
    EXPECT_TRUE(contains(error.what(), "injected failure")) << error.what();

    // Fail-fast: b is not invoked in tick 1. Then every ready instance is finalized, last created
    // first, and only then is every instance destroyed, again last created first.
    EXPECT_EQ(fixture.lifecycle(),
              (std::vector<std::string>{"a:create", "a:initialize", "b:create", "b:initialize",
                                        "a:invoke", "b:invoke", "a:invoke", "b:finalize",
                                        "a:finalize", "b:destroy", "a:destroy"}));

    EXPECT_EQ(engine.state(), WorkflowState::failed);
    EXPECT_TRUE(engine.finished());
    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.final_state, WorkflowState::failed);
    EXPECT_EQ(summary.ticks_completed, 1u);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::STEP_FAILED);
    EXPECT_EQ(summary.error->tick, std::optional<std::uint64_t>(1));
    EXPECT_EQ(json::parse(summary.error->cause_json).at("code"), 11);

    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(status.state, WorkflowState::failed);
    EXPECT_EQ(status.health, Health::unhealthy);
    ASSERT_TRUE(status.last_error.has_value());
    EXPECT_EQ(status.last_error->code, ErrorCode::STEP_FAILED);
    const StepStatus& a = fixture.step_status(status, "a");
    EXPECT_EQ(a.invocations, 2u);
    EXPECT_EQ(a.succeeded, 1u);
    EXPECT_EQ(a.failed, 1u);
    EXPECT_EQ(a.state, StepState::failed);
    const StepStatus& b = fixture.step_status(status, "b");
    EXPECT_EQ(b.invocations, 1u);
    EXPECT_EQ(b.state, StepState::destroyed);

    // The final run record says how the run ended.
    const json record = json::parse(read_file(fixture.out() / "run-record.json"));
    EXPECT_TRUE(schema_of("run-record").validate(record).empty()) << record.dump(2);
    EXPECT_EQ(record.at("execution").at("status"), "failed");
    EXPECT_EQ(record.at("execution").at("ticks_completed"), 1);
    EXPECT_EQ(record.at("execution").at("error").at("name"), "STEP_FAILED");
    EXPECT_EQ(record.at("execution").at("error").at("cause").at("code"), 11);
    EXPECT_EQ(record, engine.run_record());
}

TEST(Engine, StartFailureCleansUpCreatedSteps) {
    EngineFixture fixture;
    Engine& engine = fixture.open(config("test.chain", "run: { ticks: 2 }\n", kJsonSource,
                                         "steps:\n" + step("a", "fx-lifecycle") +
                                             step("b", "fx-fail", "{ phase: initialize }")));
    const Error error = error_of([&] { engine.start(); });
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.to_json();
    const json details = details_of(error);
    EXPECT_EQ(details.at("phase"), "initialize");
    EXPECT_EQ(details.at("step"), "b");
    EXPECT_FALSE(details.contains("tick"));
    EXPECT_EQ(details.at("cause").at("details").at("phase"), "initialize");
    // b failed to initialize, so it is destroyed without finalize; a is finalized first.
    EXPECT_EQ(fixture.lifecycle(),
              (std::vector<std::string>{"a:create", "a:initialize", "b:create", "b:initialize",
                                        "a:finalize", "b:destroy", "a:destroy"}));
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    EXPECT_TRUE(engine.finished());
    EXPECT_EQ(engine.summary().ticks_completed, 0u);
    const StatusSnapshot status = fixture.metrics.snapshot();
    const StepStatus& b = fixture.step_status(status, "b");
    EXPECT_EQ(b.state, StepState::failed);
    // The step that failed has its own last error, though nothing was invoked.
    ASSERT_TRUE(b.last_error.has_value());
    EXPECT_EQ(b.last_error->code, ErrorCode::STEP_FAILED);
    EXPECT_EQ(json::parse(b.last_error->cause_json).at("details").at("phase"), "initialize");
    EXPECT_EQ(b.invocations, 0u);
    EXPECT_FALSE(fixture.step_status(status, "a").last_error.has_value());
    EXPECT_EQ(engine.run_record().at("execution").at("status"), "failed");
}

TEST(Engine, FinalizeFailureMarksTheStep) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ phase: finalize }"));
    engine.start();
    while (!engine.finished()) {
        engine.run_tick();
    }
    // The run completed; its teardown found a problem, which the step shows (and its health).
    EXPECT_EQ(engine.state(), WorkflowState::completed);
    EXPECT_FALSE(engine.summary().error.has_value());
    const StatusSnapshot status = fixture.metrics.snapshot();
    const StepStatus& a = fixture.step_status(status, "a");
    EXPECT_EQ(a.state, StepState::failed);
    EXPECT_EQ(a.succeeded, 3u);
    ASSERT_TRUE(a.last_error.has_value());
    EXPECT_EQ(a.last_error->code, ErrorCode::STEP_FAILED);
    EXPECT_EQ(json::parse(a.last_error->cause_json).at("details").at("phase"), "finalize");
    EXPECT_FALSE(status.last_error.has_value()); // the run did not fail
    EXPECT_EQ(status.health, Health::unhealthy);
    EXPECT_EQ(fixture.lifecycle(),
              (std::vector<std::string>{"a:create", "a:initialize", "a:invoke", "a:invoke",
                                        "a:invoke", "a:finalize", "a:destroy"}));
}

TEST(Engine, TimeoutMapsToStepTimeout) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-slow", "{ sleep_ms: 5000, cooperative: true }",
                                         "    timeouts: { invoke: 100ms, cancel_grace: 5s }\n"));
    engine.start();
    const auto start = Clock::now();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(3));
    EXPECT_EQ(error.code(), ErrorCode::STEP_TIMEOUT) << error.to_json();
    const json details = details_of(error);
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_EQ(details.at("phase"), "invoke");
    EXPECT_EQ(details.at("tick"), 0);
    // The call returned within the grace, so the instance still answers: fx-slow declares
    // diagnostics, and they were asked for.
    EXPECT_EQ(details.at("diagnostics"), json({{"invocations", 1}}));
    EXPECT_EQ(details.at("cause").at("code"), 7); // fx-slow gave up with CANCELLED
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(fixture.step_status(status, "a").timed_out, 1u);
    EXPECT_EQ(status.health, Health::unhealthy);
}

TEST(Engine, UnresponsiveStepIsModuleUnresponsive) {
    // Static: the detached thread polls the gate after the engine is gone.
    static TestGate gate;
    gate.close();
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    EngineFixture fixture;
    Engine& engine = fixture.open(
        single("fx-hang", "{}", "    timeouts: { invoke: 50ms, cancel_grace: 50ms }\n"), {},
        &services);
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::MODULE_UNRESPONSIVE) << error.to_json();
    EXPECT_EQ(details_of(error).at("step"), "a");
    // fx-hang declares diagnostics, but an unresponsive instance is never called again.
    ASSERT_TRUE(praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-hang"))
                    .features.diagnostics);
    EXPECT_FALSE(details_of(error).contains("diagnostics"));
    EXPECT_FALSE(details_of(error).contains("cause")); // the call never returned
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    EXPECT_EQ(fixture.step_status(fixture.metrics.snapshot(), "a").state, StepState::unresponsive);
    fixture.engine.reset(); // the unresponsive instance is left alone

    // Release the detached thread, so it finishes before the test ends.
    gate.open();
    const auto deadline = Clock::now() + milliseconds(500);
    while (gate.seen_open() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_GT(gate.seen_open(), 0);
    std::this_thread::sleep_for(milliseconds(20));
}

TEST(Engine, StopRequestEndsAsStopped) {
    EngineFixture fixture;
    // b sleeps 5 s in invoke and 50 ms in finalize, polling for cancellation all the while.
    Engine& engine = fixture.open(config(
        "test.chain", "run: { ticks: 5 }\n", kJsonSource,
        "steps:\n" + step("a", "fx-echo") +
            step("b", "fx-slow", "{ sleep_ms: 5000, finalize_sleep_ms: 50, cooperative: true }")));
    engine.start();
    std::thread stopper([&] {
        wait_until_busy(fixture, "b");
        std::this_thread::sleep_for(milliseconds(20));
        engine.request_stop();
    });
    const auto start = Clock::now();
    const TickResult result = engine.run_tick(); // a stop is not an error
    stopper.join();
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(3));

    // The tick was cut short: no outputs, and it does not count.
    EXPECT_TRUE(result.finished);
    EXPECT_EQ(result.tick, 0u);
    EXPECT_TRUE(result.outputs.empty());
    EXPECT_TRUE(engine.finished());
    EXPECT_EQ(engine.state(), WorkflowState::stopped);
    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    EXPECT_EQ(summary.ticks_completed, 0u);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);

    // The call in flight was cancelled; the teardown then ran normally: b's finalize, which polls
    // for cancellation, was not cut short.
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(status.state, WorkflowState::stopped);
    EXPECT_EQ(status.health, Health::degraded);
    EXPECT_EQ(fixture.step_status(status, "b").cancelled, 1u);
    EXPECT_EQ(fixture.step_status(status, "b").state, StepState::destroyed);
    EXPECT_EQ(fixture.step_status(status, "a").state, StepState::destroyed);
    EXPECT_FALSE(status.last_error.has_value());
    EXPECT_EQ(engine.run_record().at("execution").at("status"), "stopped");
    EXPECT_EQ(engine.run_record().at("execution").at("error").at("name"), "RUN_STOPPED");
    EXPECT_EQ(read_file(fixture.out() / "outputs" / "result.jsonl"), ""); // nothing recorded
    EXPECT_EQ(error_of([&] { engine.run_tick(); }).code(), ErrorCode::INVALID_STATE);
}

TEST(Engine, StopBetweenTicksEndsAtTheNextTick) {
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-echo"));
    engine.start();
    engine.run_tick();
    engine.request_stop();
    // Not finished yet: the next tick winds the run down, without invoking anything, so a loop of
    // run_tick() until finished() ends the run as stopped.
    EXPECT_FALSE(engine.finished());
    EXPECT_EQ(engine.state(), WorkflowState::running);
    const TickResult result = engine.run_tick();
    EXPECT_TRUE(result.finished);
    EXPECT_TRUE(engine.finished());
    EXPECT_EQ(result.tick, 1u);
    EXPECT_TRUE(result.outputs.empty());
    EXPECT_EQ(engine.state(), WorkflowState::stopped);
    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    EXPECT_EQ(summary.ticks_completed, 1u);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(fixture.step_status(fixture.metrics.snapshot(), "a").invocations, 1u);
}

TEST(Engine, ShutdownBeforeTheEndStopsTheRun) {
    EngineFixture fixture;
    Engine& engine =
        fixture.open(config("test.chain", "run: { ticks: 5 }\n", kJsonSource,
                            "steps:\n" + step("a", "fx-lifecycle") + step("b", "fx-lifecycle")));
    engine.start();
    engine.run_tick();
    engine.shutdown();
    EXPECT_EQ(engine.state(), WorkflowState::stopped);
    const RunSummary summary = engine.summary();
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(fixture.lifecycle(),
              (std::vector<std::string>{"a:create", "a:initialize", "b:create", "b:initialize",
                                        "a:invoke", "b:invoke", "b:finalize", "a:finalize",
                                        "b:destroy", "a:destroy"}));
    engine.shutdown(); // idempotent
    EXPECT_EQ(engine.state(), WorkflowState::stopped);
    EXPECT_EQ(fixture.lifecycle().size(), 10u);

    // An engine that never started has nothing to shut down.
    EngineFixture idle;
    Engine& unused = idle.open(single("fx-lifecycle"));
    unused.shutdown();
    EXPECT_EQ(unused.state(), WorkflowState::created);
    EXPECT_TRUE(idle.lifecycle().empty());
    EXPECT_FALSE(fs::exists(idle.out() / "run-record.json"));
}

TEST(Engine, WritesRunRecordOutputsAndTrace) {
    EngineFixture fixture;
    WorkflowOptions options;
    options.seed = 99;
    Engine& engine = fixture.open(single("fx-echo"), options);
    engine.start();

    // The run record is written when the run starts.
    const json started = json::parse(read_file(fixture.out() / "run-record.json"));
    EXPECT_TRUE(schema_of("run-record").validate(started).empty()) << started.dump(2);
    EXPECT_EQ(started.at("execution").at("status"), "running");
    EXPECT_FALSE(started.at("execution").contains("final_status"));
    EXPECT_TRUE(started.at("execution").at("finished_at").is_null());
    EXPECT_EQ(started.at("fingerprint"), engine.fingerprint());

    while (!engine.finished()) {
        engine.run_tick();
    }
    const json record = json::parse(read_file(fixture.out() / "run-record.json"));
    EXPECT_TRUE(schema_of("run-record").validate(record).empty()) << record.dump(2);
    EXPECT_EQ(record, engine.run_record());
    EXPECT_EQ(record.at("fingerprint"), engine.fingerprint());
    EXPECT_EQ(record.at("fingerprint"),
              praenora::detail::sha256_hex(
                  praenora::detail::canonical_json(record.at("reproducibility"))));
    const json& execution = record.at("execution");
    EXPECT_EQ(execution.at("status"), "completed");
    EXPECT_EQ(execution.at("ticks_completed"), 3);
    EXPECT_TRUE(execution.at("error").is_null());
    EXPECT_EQ(execution.at("overrides"), json({{"seed", 99}}));
    EXPECT_EQ(execution.at("validation"), "strict");
    EXPECT_EQ(execution.at("final_status").at("workflow").at("state"), "completed");
    EXPECT_EQ(record.dump().find("hostname"), std::string::npos);

    // outputs/result.jsonl: one canonical envelope per line, as the run record describes it.
    const std::string outputs = read_file(fixture.out() / "outputs" / "result.jsonl");
    const json& file = execution.at("outputs").at("result");
    EXPECT_EQ(file.at("file"), "outputs/result.jsonl");
    EXPECT_EQ(file.at("messages"), 3);
    EXPECT_EQ(file.at("sha256"), praenora::detail::sha256_hex(outputs));
    std::istringstream lines(outputs);
    std::uint64_t tick = 0;
    for (std::string line; std::getline(lines, line); ++tick) {
        const json envelope = json::parse(line);
        EXPECT_EQ(line, praenora::detail::canonical_json(envelope));
        EXPECT_EQ(envelope.at("tick"), tick);
    }
    EXPECT_EQ(tick, 3u);
    EXPECT_EQ(outputs.find('\r'), std::string::npos);

    // trace/messages.jsonl: every envelope into and out of a step.
    const std::string trace_text = read_file(fixture.out() / "trace" / "messages.jsonl");
    const auto trace = json_lines(fixture.out() / "trace" / "messages.jsonl");
    ASSERT_EQ(trace.size(), 6u);
    EXPECT_EQ(trace[0].at("direction"), "in");
    EXPECT_EQ(trace[0].at("port"), "in");
    EXPECT_EQ(trace[0].at("envelope").at("source"), "inputs.source");
    EXPECT_EQ(trace[1].at("direction"), "out");
    EXPECT_EQ(trace[1].at("port"), "out");
    EXPECT_EQ(trace[1].at("step"), "a");
    EXPECT_EQ(execution.at("trace"), json({{"file", "trace/messages.jsonl"},
                                           {"messages", 6},
                                           {"sha256", praenora::detail::sha256_hex(trace_text)}}));
}

TEST(Engine, RunsAreByteIdentical) {
    std::string outputs[2], traces[2], fingerprints[2];
    for (int run = 0; run < 2; ++run) {
        EngineFixture fixture;
        Engine& engine =
            fixture.open(config("test.chain", "run: { ticks: 4, seed: 11 }\n", kJsonSource,
                                "steps:\n" + step("a", "fx-echo") + step("b", "fx-echo")));
        engine.start();
        while (!engine.finished()) {
            engine.run_tick();
        }
        outputs[run] = read_file(fixture.out() / "outputs" / "result.jsonl");
        traces[run] = read_file(fixture.out() / "trace" / "messages.jsonl");
        fingerprints[run] = engine.fingerprint();
    }
    EXPECT_FALSE(outputs[0].empty());
    EXPECT_EQ(outputs[0], outputs[1]);
    EXPECT_EQ(traces[0], traces[1]);
    EXPECT_EQ(fingerprints[0], fingerprints[1]);
}

TEST(Engine, QueriesDuringRunAreSafe) {
    // The query methods may be called from any thread while the run goes on (run them under the
    // ThreadSanitizer preset to check for races).
    EngineFixture fixture;
    Engine& engine = fixture.open(
        config("test.chain", "run: { ticks: 30 }\n", kJsonSource,
               "steps:\n" + step("a", "fx-echo") + step("b", "fx-slow", "{ sleep_ms: 1 }")));
    std::atomic<bool> done{false};
    std::uint64_t queries = 0;
    bool monotonic = true;
    std::thread reader([&] {
        std::uint64_t last = 0;
        while (!done.load()) {
            const RunSummary summary = engine.summary();
            monotonic = monotonic && summary.ticks_completed >= last;
            last = summary.ticks_completed;
            static_cast<void>(engine.latest_outputs_json());
            static_cast<void>(engine.run_record());
            static_cast<void>(engine.finished());
            static_cast<void>(engine.state());
            static_cast<void>(fixture.metrics.snapshot().to_json());
            ++queries;
        }
    });
    engine.start();
    while (!engine.finished()) {
        engine.run_tick();
    }
    done.store(true);
    reader.join();
    EXPECT_GT(queries, 0u);
    EXPECT_TRUE(monotonic);
    EXPECT_EQ(engine.state(), WorkflowState::completed);
    EXPECT_EQ(engine.summary().ticks_completed, 30u);
}

TEST(Engine, StoppingOnlyOnStopOrFailure) {
    // The workflow states the engine goes through, as its state-change callback sees them. A run
    // that completes never passes through `stopping`; one that fails or is stopped does.
    const auto states_of = [](const std::string& yaml, bool stop_after_first_tick) {
        EngineFixture fixture;
        write_text_file(fixture.dir.path() / "source.json", "{}");
        auto validated = praenora::test::validate_text(fixture.dir.path(), yaml);
        if (!validated.workflow) {
            ADD_FAILURE() << validated.report.to_text();
            return std::vector<WorkflowState>{};
        }
        std::vector<WorkflowState> states;
        Engine* engine = nullptr;
        EngineSettings settings;
        settings.metrics = &fixture.metrics;
        settings.on_state_change = [&] {
            states.push_back(engine->state());
            EXPECT_EQ(fixture.metrics.snapshot().state, engine->state());
        };
        Engine owned(std::move(*validated.workflow), settings);
        engine = &owned;
        try {
            owned.start();
            while (!owned.finished()) {
                owned.run_tick();
                if (stop_after_first_tick) {
                    owned.request_stop();
                }
            }
            owned.shutdown();
        } catch (const Error&) {
        }
        return states;
    };
    using S = WorkflowState;
    EXPECT_EQ(states_of(single("fx-echo"), false),
              (std::vector<S>{S::starting, S::ready, S::running, S::completed}));
    EXPECT_EQ(states_of(single("fx-fail", "{ at_invocation: 2 }"), false),
              (std::vector<S>{S::starting, S::ready, S::running, S::stopping, S::failed}));
    EXPECT_EQ(states_of(single("fx-echo"), true),
              (std::vector<S>{S::starting, S::ready, S::running, S::stopping, S::stopped}));
}

TEST(Engine, LibraryChangedAfterValidationIsRefused) {
    EngineFixture fixture;
    write_text_file(fixture.dir.path() / "source.json", "{}");
    auto validated = praenora::test::validate_text(fixture.dir.path(), single("fx-lifecycle"));
    ASSERT_TRUE(validated.workflow) << validated.report.to_text();
    // As if the library file had been replaced between validation and loading.
    validated.workflow->steps.at(0).library_sha256 = std::string(64, 'a');
    EngineSettings settings;
    settings.logs = &fixture.logs.hub();
    settings.metrics = &fixture.metrics;
    Engine engine(std::move(*validated.workflow), settings);
    const Error error = error_of([&] { engine.start(); });
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_DIGEST_MISMATCH) << error.to_json();
    EXPECT_EQ(error.location().pointer, "/library/path");
    EXPECT_EQ(details_of(error).at("phase"), "load");
    EXPECT_EQ(details_of(error).at("step"), "a");
    EXPECT_TRUE(fixture.lifecycle().empty()); // nothing of the module ran
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    const StatusSnapshot status = fixture.metrics.snapshot();
    const StepStatus& a = fixture.step_status(status, "a");
    ASSERT_TRUE(a.last_error.has_value());
    EXPECT_EQ(a.last_error->code, ErrorCode::LIBRARY_DIGEST_MISMATCH);
}

TEST(Engine, LongMalformedOutputShowsItsFirst200Bytes) {
    EngineFixture fixture;
    Engine& engine =
        fixture.open(single("fx-fail", "{ mode: malformed_json, malformed_bytes: 1000 }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::OUTPUT_INVALID) << error.to_json();
    // fx-fail returned 1000 bytes: {"outputs":{"out":{"data":" followed by x characters.
    const std::string expected =
        std::string(R"({"outputs":{"out":{"data":")") + std::string(200 - 27, 'x');
    EXPECT_EQ(details_of(error).at("raw_prefix"), expected);
}

TEST(Engine, OversizedDiagnosticsAreCut) {
    // Diagnostics larger than 64 KiB are not embedded whole in the error: a marker object says how
    // large they were and keeps their first 64 KiB as text.
    EngineFixture fixture;
    Engine& engine = fixture.open(single("fx-fail", "{ diagnostics_bytes: 100000 }"));
    engine.start();
    const Error error = error_of([&] { engine.run_tick(); });
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json diagnostics = details_of(error).at("diagnostics");
    EXPECT_EQ(diagnostics.at("truncated"), true);
    EXPECT_EQ(diagnostics.at("bytes"), 100000);
    const std::string text = diagnostics.at("text").get<std::string>();
    EXPECT_EQ(text.size(), 65536u);
    EXPECT_EQ(text.rfind("{\"invocations\":1,\"padding\":\"xxx", 0), 0u);
    EXPECT_LT(error.details_json().size(), 70000u);
}

TEST(Engine, OptionalOutputLeftOutIsNotRecorded) {
    // test.quiet: the output of step a is optional. fx-fail leaves it out of its second result,
    // which carries a diagnostics object instead.
    EngineFixture fixture;
    Engine& engine = fixture.open(
        config("test.quiet", "run: { ticks: 3 }\n", kJsonSource,
               "steps:\n" + step("a", "fx-fail", "{ mode: no_outputs, at_invocation: 2 }")));
    engine.start();
    std::vector<TickResult> results;
    while (!engine.finished()) {
        results.push_back(engine.run_tick());
    }
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].outputs, std::vector<std::string>{"result"});
    EXPECT_TRUE(results[1].outputs.empty());
    EXPECT_EQ(results[2].outputs, std::vector<std::string>{"result"});
    EXPECT_EQ(engine.state(), WorkflowState::completed);
    EXPECT_EQ(fixture.step_status(fixture.metrics.snapshot(), "a").succeeded, 3u);

    // Only the outputs produced are recorded.
    const auto recorded = json_lines(fixture.out() / "outputs" / "result.jsonl");
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded[0].at("tick"), 0);
    EXPECT_EQ(recorded[1].at("tick"), 2);
    const json record = engine.run_record();
    const json& execution = record.at("execution");
    EXPECT_EQ(execution.at("outputs").at("result").at("messages"), 2);

    // The trace keeps the diagnostics of the result without outputs, in a record of its own.
    std::vector<json> tick_one;
    for (const json& line : json_lines(fixture.out() / "trace" / "messages.jsonl")) {
        if (line.at("tick") == 1) {
            tick_one.push_back(line);
        }
    }
    ASSERT_EQ(tick_one.size(), 2u);
    EXPECT_EQ(tick_one[0].at("direction"), "in");
    EXPECT_EQ(tick_one[1], json({{"tick", 1},
                                 {"step", "a"},
                                 {"direction", "out"},
                                 {"port", nullptr},
                                 {"envelope", nullptr},
                                 {"diagnostics", {{"invocations", 2}}}}));
    EXPECT_EQ(execution.at("trace").at("messages"), 5); // envelopes: 3 in, 2 out
}

TEST(Engine, UnresponsiveInstanceKeepsItsLibraryToItself) {
    // fx-hang allows no concurrent calls into its instances (concurrency "module"). Once one of
    // them is unresponsive, the module's code may still be running in it, so the teardown calls
    // no other instance of that library either: they are left as they are, never finalized or
    // destroyed, and a warning says so.
    static TestGate gate; // static: the detached thread polls it after the engine is gone
    gate.close();
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    EngineFixture fixture;
    Engine& engine =
        fixture.open(config("test.chain", "run: { ticks: 2 }\n", kJsonSource,
                            "steps:\n" +
                                step("a", "fx-hang", "{}",
                                     "    timeouts: { invoke: 50ms, cancel_grace: 50ms }\n") +
                                step("b", "fx-hang")),
                     {}, &services);
    engine.start();
    EXPECT_EQ(error_of([&] { engine.run_tick(); }).code(), ErrorCode::MODULE_UNRESPONSIVE);
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(fixture.step_status(status, "a").state, StepState::unresponsive);
    EXPECT_EQ(fixture.step_status(status, "b").state, StepState::ready); // never finalized
    const auto skipped = skipped_in_teardown(fixture);
    ASSERT_EQ(skipped.size(), 1u);
    ASSERT_EQ(skipped.count("b"), 1u);
    EXPECT_TRUE(contains(skipped.at("b"), "is neither finalized nor destroyed")) << skipped.at("b");
    fixture.engine.reset();

    // Release the detached thread, so it finishes before the test ends.
    gate.open();
    const auto deadline = Clock::now() + milliseconds(500);
    while (gate.seen_open() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_GT(gate.seen_open(), 0);
    std::this_thread::sleep_for(milliseconds(20));
}

TEST(Engine, InstanceConcurrencyTearsDownOtherInstances) {
    // fx-slow's instances are independent (concurrency "instance"): with one unresponsive, the
    // others are finalized and destroyed as usual.
    EngineFixture fixture;
    Engine& engine =
        fixture.open(config("test.chain", "run: { ticks: 2 }\n", kJsonSource,
                            "steps:\n" +
                                step("a", "fx-slow", "{ sleep_ms: 300, cooperative: false }",
                                     "    timeouts: { invoke: 50ms, cancel_grace: 50ms }\n") +
                                step("b", "fx-slow")));
    const auto start = Clock::now();
    engine.start();
    EXPECT_EQ(error_of([&] { engine.run_tick(); }).code(), ErrorCode::MODULE_UNRESPONSIVE);
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(fixture.step_status(status, "a").state, StepState::unresponsive);
    EXPECT_EQ(fixture.step_status(status, "b").state, StepState::destroyed);
    for (const json& event : fixture.logs.events()) {
        EXPECT_NE(event.at("attributes").value("event", std::string()), "teardown.skipped");
    }
    // Let the abandoned call end before the test does.
    std::this_thread::sleep_until(start + milliseconds(500));
}

TEST(Engine, TeardownReleasesSiblingsAlreadyDestroyed) {
    // b is destroyed first (the last created goes first); then a's destroy hangs and a becomes
    // unresponsive. b needs no holding back: a destroyed instance is never called again. It is
    // released as usual, and nothing is reported as skipped.
    static TestGate gate; // static: the detached thread polls it after the engine is gone
    gate.close();
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    EngineFixture fixture;
    Engine& engine =
        fixture.open(config("test.chain", "run: { ticks: 1 }\n", kJsonSource,
                            "steps:\n" +
                                step("a", "fx-hang", "{ hang: destroy }",
                                     "    timeouts: { lifecycle: 50ms, cancel_grace: 50ms }\n") +
                                step("b", "fx-hang", "{ hang: none }")),
                     {}, &services);
    engine.start();
    EXPECT_TRUE(engine.run_tick().finished); // the only tick; then the teardown
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(fixture.step_status(status, "a").state, StepState::unresponsive);
    EXPECT_EQ(fixture.step_status(status, "b").state, StepState::destroyed);
    EXPECT_TRUE(skipped_in_teardown(fixture).empty());
    fixture.engine.reset();

    // Release the detached thread, so it finishes before the test ends.
    gate.open();
    const auto deadline = Clock::now() + milliseconds(500);
    while (gate.seen_open() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_GT(gate.seen_open(), 0);
    std::this_thread::sleep_for(milliseconds(20));
}

TEST(Engine, TeardownSkipWarningSaysWhatWasLeftUndone) {
    // b is finalized first; then a's finalize hangs and a becomes unresponsive. b, already
    // finalized, is not destroyed, and the warning says so.
    static TestGate gate; // static: the detached thread polls it after the engine is gone
    gate.close();
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    EngineFixture fixture;
    Engine& engine =
        fixture.open(config("test.chain", "run: { ticks: 1 }\n", kJsonSource,
                            "steps:\n" +
                                step("a", "fx-hang", "{ hang: finalize }",
                                     "    timeouts: { lifecycle: 50ms, cancel_grace: 50ms }\n") +
                                step("b", "fx-hang", "{ hang: none }")),
                     {}, &services);
    engine.start();
    EXPECT_TRUE(engine.run_tick().finished);
    const StatusSnapshot status = fixture.metrics.snapshot();
    EXPECT_EQ(fixture.step_status(status, "a").state, StepState::unresponsive);
    EXPECT_EQ(fixture.step_status(status, "b").state, StepState::finalized);
    const auto skipped = skipped_in_teardown(fixture);
    ASSERT_EQ(skipped.size(), 1u);
    ASSERT_EQ(skipped.count("b"), 1u);
    EXPECT_TRUE(contains(skipped.at("b"), "is finalized but not destroyed")) << skipped.at("b");
    EXPECT_FALSE(contains(skipped.at("b"), "neither")) << skipped.at("b");
    fixture.engine.reset();

    gate.open();
    const auto deadline = Clock::now() + milliseconds(500);
    while (gate.seen_open() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_GT(gate.seen_open(), 0);
    std::this_thread::sleep_for(milliseconds(20));
}

TEST(Engine, ExistingRunRecordIsNotOverwritten) {
    EngineFixture fixture;
    write_text_file(fixture.out() / "run-record.json", "{\"earlier\": true}\n");
    Engine& engine = fixture.open(single("fx-lifecycle"));
    const Error error = error_of([&] { engine.start(); });
    EXPECT_EQ(error.code(), ErrorCode::IO_ERROR) << error.to_json();
    EXPECT_EQ(engine.state(), WorkflowState::failed);
    // Nothing ran and the earlier record is untouched.
    EXPECT_TRUE(fixture.lifecycle().empty());
    EXPECT_EQ(read_file(fixture.out() / "run-record.json"), "{\"earlier\": true}\n");
}
