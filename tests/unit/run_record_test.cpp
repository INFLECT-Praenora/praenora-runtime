// The files of a run's output directory: the run record (run-record.json, written when the run
// starts and again when it ends, its error records naming the directories the run knows by
// placeholders), the recorded workflow outputs (outputs/<output>.jsonl), the message trace
// (trace/messages.jsonl), and the preparation of the directory, which refuses an earlier run unless
// asked to overwrite it.
#include "io/output_recorder.hpp"
#include "io/run_record.hpp"
#include "io/trace_writer.hpp"

#include "api/validation.hpp"
#include "config/workflow_config.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "observability/metrics.hpp"
#include "observability/path_anchors.hpp"

#include "directory_checks.hpp"
#include "module_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::ErrorRecord;
using praenora::RunSummary;
using praenora::WorkflowState;
using praenora::detail::canonical_json;
using praenora::detail::MetricsRegistry;
using praenora::detail::OutputRecorder;
using praenora::detail::path_from_utf8;
using praenora::detail::path_to_utf8;
using praenora::detail::prepare_output_dir;
using praenora::detail::RunOverrides;
using praenora::detail::RunRecordWriter;
using praenora::detail::sha256_hex;
using praenora::detail::TraceWriter;
using praenora::detail::ValidatedWorkflow;
using praenora::test::named_path;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

namespace {

// 2023-11-14T22:13:20.123456789Z
constexpr std::int64_t kStartedAt = 1'700'000'000'123'456'789;

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + file.u8string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A validated test.single workflow (fx-echo, a json-document input), for a real reproducibility
// section and fingerprint; `parameters`, when given, are step a's (a YAML flow mapping).
ValidatedWorkflow validated_single(const fs::path& dir, const std::string& parameters = "") {
    write_text_file(dir / "source.json", "{\"value\": 1}");
    auto result = praenora::test::validate_text(
        dir, "kind: workflow-config\n"
             "format_version: \"0.1.0\"\n"
             "workflow: { name: test.single, version: \"0.1.0\" }\n"
             "run: { ticks: 3, seed: 7 }\n"
             "inputs:\n"
             "  source: { source: json-document, path: source.json }\n"
             "steps:\n"
             "  a: { module: { name: fx-echo, version: \"0.1.0\" }" +
                 (parameters.empty() ? std::string() : ", parameters: " + parameters) + " }\n");
    if (!result.workflow) {
        ADD_FAILURE() << result.report.to_text();
        throw std::runtime_error("validation failed");
    }
    return std::move(*result.workflow);
}

// A status snapshot of `workflow` in `state`.
std::string status_json(const ValidatedWorkflow& workflow, WorkflowState state) {
    MetricsRegistry metrics;
    metrics.set_run_info("test.single", "0.1.0", workflow.fingerprint);
    metrics.register_step("a", "fx-echo", "0.1.0");
    metrics.mark_started(kStartedAt);
    metrics.set_workflow_state(state);
    return metrics.snapshot().to_json();
}

const praenora::detail::CompiledSchema& run_record_schema() {
    return praenora::detail::SchemaRegistry::builtin().for_kind(
        "run-record", praenora::detail::parse_semver("0.2.0"));
}

// Every issue of `document` against the run-record schema, one per line.
std::string issues_of(const json& document) {
    std::string text;
    for (const auto& issue : run_record_schema().validate(document)) {
        text += issue.pointer + ": " + issue.message + "\n";
    }
    return text;
}

template <class Call> Error error_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "no praenora::Error was thrown";
    return Error(ErrorCode::OK, "nothing thrown");
}

// Calls `check(where, text)` for every string of `value`, object keys included; `where` is the
// string's JSON pointer.
void for_each_string(const json& value, const std::string& pointer,
                     const std::function<void(const std::string&, const std::string&)>& check) {
    if (value.is_string()) {
        check(pointer, value.get<std::string>());
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            check(pointer + " (a key)", it.key());
            for_each_string(it.value(), pointer + "/" + it.key(), check);
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            for_each_string(value[i], pointer + "/" + std::to_string(i), check);
        }
    }
}

OutputRecorder::FileInfo file_info(std::string port, std::string path, std::string data,
                                   std::uint64_t messages) {
    OutputRecorder::FileInfo info;
    info.port = std::move(port);
    info.path = std::move(path);
    info.sha256 = sha256_hex(data);
    info.messages = messages;
    return info;
}

} // namespace

TEST(RunRecord, ValidatesAgainstSchema) {
    ScratchDir dir;
    const ValidatedWorkflow workflow = validated_single(dir.path());
    const fs::path out = dir.path() / "out";
    RunOverrides overrides;
    overrides.seed = 7;
    RunRecordWriter writer(out, workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation, overrides, true);

    // Started: written at once, status running.
    writer.write_started(kStartedAt);
    const json started = json::parse(read_file(out / "run-record.json"));
    EXPECT_EQ(issues_of(started), "");
    EXPECT_EQ(started, writer.current());
    EXPECT_EQ(started.at("kind"), "run-record");
    EXPECT_EQ(started.at("format_version"), "0.2.0");
    const json& execution = started.at("execution");
    EXPECT_EQ(execution.at("status"), "running");
    EXPECT_EQ(execution.at("started_at"), "2023-11-14T22:13:20.123456789Z");
    EXPECT_TRUE(execution.at("finished_at").is_null());
    EXPECT_EQ(execution.at("ticks_completed"), 0);
    EXPECT_TRUE(execution.at("error").is_null());
    EXPECT_EQ(execution.at("overrides"), json({{"seed", 7}}));
    EXPECT_EQ(execution.at("validation"), "strict");
    EXPECT_EQ(execution.at("manifests"), workflow.manifest_locations);
    EXPECT_FALSE(execution.contains("final_status")); // only once the run has ended
    // The platform, and nothing about the machine or its user.
    const json& platform = execution.at("platform");
    EXPECT_EQ(platform.size(), 4u);
    for (const char* key : {"os", "arch", "compiler", "build_type"}) {
        EXPECT_FALSE(platform.at(key).get<std::string>().empty()) << key;
    }
    EXPECT_EQ(started.dump().find("hostname"), std::string::npos);

    // Final: completed, with the output files and the trace.
    RunSummary summary;
    summary.final_state = WorkflowState::completed;
    summary.ticks_completed = 3;
    summary.fingerprint = workflow.fingerprint;
    writer.write_final(summary, kStartedAt + 5'000'000'000,
                       {file_info("result", "outputs/result.jsonl", "lines", 3)},
                       status_json(workflow, WorkflowState::completed),
                       file_info("", "trace/messages.jsonl", "trace", 6));
    const json final_record = json::parse(read_file(out / "run-record.json"));
    EXPECT_EQ(issues_of(final_record), "");
    EXPECT_EQ(final_record, writer.current());
    const json& done = final_record.at("execution");
    EXPECT_EQ(done.at("status"), "completed");
    EXPECT_EQ(done.at("started_at"), "2023-11-14T22:13:20.123456789Z");
    EXPECT_EQ(done.at("finished_at"), "2023-11-14T22:13:25.123456789Z");
    EXPECT_EQ(done.at("ticks_completed"), 3);
    EXPECT_TRUE(done.at("error").is_null());
    EXPECT_EQ(done.at("outputs"), json({{"result",
                                         {{"file", "outputs/result.jsonl"},
                                          {"sha256", sha256_hex("lines")},
                                          {"messages", 3}}}}));
    EXPECT_EQ(
        done.at("trace"),
        json({{"file", "trace/messages.jsonl"}, {"sha256", sha256_hex("trace")}, {"messages", 6}}));
    EXPECT_EQ(done.at("final_status").at("kind"), "status-snapshot");
    EXPECT_EQ(done.at("final_status").at("workflow").at("state"), "completed");
    // The file is UTF-8 text with LF line endings, ending in a newline.
    const std::string text = read_file(out / "run-record.json");
    EXPECT_EQ(text.find('\r'), std::string::npos);
    EXPECT_EQ(text.back(), '\n');

    // A failed run records its error, with its location, its details and the module's error
    // document as the cause; a cause that is not a JSON object is kept as text.
    RunRecordWriter failed(dir.path() / "failed", workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation, {}, false);
    failed.write_started(kStartedAt);
    RunSummary failure;
    failure.final_state = WorkflowState::failed;
    failure.ticks_completed = 1;
    failure.fingerprint = workflow.fingerprint;
    ErrorRecord error;
    error.code = ErrorCode::STEP_FAILED;
    error.message = "step 'a' failed";
    error.tick = 1;
    error.at_unix_ns = kStartedAt;
    error.cause_json = R"({"code":11,"message":"bad row","details":{}})";
    error.location.pointer = "/outputs/out";
    error.details_json = R"({"step":"a","phase":"invoke","tick":1})";
    failure.error = error;
    failed.write_final(failure, kStartedAt + 1, {}, status_json(workflow, WorkflowState::failed));
    const json failed_record = json::parse(read_file(dir.path() / "failed" / "run-record.json"));
    EXPECT_EQ(issues_of(failed_record), "");
    const json& failed_execution = failed_record.at("execution");
    EXPECT_EQ(failed_execution.at("status"), "failed");
    EXPECT_EQ(failed_execution.at("validation"), "off");
    EXPECT_EQ(failed_execution.at("overrides"), json::object());
    EXPECT_EQ(failed_execution.at("error"),
              json({{"code", 700},
                    {"name", "STEP_FAILED"},
                    {"message", "step 'a' failed"},
                    {"tick", 1},
                    {"at", "2023-11-14T22:13:20.123456789Z"},
                    {"cause", {{"code", 11}, {"message", "bad row"}, {"details", json::object()}}},
                    {"location", {{"pointer", "/outputs/out"}}},
                    {"details", {{"step", "a"}, {"phase", "invoke"}, {"tick", 1}}}}));
    EXPECT_EQ(failed_execution.at("outputs"), json::object());
    EXPECT_FALSE(failed_execution.contains("trace"));

    // Only completed, stopped and failed end a run.
    RunSummary running = summary;
    running.final_state = WorkflowState::running;
    EXPECT_EQ(error_of([&] {
                  failed.write_final(running, kStartedAt, {},
                                     status_json(workflow, WorkflowState::running));
              }).code(),
              ErrorCode::INVALID_ARGUMENT);

    // Without an output directory the record is kept in memory only.
    RunRecordWriter memory({}, workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation);
    memory.write_started(kStartedAt);
    EXPECT_EQ(issues_of(memory.current()), "");
    EXPECT_EQ(memory.current().at("execution").at("status"), "running");
}

// Process isolation is not available on Windows in this version: validation refuses a step in a
// module host there (docs/specs/process-isolation.md).
#ifndef _WIN32
TEST(RunRecord, ListsEveryStepsIsolation) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{\"value\": 1}");
    const auto yaml = [](const std::string& b_isolation) {
        return "kind: workflow-config\n"
               "format_version: \"0.1.0\"\n"
               "workflow: { name: test.chain, version: \"0.1.0\" }\n"
               "run: { ticks: 1 }\n"
               "inputs:\n"
               "  source: { source: json-document, path: source.json }\n"
               "steps:\n"
               "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
               "  b:\n"
               "    module: { name: fx-echo, version: \"0.1.0\" }\n" +
               b_isolation;
    };
    // Every step, with all of its fields, whether the configuration sets them or not.
    auto validated = praenora::test::validate_text(
        dir.path(), yaml("    isolation: { mode: process, share_compiled_models: true }\n"));
    ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
    const ValidatedWorkflow& workflow = *validated.workflow;
    const json expected = json::parse(R"({"a":{"mode":"in-process","share_compiled_models":false,)"
                                      R"("sandbox":"none"},)"
                                      R"("b":{"mode":"process","share_compiled_models":true,)"
                                      R"("sandbox":"none"}})");
    EXPECT_EQ(workflow.isolation, expected);
    RunRecordWriter writer({}, workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation);
    writer.write_started(kStartedAt);
    EXPECT_EQ(issues_of(writer.current()), "");
    EXPECT_EQ(writer.current().at("execution").at("isolation"), expected);
    EXPECT_EQ(writer.current().at("execution").at("overrides"), json::object());
    // Nothing of it is in the hashed section.
    EXPECT_EQ(writer.current().at("reproducibility").dump().find("isolation"), std::string::npos);

    // A caller's override is listed with the other overrides.
    praenora::WorkflowOptions options;
    options.isolation = praenora::IsolationMode::process;
    auto overridden = praenora::test::validate_text(dir.path(), yaml(""), options);
    ASSERT_TRUE(overridden.workflow.has_value()) << overridden.report.to_text();
    EXPECT_EQ(overridden.workflow->isolation,
              json::parse(R"({"a":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("b":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
    RunRecordWriter override_writer(
        {}, overridden.workflow->reproducibility, overridden.workflow->fingerprint,
        overridden.workflow->manifest_locations, overridden.workflow->isolation,
        overridden.workflow->config.overrides);
    override_writer.write_started(kStartedAt);
    EXPECT_EQ(issues_of(override_writer.current()), "");
    EXPECT_EQ(override_writer.current().at("execution").at("overrides"),
              json({{"isolation", "process"}}));
    EXPECT_EQ(overridden.workflow->fingerprint, workflow.fingerprint);
}
#endif // _WIN32

TEST(RunRecord, FingerprintMatchesReproducibilitySection) {
    ScratchDir dir;
    const ValidatedWorkflow workflow = validated_single(dir.path());
    RunRecordWriter writer(dir.path() / "out", workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation);
    writer.write_started(kStartedAt);
    const json record = json::parse(read_file(dir.path() / "out" / "run-record.json"));
    // The reproducibility section is the validated one, verbatim, and the fingerprint is its hash.
    EXPECT_EQ(record.at("reproducibility"), workflow.reproducibility);
    EXPECT_EQ(record.at("fingerprint"), workflow.fingerprint);
    EXPECT_EQ(record.at("fingerprint"), sha256_hex(canonical_json(record.at("reproducibility"))));
    // Nothing from the execution (wall-clock time, platform, status) leaks into it.
    const std::string section = record.at("reproducibility").dump();
    EXPECT_EQ(section.find("2023-11-14"), std::string::npos);
    EXPECT_EQ(section.find("build_type"), std::string::npos);

    // A fingerprint that is not the section's hash is refused.
    EXPECT_EQ(error_of([&] {
                  RunRecordWriter(dir.path() / "other", workflow.reproducibility,
                                  std::string(64, '0'), workflow.manifest_locations,
                                  workflow.isolation);
              }).code(),
              ErrorCode::INVALID_ARGUMENT);
}

TEST(OutputRecorder, CanonicalLinesAndDigest) {
    ScratchDir dir;
    OutputRecorder recorder(dir.path(), {"state", "result"});
    // Both files exist from the start, empty.
    EXPECT_EQ(read_file(dir.path() / "outputs" / "result.jsonl"), "");
    EXPECT_EQ(read_file(dir.path() / "outputs" / "state.jsonl"), "");

    const json first =
        json::parse("{\"tick\": 0, \"data\": {\"z\": 0.1, \"a\": [1, 2], \"s\": \"s\\u00ebd\"}, "
                    "\"id\": \"x:0:a.out\"}");
    const json second = json::parse(R"({"tick": 1, "data": {"value": 1e-7}, "id": "x:1:a.out"})");
    recorder.record("result", first);
    recorder.record("result", second);
    recorder.record("state", second);
    EXPECT_EQ(error_of([&] { recorder.record("undeclared", first); }).code(),
              ErrorCode::INVALID_ARGUMENT);

    const std::vector<OutputRecorder::FileInfo> files = recorder.finish();
    ASSERT_EQ(files.size(), 2u);
    // Sorted by output name.
    EXPECT_EQ(files[0].port, "result");
    EXPECT_EQ(files[1].port, "state");
    EXPECT_EQ(files[0].path, "outputs/result.jsonl");
    EXPECT_EQ(files[1].path, "outputs/state.jsonl");
    EXPECT_EQ(files[0].messages, 2u);
    EXPECT_EQ(files[1].messages, 1u);

    // One canonical envelope per line: sorted keys, no spaces, shortest round-trip numbers, UTF-8,
    // and "\n" line endings.
    const std::string result = read_file(dir.path() / "outputs" / "result.jsonl");
    EXPECT_EQ(result, canonical_json(first) + "\n" + canonical_json(second) + "\n");
    EXPECT_EQ(result.substr(0, result.find('\n')),
              "{\"data\":{\"a\":[1,2],\"s\":\"s\xC3\xAB"
              "d\",\"z\":0.1},\"id\":\"x:0:a.out\",\"tick\":0}");
    EXPECT_EQ(files[0].sha256, sha256_hex(result));
    EXPECT_EQ(files[1].sha256, sha256_hex(read_file(dir.path() / "outputs" / "state.jsonl")));

    // Finished: nothing more is recorded, and finishing again reports the same files.
    EXPECT_EQ(error_of([&] { recorder.record("result", first); }).code(), ErrorCode::INVALID_STATE);
    const auto again = recorder.finish();
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[0].sha256, files[0].sha256);
}

TEST(TraceWriter, CanonicalLinesWithDiagnostics) {
    ScratchDir dir;
    const fs::path file = dir.path() / "trace" / "messages.jsonl";
    TraceWriter trace(file);
    const json envelope = {{"id", "x:0:a.out"}, {"data", {{"v", 1}}}};
    const json diagnostics = {{"updates", 2}};
    trace.write(0, "a", "in", "in", envelope, nullptr);
    trace.write(0, "a", "out", "out", envelope, &diagnostics);
    // A result without outputs still has its diagnostics traced, in a record without a message.
    trace.write_diagnostics(1, "a", diagnostics);
    const TraceWriter::Summary summary = trace.finish();
    const std::string text = read_file(file);
    EXPECT_EQ(text, canonical_json({{"tick", 0},
                                    {"step", "a"},
                                    {"direction", "in"},
                                    {"port", "in"},
                                    {"envelope", envelope}}) +
                        "\n" +
                        canonical_json({{"tick", 0},
                                        {"step", "a"},
                                        {"direction", "out"},
                                        {"port", "out"},
                                        {"envelope", envelope},
                                        {"diagnostics", diagnostics}}) +
                        "\n" +
                        canonical_json({{"tick", 1},
                                        {"step", "a"},
                                        {"direction", "out"},
                                        {"port", nullptr},
                                        {"envelope", nullptr},
                                        {"diagnostics", diagnostics}}) +
                        "\n");
    EXPECT_EQ(summary.messages, 2u); // the envelopes; the diagnostics-only record carries none
    EXPECT_EQ(summary.sha256, sha256_hex(text));
    EXPECT_EQ(error_of([&] { trace.write(1, "a", "in", "in", envelope, nullptr); }).code(),
              ErrorCode::INVALID_STATE);
}

TEST(TraceWriter, LookupRecords) {
    ScratchDir dir;
    const fs::path file = dir.path() / "trace" / "messages.jsonl";
    TraceWriter trace(file);
    const json envelope = {{"id", "x:3:inputs.source"}, {"data", {{"v", 1}}}};
    trace.write(3, "a", "in", "in", envelope, nullptr);
    // A lookup outside an invocation has no tick. A service name that is not valid UTF-8 is
    // written with each invalid byte replaced by U+FFFD, rather than failing the run.
    trace.write_lookup(std::nullopt, "a", "create", "praenora.simulation", 0, true, 1);
    trace.write_lookup(3, "a", "invoke", std::string("bad\xFF", 4), 2, false, 5);
    const TraceWriter::Summary summary = trace.finish();
    const std::string text = read_file(file);
    EXPECT_EQ(text, canonical_json({{"tick", 3},
                                    {"step", "a"},
                                    {"direction", "in"},
                                    {"port", "in"},
                                    {"envelope", envelope}}) +
                        "\n"
                        "{\"count\":1,\"major\":0,\"phase\":\"create\",\"provided\":true,"
                        "\"service\":\"praenora.simulation\",\"step\":\"a\",\"tick\":null}\n"
                        "{\"count\":5,\"major\":2,\"phase\":\"invoke\",\"provided\":false,"
                        "\"service\":\"bad\xEF\xBF\xBD\",\"step\":\"a\",\"tick\":3}\n");
    EXPECT_EQ(summary.messages, 1u); // the envelopes; lookup records carry none
    EXPECT_EQ(summary.sha256, sha256_hex(text));
    EXPECT_EQ(error_of([&] {
                  trace.write_lookup(4, "a", "invoke", "praenora.simulation", 0, true, 1);
              }).code(),
              ErrorCode::INVALID_STATE);
}

TEST(OutputDir, RefusesExistingRunWithoutOverwrite) {
    ScratchDir dir;
    // A directory that does not exist yet is created.
    const fs::path out = dir.path() / "runs" / "first";
    prepare_output_dir(out, false);
    EXPECT_TRUE(fs::is_directory(out));
    // One without a run record is used as it is, whatever else it holds.
    write_text_file(out / "notes.txt", "mine");
    prepare_output_dir(out, false);
    EXPECT_EQ(read_file(out / "notes.txt"), "mine");

    // One that holds an earlier run is refused, and left untouched.
    write_text_file(out / "run-record.json", "{}");
    write_text_file(out / "outputs" / "result.jsonl", "{}\n");
    const Error error = error_of([&] { prepare_output_dir(out, false); });
    EXPECT_EQ(error.code(), ErrorCode::IO_ERROR);
    EXPECT_EQ(std::string(error.what()),
              "output directory already contains a run record; use overwrite");
    EXPECT_EQ(error.location().file, praenora::detail::path_to_utf8(out / "run-record.json"));
    EXPECT_EQ(read_file(out / "run-record.json"), "{}");
    EXPECT_EQ(read_file(out / "outputs" / "result.jsonl"), "{}\n");

    // A path that is a file is no directory.
    write_text_file(dir.path() / "plain", "x");
    EXPECT_EQ(error_of([&] { prepare_output_dir(dir.path() / "plain", true); }).code(),
              ErrorCode::IO_ERROR);
}

TEST(OutputDir, OverwriteRemovesOnlyManagedFiles) {
    ScratchDir dir;
    const fs::path out = dir.path() / "run";
    write_text_file(out / "run-record.json", "{}");
    write_text_file(out / "outputs" / "result.jsonl", "{}\n");
    write_text_file(out / "outputs" / "old.jsonl", "{}\n");
    write_text_file(out / "trace" / "messages.jsonl", "{}\n");
    write_text_file(out / "status.json", "{}");
    write_text_file(out / "notes.txt", "mine");
    write_text_file(out / "logs" / "praenora.log", "earlier log\n");
    write_text_file(out / "data" / "observations.csv", "series_id\n");

    prepare_output_dir(out, true);

    // The run record, outputs/, trace/ and status.json are gone ...
    EXPECT_FALSE(fs::exists(out / "run-record.json"));
    EXPECT_FALSE(fs::exists(out / "outputs"));
    EXPECT_FALSE(fs::exists(out / "trace"));
    EXPECT_FALSE(fs::exists(out / "status.json"));
    // ... and everything else stays as it was; log files are appended to across runs.
    EXPECT_EQ(read_file(out / "notes.txt"), "mine");
    EXPECT_EQ(read_file(out / "logs" / "praenora.log"), "earlier log\n");
    EXPECT_EQ(read_file(out / "data" / "observations.csv"), "series_id\n");

    // Without a run record, overwrite removes nothing: the directory holds no earlier run.
    write_text_file(out / "outputs" / "mine.jsonl", "{}\n");
    write_text_file(out / "status.json", "{}");
    prepare_output_dir(out, true);
    EXPECT_TRUE(fs::exists(out / "outputs" / "mine.jsonl"));
    EXPECT_TRUE(fs::exists(out / "status.json"));
}

// The record says where each step's manifest was found, relative to a search path that it names
// only by origin and index. Like the rest of a record without an error, it holds no absolute path,
// so it names neither the machine's directories nor its user.
TEST(RunRecord, SaysWhereEachManifestWasFoundWithoutAnAbsolutePath) {
    ScratchDir dir;
    const ValidatedWorkflow workflow = validated_single(dir.path());
    const fs::path out = dir.path() / "out";
    RunRecordWriter writer(out, workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation);
    writer.write_started(kStartedAt);
    EXPECT_EQ(writer.current().at("execution").at("manifests"), json::parse(R"({
        "steps": {"a": {"search_path": {"origin": "caller", "index": 0},
                        "file": "fx-echo/fx-echo.pmm.json"}},
        "services": {}})"));

    RunSummary summary;
    summary.final_state = WorkflowState::completed;
    summary.ticks_completed = 3;
    summary.fingerprint = workflow.fingerprint;
    writer.write_final(summary, kStartedAt + 1'000'000'000,
                       {file_info("result", "outputs/result.jsonl", "lines", 3)},
                       status_json(workflow, WorkflowState::completed));
    const json record = json::parse(read_file(out / "run-record.json"));
    EXPECT_EQ(issues_of(record), "");
    EXPECT_EQ(record.at("execution").at("manifests"), workflow.manifest_locations);

    const std::string modules =
        path_to_utf8(praenora::test::utf8_path(PRAENORA_TEST_MODULES_DIR).lexically_normal());
    const std::string scratch = path_to_utf8(dir.path());
    for_each_string(record, "", [&](const std::string& where, const std::string& text) {
        EXPECT_FALSE(path_from_utf8(text).is_absolute()) << where << ": " << text;
        EXPECT_EQ(text.find(modules), std::string::npos) << where << ": " << text;
        EXPECT_EQ(text.find(scratch), std::string::npos) << where << ": " << text;
    });
}

namespace {

// The final status of a failed test.single run whose workflow and step a have `error` as their
// last error.
std::string failed_status_json(const ValidatedWorkflow& workflow, const ErrorRecord& error) {
    MetricsRegistry metrics;
    metrics.set_run_info("test.single", "0.1.0", workflow.fingerprint);
    metrics.register_step("a", "fx-echo", "0.1.0");
    metrics.mark_started(kStartedAt);
    metrics.set_step_error("a", error);
    metrics.set_last_error(error);
    metrics.set_workflow_state(WorkflowState::failed);
    return metrics.snapshot().to_json();
}

// `record` with its error records (execution.error and the last errors of final_status) null.
json without_error_records(json record) {
    json& execution = record.at("execution");
    execution["error"] = nullptr;
    execution["final_status"]["workflow"]["last_error"] = nullptr;
    for (json& step : execution["final_status"]["steps"]) {
        step["last_error"] = nullptr;
    }
    return record;
}

} // namespace

// The error records of the run record name the directories the run knows by placeholders: the
// output directory, the configuration's directory, the module search paths and the home
// directory, in the record kept (current()) and in the file alike. Nothing else changes; a writer
// without anchors keeps the absolute paths.
TEST(RunRecord, ErrorRecordsNameKnownDirectoriesByPlaceholders) {
    ScratchDir dir;
    const fs::path out = dir.path() / "out";
    const fs::path home = dir.path() / "home";
    const fs::path data = home / "data" / "x.csv";
    // Step a's parameter names a file in the home directory: the reproducibility section keeps it
    // as it is, since only the error records name directories by placeholders.
    const ValidatedWorkflow workflow =
        validated_single(dir.path(), "{ note: " + json(path_to_utf8(data)).dump() + " }");
    const fs::path configuration = workflow.config.file.parent_path();
    ASSERT_FALSE(workflow.module_search_paths.empty());
    const praenora::detail::SearchPath& modules = workflow.module_search_paths.front();
    ASSERT_EQ(modules.path,
              praenora::test::utf8_path(PRAENORA_TEST_MODULES_DIR).lexically_normal());
    ASSERT_EQ(modules.origin, praenora::detail::SearchPathOrigin::caller);
    praenora::detail::RunDirectories directories;
    directories.output = out;
    directories.configuration = configuration;
    directories.modules = workflow.module_search_paths;
    directories.home = home;
    const auto anchors = std::make_shared<const praenora::detail::PathAnchors>(directories);

    const fs::path outputs = out / "outputs";
    const fs::path library = modules.path / "fx-echo" / "fx-echo.so";
    const fs::path config_file = configuration / "workflow.yaml";
    ErrorRecord error;
    error.code = ErrorCode::IO_ERROR;
    error.message = "cannot create directory '" + path_to_utf8(outputs) +
                    "': File exists (library '" + path_to_utf8(library) + "')";
    error.at_unix_ns = kStartedAt;
    error.cause_json = json({{"code", 11},
                             {"message", "bad row in " + path_to_utf8(data)},
                             {"details", {{"files", {path_to_utf8(data), 2}}}}})
                           .dump();
    error.location.file = path_to_utf8(config_file);
    error.location.pointer = "/outputs/directory";
    error.details_json =
        json({{"library", path_to_utf8(library)}, {"home", path_to_utf8(home)}}).dump();
    RunSummary failure;
    failure.final_state = WorkflowState::failed;
    failure.fingerprint = workflow.fingerprint;
    failure.error = error;
    const std::string status = failed_status_json(workflow, error);
    const std::vector<OutputRecorder::FileInfo> files = {
        file_info("result", "outputs/result.jsonl", "lines", 0)};
    const OutputRecorder::FileInfo trace = file_info("", "trace/messages.jsonl", "trace", 0);

    RunRecordWriter writer(out, workflow.reproducibility, workflow.fingerprint,
                           workflow.manifest_locations, workflow.isolation, {}, true, anchors);
    writer.write_started(kStartedAt);
    writer.write_final(failure, kStartedAt + 1, files, status, trace);
    const json record = json::parse(read_file(out / "run-record.json"));
    EXPECT_EQ(record, writer.current());
    EXPECT_EQ(issues_of(record), "");

    const std::string modules_library = named_path("<modules:caller:0>", modules.path, library);
    const json expected_error = {
        {"code", static_cast<std::int32_t>(ErrorCode::IO_ERROR)},
        {"name", "IO_ERROR"},
        {"message", "cannot create directory '" + named_path("<output>", out, outputs) +
                        "': File exists (library '" + modules_library + "')"},
        {"tick", nullptr},
        {"at", "2023-11-14T22:13:20.123456789Z"},
        {"cause",
         {{"code", 11},
          {"message", "bad row in " + named_path("<home>", home, data)},
          {"details", {{"files", {named_path("<home>", home, data), 2}}}}}},
        {"location",
         {{"file", named_path("<configuration>", configuration, config_file)},
          {"pointer", "/outputs/directory"}}},
        {"details", {{"library", modules_library}, {"home", "<home>"}}}};
    const json& execution = record.at("execution");
    EXPECT_EQ(execution.at("error"), expected_error);
    EXPECT_EQ(execution.at("final_status").at("workflow").at("last_error"), expected_error);
    EXPECT_EQ(execution.at("final_status").at("steps").at(0).at("last_error"), expected_error);

    // A writer without anchors writes the same record with the absolute paths.
    RunRecordWriter plain(dir.path() / "plain", workflow.reproducibility, workflow.fingerprint,
                          workflow.manifest_locations, workflow.isolation, {}, true);
    plain.write_started(kStartedAt);
    plain.write_final(failure, kStartedAt + 1, files, status, trace);
    const json& plain_error = plain.current().at("execution").at("error");
    EXPECT_EQ(plain_error.at("message"), error.message);
    EXPECT_EQ(plain_error.at("location").at("file"), error.location.file);
    EXPECT_EQ(plain_error.at("details"), json::parse(error.details_json));
    EXPECT_EQ(plain_error.at("cause"), json::parse(error.cause_json));
    EXPECT_EQ(plain.current().at("execution").at("final_status").at("workflow").at("last_error"),
              plain_error);
    // Nothing but the error records differs: the reproducibility section and the fingerprint
    // above all.
    EXPECT_EQ(without_error_records(record), without_error_records(plain.current()));
    EXPECT_EQ(record.at("reproducibility"), workflow.reproducibility);
    EXPECT_EQ(record.at("fingerprint"), workflow.fingerprint);
    EXPECT_EQ(execution.at("manifests"), workflow.manifest_locations);
    // The parameter's path stays absolute in the reproducibility section, in the record kept as in
    // the file, and the fingerprint is still the hash of that section.
    for (const json* kept : {&record, &writer.current()}) {
        EXPECT_EQ(kept->at("reproducibility").at("steps").at("a").at("parameters").at("note"),
                  path_to_utf8(data));
        EXPECT_EQ(kept->at("fingerprint"), workflow.fingerprint);
        EXPECT_EQ(sha256_hex(canonical_json(kept->at("reproducibility"))), workflow.fingerprint);
    }

    // No anchored directory is left in the execution section, as given or with links resolved.
    std::vector<std::string> directory_texts;
    for (const fs::path& directory : {dir.path(), modules.path}) {
        directory_texts.push_back(path_to_utf8(directory));
        directory_texts.push_back(path_to_utf8(fs::weakly_canonical(directory)));
    }
    for_each_string(
        execution, "/execution", [&](const std::string& where, const std::string& text) {
            for (const std::string& directory : directory_texts) {
                EXPECT_EQ(text.find(directory), std::string::npos) << where << ": " << text;
            }
        });
}
