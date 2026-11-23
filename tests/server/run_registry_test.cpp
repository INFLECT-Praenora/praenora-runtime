// The runs behind the control API (src/server/run_registry.hpp), with the fixture modules and the
// test catalog: serve mode's RunRegistry (the output root and its numbering, the configuration
// root and the names it refuses, max_runs, deleting, forced isolation, the status's placeholders,
// the errors of Runtime::open(), shutdown, queries racing a remove and a shutdown, and the same
// results as a run of `praenora run`) and run mode's SingleRun.
#include "clock.hpp"
#include "control_api.hpp"
#include "run_registry.hpp"

#include "server_fixtures.hpp"

#include "directory_checks.hpp"
#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::IsolationMode;
using praenora::Runtime;
using praenora::Workflow;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::server::ControlApi;
using praenora::server::ControlApiSettings;
using praenora::server::first_run_number;
using praenora::server::output_directory_refusal;
using praenora::server::RegistrySettings;
using praenora::server::resolve_configuration;
using praenora::server::Response;
using praenora::server::RunRegistry;
using praenora::server::SingleRun;
using praenora::test::body_of;
using praenora::test::config_step;
using praenora::test::error_of;
using praenora::test::is_final;
using praenora::test::post_json;
using praenora::test::request;
using praenora::test::ScratchDir;
using praenora::test::utf8;
using praenora::test::wait_for;
using praenora::test::write_fixture_config;
using praenora::test::write_text_file;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const Runtime& fixture_runtime() {
    static const Runtime instance(praenora::test::fixture_runtime_options());
    return instance;
}

ControlApiSettings loopback_settings() {
    ControlApiSettings settings;
    settings.listener = praenora::server::parse_listen_address("127.0.0.1:8080");
    settings.started_at_unix_ns = praenora::server::now_unix_ns();
    settings.pid = praenora::server::current_process_id();
    return settings;
}

// A configuration root with single.yaml (test.single, one tick of fx-echo), slow.yaml (test.chain
// whose step b sleeps 5 s in each of its 3 invocations, polling for cancellation) and
// unknown.yaml (a module that does not exist), and an output root beside it.
struct Roots {
    ScratchDir dir;
    fs::path configs = dir.path() / "configs";
    fs::path outputs = dir.path() / "outputs";

    Roots() {
        write_fixture_config(configs / "single.yaml", "test.single", "run: { ticks: 1 }\n",
                             config_step("a", "fx-echo"));
        write_fixture_config(configs / "slow.yaml", "test.chain", "run: { ticks: 3 }\n",
                             config_step("a", "fx-echo") +
                                 config_step("b", "fx-slow", "{ sleep_ms: 5000 }"));
        write_fixture_config(configs / "unknown.yaml", "test.single", "run: { ticks: 1 }\n",
                             config_step("a", "fx-nowhere"));
    }

    RegistrySettings settings(std::size_t max_runs = 16) const {
        RegistrySettings result;
        result.configuration_root = utf8(configs);
        result.output_root = utf8(outputs);
        result.max_runs = max_runs;
        return result;
    }
};

// Opens a run of `configuration` through the API and returns its answer.
Response create(const ControlApi& api, const std::string& configuration,
                const std::string& more = "") {
    return api.handle(
        post_json("/v0/runs", R"({"configuration":")" + configuration + "\"" + more + "}"));
}

// Waits until the run `id` of `registry` ended; returns whether it did.
bool wait_until_over(const RunRegistry& registry, const std::string& id) {
    return wait_for([&] {
        const auto run = registry.find(id);
        return run != nullptr && is_final(run->status().state);
    });
}

// Waits until the run `id` of `registry` is running.
bool wait_until_running(const RunRegistry& registry, const std::string& id) {
    return wait_for([&] {
        const auto run = registry.find(id);
        return run != nullptr && run->status().state == WorkflowState::running;
    });
}

// Sets `done` and joins `threads` when the scope ends, however the test ends.
class JoinOnExit {
public:
    JoinOnExit(std::atomic<bool>& done, std::vector<std::thread>& threads)
        : done_(done), threads_(threads) {}
    ~JoinOnExit() {
        done_ = true;
        for (std::thread& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
    JoinOnExit(const JoinOnExit&) = delete;
    JoinOnExit& operator=(const JoinOnExit&) = delete;

private:
    std::atomic<bool>& done_;
    std::vector<std::thread>& threads_;
};

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The files below `dir`, by their relative paths (with '/'), with their bytes.
std::map<std::string, std::string> files_below(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[entry.path().lexically_relative(dir).generic_string()] = read_file(entry.path());
        }
    }
    return files;
}

} // namespace

TEST(RunRegistry, RefusesBadSettingsAndCreatesTheOutputRoot) {
    Roots roots;
    RegistrySettings settings = roots.settings();
    settings.configuration_root = utf8(roots.dir.path() / "missing");
    EXPECT_EQ(error_of([&] { RunRegistry registry(fixture_runtime(), settings); }).code(),
              ErrorCode::INVALID_ARGUMENT);
    settings.configuration_root = utf8(roots.configs / "single.yaml");
    EXPECT_EQ(error_of([&] { RunRegistry registry(fixture_runtime(), settings); }).code(),
              ErrorCode::INVALID_ARGUMENT);
    settings = roots.settings(0);
    const Error zero = error_of([&] { RunRegistry registry(fixture_runtime(), settings); });
    EXPECT_EQ(zero.code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_TRUE(contains(zero.what(), "--max-runs")) << zero.what();
    // The output root is created with its parents.
    settings = roots.settings();
    settings.output_root = utf8(roots.dir.path() / "deep" / "er" / "outputs");
    const RunRegistry registry(fixture_runtime(), settings);
    EXPECT_TRUE(fs::is_directory(roots.dir.path() / "deep" / "er" / "outputs"));
    EXPECT_EQ(registry.next_run_number(), 1u);
    EXPECT_TRUE(registry.list().empty());
    // An output root that is a file cannot be one.
    write_text_file(roots.dir.path() / "file", "x");
    settings.output_root = utf8(roots.dir.path() / "file");
    EXPECT_EQ(error_of([&] { RunRegistry file_root(fixture_runtime(), settings); }).code(),
              ErrorCode::IO_ERROR);
}

TEST(ConfigurationRoot, AcceptsNamesInsideTheRoot) {
    ScratchDir dir;
    const fs::path root = dir.path() / "root";
    write_text_file(root / "a.yaml", "x");
    write_text_file(root / "sub" / "deeper" / "b.yaml", "x");
    // A non-ASCII name inside the root is accepted.
    write_text_file(root /
                        praenora::test::utf8_path("\xC3\xBCnic\xC3\xB6"
                                                  "de") /
                        praenora::test::utf8_path("w\xC3\xB6rk.yaml"),
                    "x");
    const fs::path canonical = fs::canonical(root);
    EXPECT_EQ(resolve_configuration(canonical, "a.yaml"), canonical / "a.yaml");
    EXPECT_EQ(resolve_configuration(canonical, "sub/deeper/b.yaml"),
              canonical / "sub" / "deeper" / "b.yaml");
    EXPECT_EQ(resolve_configuration(canonical, "\xC3\xBCnic\xC3\xB6"
                                               "de/w\xC3\xB6rk.yaml"),
              fs::canonical(root /
                            praenora::test::utf8_path("\xC3\xBCnic\xC3\xB6"
                                                      "de") /
                            praenora::test::utf8_path("w\xC3\xB6rk.yaml")));
#ifndef _WIN32
    // A symbolic link that stays inside the root is accepted, as its target.
    fs::create_symlink("a.yaml", root / "inner.yaml");
    fs::create_directory_symlink("sub", root / "linked");
    EXPECT_EQ(resolve_configuration(canonical, "inner.yaml"), canonical / "a.yaml");
    EXPECT_EQ(resolve_configuration(canonical, "linked/deeper/b.yaml"),
              canonical / "sub" / "deeper" / "b.yaml");
#endif
}

TEST(ConfigurationRoot, RefusesHostileNames) {
    ScratchDir dir;
    const fs::path root = dir.path() / "root";
    write_text_file(root / "a.yaml", "x");
    write_text_file(root / "sub" / "b.yaml", "x");
    write_text_file(dir.path() / "outside.yaml", "x");
    const fs::path canonical = fs::canonical(root);
    std::vector<std::string> names = {"",
                                      "../outside.yaml",
                                      "sub/../../outside.yaml",
                                      "sub/../a.yaml",
                                      "./a.yaml",
                                      "sub/./b.yaml",
                                      "sub//b.yaml",
                                      "sub/",
                                      "/a.yaml",
                                      utf8(canonical / "a.yaml"),
                                      "sub\\b.yaml",
                                      "..\\outside.yaml",
                                      std::string("a.yaml\0x", 8),
                                      "a\nb.yaml",
                                      "a\x7f.yaml",
                                      ".",
                                      ".."};
#ifdef _WIN32
    names.push_back("C:a.yaml");
    names.push_back("C:/a.yaml");
#else
    // Symbolic links that leave the root, to a file or through a directory.
    fs::create_symlink(dir.path() / "outside.yaml", root / "escape.yaml");
    fs::create_directory_symlink(dir.path(), root / "up");
    names.push_back("escape.yaml");
    // Beyond a link that leaves the root, whether its target exists or not: nothing outside the
    // root can be probed.
    names.push_back("up/outside.yaml");
    names.push_back("up/missing.yaml");
    names.push_back("up/missing/deeper.yaml");
    // A loop of links cannot be resolved.
    fs::create_symlink("loop.yaml", root / "loop.yaml");
    names.push_back("loop.yaml");
    names.push_back("loop.yaml/x.yaml");
    // Through a link that leaves the root and back: the canonical path lies inside it.
    EXPECT_EQ(resolve_configuration(canonical, "up/root/a.yaml"), canonical / "a.yaml");
#endif
    const std::string scratch = utf8(dir.path());
    const std::string resolved = utf8(fs::canonical(dir.path()));
    for (const std::string& name : names) {
        SCOPED_TRACE(name);
        const Error error = error_of([&] { resolve_configuration(canonical, name); });
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT) << error.to_json();
        // Messages name no absolute path of the server's (but the name the client sent).
        if (!contains(name, scratch) && !contains(name, resolved)) {
            EXPECT_FALSE(contains(error.what(), scratch)) << error.what();
            EXPECT_FALSE(contains(error.what(), resolved)) << error.what();
        }
    }
}

TEST(ConfigurationRoot, MissingFileIsNotFoundAndADirectoryIsRefused) {
    ScratchDir dir;
    const fs::path root = dir.path() / "root";
    write_text_file(root / "sub" / "b.yaml", "x");
    const fs::path canonical = fs::canonical(root);
    const Error missing = error_of([&] { resolve_configuration(canonical, "missing.yaml"); });
    EXPECT_EQ(missing.code(), ErrorCode::NOT_FOUND) << missing.to_json();
    EXPECT_TRUE(contains(missing.what(), "'missing.yaml'")) << missing.what();
    EXPECT_FALSE(contains(missing.what(), utf8(dir.path()))) << missing.what();
    EXPECT_EQ(error_of([&] { resolve_configuration(canonical, "sub/missing/c.yaml"); }).code(),
              ErrorCode::NOT_FOUND);
    EXPECT_EQ(error_of([&] { resolve_configuration(canonical, "sub"); }).code(),
              ErrorCode::INVALID_ARGUMENT);
}

TEST(OutputRoot, FirstRunNumberFollowsTheHighestRunEntry) {
    ScratchDir dir;
    EXPECT_EQ(first_run_number(dir.path() / "missing"), 1u);
    EXPECT_EQ(first_run_number(dir.path()), 1u);
    fs::create_directories(dir.path() / "run-3");
    fs::create_directories(dir.path() / "run-7");
    write_text_file(dir.path() / "run-x", "a file named run-x");
    write_text_file(dir.path() / "run-12.txt", "x");
    write_text_file(dir.path() / "run-", "x");
    write_text_file(dir.path() / "run-1a", "x");
    write_text_file(dir.path() / "Run-30", "x");
    fs::create_directories(dir.path() / "other" / "run-40");
    EXPECT_EQ(first_run_number(dir.path()), 8u);
    // A log file of a run counts, as its directory does.
    write_text_file(dir.path() / "run-9.log", "x");
    EXPECT_EQ(first_run_number(dir.path()), 10u);
    fs::create_directories(dir.path() / "run-0011");
    EXPECT_EQ(first_run_number(dir.path()), 12u);
    // Numbers beyond what a run id can be are ignored.
    fs::create_directories(dir.path() / "run-99999999999999999999");
    EXPECT_EQ(first_run_number(dir.path()), 12u);
}

TEST(RunRegistry, OpensRunsUnderTheOutputRoot) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    const Response created =
        create(api, "single.yaml", R"(,"seed":7,"ticks":2,"trace_messages":true)");
    ASSERT_EQ(created.status, 201) << created.body;
    ASSERT_NE(created.header("Location"), nullptr);
    EXPECT_EQ(*created.header("Location"), "/v0/runs/1");
    const json entry = body_of(created);
    EXPECT_EQ(entry.at("id"), "1");
    EXPECT_EQ(entry.at("configuration"), "single.yaml");
    EXPECT_EQ(entry.at("output"), "run-1");
    EXPECT_EQ(entry.at("workflow"), json({{"name", "test.single"}, {"version", "0.1.0"}}));
    EXPECT_GE(entry.at("run_number").get<std::uint64_t>(), 1u);
    EXPECT_EQ(entry.at("fingerprint").get<std::string>().size(), 64u);
    EXPECT_TRUE(std::regex_match(entry.at("created_at").get<std::string>(),
                                 std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{9}Z)")))
        << entry.dump();
    ASSERT_TRUE(wait_until_over(registry, "1"));

    const json listed = body_of(api.handle(request("GET", "/v0/runs"))).at("runs");
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.at(0).at("state"), "completed");
    EXPECT_EQ(listed.at(0).at("health"), "healthy");
    EXPECT_EQ(listed.at(0).at("ticks_completed"), 2);
    EXPECT_EQ(listed.at(0).at("created_at"), entry.at("created_at"));
    EXPECT_EQ(listed.at(0).at("run_number"), entry.at("run_number"));

    const Response record = api.handle(request("GET", "/v0/runs/1/record"));
    ASSERT_EQ(record.status, 200) << record.body;
    const json document = body_of(record);
    EXPECT_EQ(document.at("execution").at("status"), "completed");
    EXPECT_EQ(document.at("reproducibility").at("seed"), 7);
    EXPECT_EQ(document.at("reproducibility").at("ticks"), 2);
    EXPECT_EQ(document.at("fingerprint"), entry.at("fingerprint"));
    EXPECT_EQ(body_of(api.handle(request("GET", "/v0/runs/1"))).at("kind"), "status-snapshot");
    EXPECT_EQ(body_of(api.handle(request("GET", "/v0/runs/1/metrics"))).at("metrics").size(), 8u);
    EXPECT_TRUE(body_of(api.handle(request("GET", "/v0/runs/1/outputs"))).is_object());
    const Response prometheus = api.handle(request("GET", "/metrics"));
    ASSERT_EQ(prometheus.status, 200);
    EXPECT_TRUE(contains(prometheus.body,
                         R"(praenora_workflow_ticks_total{praenora_workflow="test.single@0.1.0",)"
                         R"(praenora_run_number=")" +
                             std::to_string(entry.at("run_number").get<std::uint64_t>()) +
                             R"(",praenora_run_fingerprint=")" +
                             entry.at("fingerprint").get<std::string>() + "\"} 2\n"))
        << prometheus.body;

    // Its files: the output directory with the trace, the log file beside it, no status file.
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-1" / "run-record.json"));
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-1" / "trace" / "messages.jsonl"));
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-1.log"));
    EXPECT_FALSE(fs::exists(roots.outputs / "run-1" / "status.json"));
    EXPECT_TRUE(contains(read_file(roots.outputs / "run-1.log"), "run.completed"));

    // The next run is run 2, in its own directory.
    const Response second = create(api, "single.yaml");
    ASSERT_EQ(second.status, 201) << second.body;
    EXPECT_EQ(body_of(second).at("id"), "2");
    EXPECT_EQ(body_of(second).at("output"), "run-2");
    EXPECT_EQ(body_of(second).at("run_number"), entry.at("run_number").get<std::uint64_t>() + 1);
    ASSERT_TRUE(wait_until_over(registry, "2"));
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-2" / "run-record.json"));
}

TEST(RunRegistry, NumbersRunsAfterTheHighestEntryOfTheOutputRoot) {
    Roots roots;
    fs::create_directories(roots.outputs / "run-3");
    fs::create_directories(roots.outputs / "run-7");
    write_text_file(roots.outputs / "run-x", "ignored");
    RunRegistry registry(fixture_runtime(), roots.settings());
    EXPECT_EQ(registry.next_run_number(), 8u);
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    const Response created = create(api, "single.yaml");
    ASSERT_EQ(created.status, 201) << created.body;
    EXPECT_EQ(body_of(created).at("id"), "8");
    EXPECT_EQ(body_of(created).at("output"), "run-8");
    ASSERT_TRUE(wait_until_over(registry, "8"));
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-8" / "run-record.json"));
    EXPECT_EQ(registry.next_run_number(), 9u);
    EXPECT_EQ(api.handle(request("GET", "/v0/runs/1")).status, 404);
}

TEST(RunRegistry, MaxRunsAnswers503) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings(1));
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    ASSERT_EQ(create(api, "single.yaml").status, 201);
    ASSERT_TRUE(wait_until_over(registry, "1"));
    // Finished runs count too, until they are deleted.
    const Response refused = create(api, "single.yaml");
    EXPECT_EQ(refused.status, 503) << refused.body;
    EXPECT_EQ(body_of(refused).at("name"), "RESOURCE_EXHAUSTED");
    EXPECT_TRUE(contains(body_of(refused).at("message"), "--max-runs")) << refused.body;
    EXPECT_EQ(api.handle(request("DELETE", "/v0/runs/1")).status, 204);
    const Response next = create(api, "single.yaml");
    ASSERT_EQ(next.status, 201) << next.body;
    EXPECT_EQ(body_of(next).at("id"), "2");
    ASSERT_TRUE(wait_until_over(registry, "2"));
}

TEST(RunRegistry, DeleteWhileRunningIs409) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    ASSERT_EQ(create(api, "slow.yaml").status, 201);
    ASSERT_TRUE(wait_until_running(registry, "1"));
    const Response refused = api.handle(request("DELETE", "/v0/runs/1"));
    EXPECT_EQ(refused.status, 409) << refused.body;
    EXPECT_EQ(body_of(refused).at("name"), "INVALID_STATE");
    // stop twice: 202 twice.
    for (int i = 0; i < 2; ++i) {
        const Response stopped = api.handle(post_json("/v0/runs/1/stop", ""));
        EXPECT_EQ(stopped.status, 202) << stopped.body;
        EXPECT_EQ(body_of(stopped).at("id"), "1");
    }
    ASSERT_TRUE(wait_until_over(registry, "1"));
    EXPECT_EQ(registry.find("1")->status().state, WorkflowState::stopped);
    // A run whose state is final is deleted even while its thread winds up.
    const Response deleted = api.handle(request("DELETE", "/v0/runs/1"));
    EXPECT_EQ(deleted.status, 204) << deleted.body;
    EXPECT_EQ(api.handle(request("GET", "/v0/runs/1")).status, 404);
    EXPECT_EQ(api.handle(request("DELETE", "/v0/runs/1")).status, 404);
    EXPECT_EQ(body_of(api.handle(request("GET", "/v0/runs"))).at("runs"), json::array());
    // Its files stay.
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-1" / "run-record.json"));
    EXPECT_TRUE(fs::is_regular_file(roots.outputs / "run-1.log"));
}

TEST(RunRegistry, ForcedIsolationRefusesAnotherOne) {
    Roots roots;
    RegistrySettings settings = roots.settings();
    settings.isolation = IsolationMode::in_process;
    RunRegistry registry(fixture_runtime(), settings);
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    const Response refused = create(api, "single.yaml", R"(,"isolation":"process")");
    EXPECT_EQ(refused.status, 400) << refused.body;
    EXPECT_EQ(body_of(refused).at("name"), "INVALID_ARGUMENT");
    EXPECT_TRUE(contains(body_of(refused).at("message"), "--isolation in-process")) << refused.body;
    EXPECT_EQ(registry.next_run_number(), 1u);
    ASSERT_EQ(create(api, "single.yaml", R"(,"isolation":"in-process")").status, 201);
    ASSERT_EQ(create(api, "single.yaml").status, 201);
    for (const char* id : {"1", "2"}) {
        SCOPED_TRACE(id);
        ASSERT_TRUE(wait_until_over(registry, id));
        // The forced isolation is the run's override, which its run record lists.
        const json record = json::parse(registry.find(id)->run_record_json());
        EXPECT_EQ(record.at("execution").at("overrides").at("isolation"), "in-process");
    }
}

TEST(RunRegistry, StatusNamesTheRunsDirectoriesByPlaceholders) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    // A file where run 1's outputs directory goes: the run fails with an error that names it.
    write_text_file(roots.outputs / "run-1" / "outputs", "a file, not a directory\n");
    ASSERT_EQ(create(api, "single.yaml").status, 201);
    ASSERT_TRUE(wait_until_over(registry, "1"));
    const Response status = api.handle(request("GET", "/v0/runs/1"));
    ASSERT_EQ(status.status, 200) << status.body;
    const json snapshot = body_of(status);
    const json& error = snapshot.at("workflow").at("last_error");
    EXPECT_EQ(error.at("name"), "IO_ERROR") << status.body;
    const fs::path output = fs::absolute(roots.outputs / "run-1");
    EXPECT_EQ(error.at("location").at("file"),
              praenora::test::named_path("<output>", output, output / "outputs"));
    praenora::test::expect_names_none(snapshot,
                                      praenora::test::directory_forms({roots.dir.path()}));
    // The list's entry names no absolute path either.
    praenora::test::expect_names_none(body_of(api.handle(request("GET", "/v0/runs"))),
                                      praenora::test::directory_forms({roots.dir.path()}));
}

TEST(RunRegistry, OpenFailuresAnswerTheirStatuses) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    // A validation failure: 422 with the thrown error document, its report in the details.
    const Response invalid = create(api, "unknown.yaml");
    ASSERT_EQ(invalid.status, 422) << invalid.body;
    const json error = body_of(invalid);
    EXPECT_EQ(error.at("name"), "MODULE_NOT_FOUND");
    EXPECT_TRUE(contains(error.at("message"), "validation failed")) << invalid.body;
    EXPECT_EQ(error.at("details").at("ok"), false);
    EXPECT_FALSE(error.at("details").at("diagnostics").empty());
    // Hostile and missing names, before anything is opened.
    const Response escape = create(api, "../outputs/x.yaml");
    EXPECT_EQ(escape.status, 400) << escape.body;
    EXPECT_FALSE(contains(escape.body, utf8(roots.dir.path()))) << escape.body;
    const Response missing = create(api, "missing.yaml");
    EXPECT_EQ(missing.status, 404) << missing.body;
    EXPECT_EQ(body_of(missing).at("name"), "NOT_FOUND");
#ifndef _WIN32
    // Beyond an operator's link that leaves the root, a file that exists and one that does not
    // get the same answer.
    write_text_file(roots.dir.path() / "outside.yaml", "x");
    fs::create_directory_symlink(roots.dir.path(), roots.configs / "up");
    // So does one beyond it that cannot be resolved: a loop of links, a directory that cannot
    // be searched.
    fs::create_symlink("loop", roots.dir.path() / "loop");
    fs::create_directories(roots.dir.path() / "locked" / "inner");
    write_text_file(roots.dir.path() / "locked" / "inner" / "x.yaml", "x");
    fs::permissions(roots.dir.path() / "locked", fs::perms::none, fs::perm_options::replace);
    const praenora::test::RestorePermissions unlock(roots.dir.path() / "locked",
                                                    fs::perms::owner_all);
    for (const char* name : {"up/outside.yaml", "up/nowhere.yaml", "up/nowhere/deeper.yaml",
                             "up/loop/x.yaml", "up/locked/inner/x.yaml", "up/locked/none.yaml"}) {
        SCOPED_TRACE(name);
        const Response refused = create(api, name);
        EXPECT_EQ(refused.status, 400) << refused.body;
        EXPECT_EQ(body_of(refused).at("message").get<std::string>(),
                  std::string("configuration '") + name +
                      "': it does not resolve to a path inside the configuration root");
    }
#endif
    // The refused validation took a number; the refused names did not.
    EXPECT_EQ(registry.next_run_number(), 2u);
    EXPECT_TRUE(registry.list().empty());
    EXPECT_TRUE(registry.find("1") == nullptr);
}

TEST(RunRegistry, OutputDirectoryOfAnotherRunIs409) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    // Another server put a run into run-1 after this one numbered its runs.
    write_text_file(roots.outputs / "run-1" / "run-record.json", "{}");
    const Response refused = create(api, "single.yaml");
    EXPECT_EQ(refused.status, 409) << refused.body;
    EXPECT_EQ(body_of(refused).at("name"), "INVALID_STATE");
    EXPECT_TRUE(contains(body_of(refused).at("message"), "run-1")) << refused.body;
    EXPECT_FALSE(contains(refused.body, utf8(roots.dir.path()))) << refused.body;
    // Its number is used up; the next request takes run-2.
    const Response next = create(api, "single.yaml");
    ASSERT_EQ(next.status, 201) << next.body;
    EXPECT_EQ(body_of(next).at("id"), "2");
    ASSERT_TRUE(wait_until_over(registry, "2"));
    EXPECT_EQ(read_file(roots.outputs / "run-1" / "run-record.json"), "{}");
}

TEST(RunRegistry, ShutdownStopsEveryRunAndRefusesNewOnes) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    ASSERT_EQ(create(api, "slow.yaml").status, 201);
    ASSERT_EQ(create(api, "single.yaml").status, 201);
    ASSERT_TRUE(wait_until_running(registry, "1"));
    const auto started = std::chrono::steady_clock::now();
    registry.shutdown();
    // Well before step b's 5 s invocation would have ended on its own.
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
    EXPECT_EQ(registry.find("1")->status().state, WorkflowState::stopped);
    EXPECT_TRUE(is_final(registry.find("2")->status().state));
    const Response refused = create(api, "single.yaml");
    EXPECT_EQ(refused.status, 409) << refused.body;
    EXPECT_TRUE(contains(body_of(refused).at("message"), "shutting down")) << refused.body;
    // The queries keep working, and so does deleting.
    EXPECT_EQ(body_of(api.handle(request("GET", "/v0/runs"))).at("runs").size(), 2u);
    EXPECT_EQ(api.handle(request("GET", "/v0/runs/1/record")).status, 200);
    EXPECT_EQ(api.handle(request("DELETE", "/v0/runs/2")).status, 204);
    registry.shutdown();
}

// A POST whose open is in progress when shutdown() begins: shutdown() waits for the open, and the
// request is refused once it ends, its workflow closed without ever starting. Run under
// ThreadSanitizer (dev-tsan) too.
TEST(RunRegistry, OpenRacingShutdownIsRefusedAndNeverStarts) {
    Roots roots;
    RegistrySettings settings = roots.settings();
    RunRegistry* registry_of_hook = nullptr;
    std::thread stopper;
    std::atomic<bool> hooked{false};
    settings.after_open = [&](std::uint64_t) {
        hooked = true;
        stopper = std::thread([&] { registry_of_hook->shutdown(); });
        EXPECT_TRUE(wait_for([&] { return registry_of_hook->closing(); }));
    };
    RunRegistry registry(fixture_runtime(), settings);
    registry_of_hook = &registry;
    struct JoinStopper {
        std::thread& thread;
        ~JoinStopper() {
            if (thread.joinable()) {
                thread.join();
            }
        }
    } joining{stopper};
    const ControlApi api(fixture_runtime(), registry, loopback_settings());

    const Response refused = create(api, "single.yaml");
    ASSERT_TRUE(hooked.load());
    stopper.join();
    EXPECT_EQ(refused.status, 409) << refused.body;
    EXPECT_TRUE(contains(body_of(refused).at("message"), "shutting down")) << refused.body;
    EXPECT_TRUE(registry.closing());
    EXPECT_TRUE(registry.list().empty());
    EXPECT_TRUE(registry.find("1") == nullptr);
    // Its number was taken; it never started (its log has no run.started).
    EXPECT_EQ(registry.next_run_number(), 2u);
    EXPECT_FALSE(contains(read_file(roots.outputs / "run-1.log"), "run.started"));
    EXPECT_EQ(create(api, "single.yaml").status, 409);
}

// DELETE racing shutdown(): each run's thread is joined once, by whichever takes it; every
// DELETE answers 204, or 404 for a run gone already, or 409 for the run still running until the
// shutdown stopped it; no run is left. Run under ThreadSanitizer (dev-tsan) too.
TEST(RunRegistry, DeleteRacingShutdownJoinsEachRunOnce) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    for (int i = 1; i <= 6; ++i) {
        ASSERT_EQ(create(api, "single.yaml").status, 201);
    }
    ASSERT_EQ(create(api, "slow.yaml").status, 201);
    for (const char* id : {"1", "2", "3", "4", "5", "6"}) {
        ASSERT_TRUE(wait_until_over(registry, id)) << id;
    }
    ASSERT_TRUE(wait_until_running(registry, "7"));

    std::atomic<bool> go{false}, done{false};
    std::atomic<int> unexpected{0};
    std::vector<std::thread> threads;
    const JoinOnExit joining(done, threads);
    threads.emplace_back([&] {
        while (!go.load()) {
            std::this_thread::yield();
        }
        std::vector<std::string> left = {"7", "1", "2", "3", "4", "5", "6"};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!left.empty() && std::chrono::steady_clock::now() < deadline) {
            for (auto it = left.begin(); it != left.end();) {
                const int status = api.handle(request("DELETE", "/v0/runs/" + *it)).status;
                if (status == 204 || status == 404) {
                    it = left.erase(it);
                    continue;
                }
                if (status != 409 || *it != "7") {
                    ++unexpected;
                }
                ++it;
            }
            if (!left.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); // run 7 winds down
            }
        }
        unexpected += static_cast<int>(left.size());
    });
    threads.emplace_back([&] {
        while (!go.load()) {
            std::this_thread::yield();
        }
        registry.shutdown();
    });
    go = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(unexpected.load(), 0);
    EXPECT_TRUE(registry.list().empty());
    EXPECT_TRUE(registry.closing());
    // Every run's files stay.
    for (int i = 1; i <= 7; ++i) {
        EXPECT_TRUE(
            fs::is_regular_file(roots.outputs / ("run-" + std::to_string(i)) / "run-record.json"))
            << i;
    }
}

// The error of Runtime::open() that RunRegistry::create() turns into a 409: an IO_ERROR located in
// the run's output directory, both paths resolved; one whose paths cannot be resolved is an
// IO_ERROR of its own, never blamed on another server.
TEST(OutputRoot, RefusedRunDirectoryIsAnotherServersOrUnresolvable) {
    ScratchDir dir;
    const fs::path output = dir.path() / "outputs" / "run-3";
    fs::create_directories(output);
    const auto io_error = [](const fs::path& file) {
        praenora::SourceLocation location;
        location.file = utf8(file);
        return Error(ErrorCode::IO_ERROR, "cannot use it", location);
    };
    for (const fs::path& file : {output / "run-record.json", output}) {
        SCOPED_TRACE(utf8(file));
        const std::optional<Error> refusal = output_directory_refusal(io_error(file), output, 3);
        ASSERT_TRUE(refusal.has_value());
        EXPECT_EQ(refusal->code(), ErrorCode::INVALID_STATE);
        EXPECT_TRUE(contains(refusal->what(), "run-3")) << refusal->what();
        EXPECT_FALSE(contains(refusal->what(), utf8(dir.path()))) << refusal->what();
        EXPECT_EQ(json::parse(refusal->details_json()), json({{"output", "run-3"}}));
    }
    // Elsewhere (a sibling whose name starts alike, the log file beside it), another code, no
    // location: answered as it is.
    EXPECT_FALSE(
        output_directory_refusal(io_error(dir.path() / "outputs" / "run-30" / "x"), output, 3));
    EXPECT_FALSE(
        output_directory_refusal(io_error(dir.path() / "outputs" / "run-3.log"), output, 3));
    praenora::SourceLocation inside;
    inside.file = utf8(output / "run-record.json");
    EXPECT_FALSE(
        output_directory_refusal(Error(ErrorCode::CONFIG_INVALID, "x", inside), output, 3));
    EXPECT_FALSE(output_directory_refusal(Error(ErrorCode::IO_ERROR, "x"), output, 3));
#ifndef _WIN32
    // Through a symbolic link to the output root: the same directory.
    fs::create_directory_symlink(dir.path() / "outputs", dir.path() / "alias");
    const std::optional<Error> aliased = output_directory_refusal(
        io_error(output / "run-record.json"), dir.path() / "alias" / "run-3", 3);
    ASSERT_TRUE(aliased.has_value());
    EXPECT_EQ(aliased->code(), ErrorCode::INVALID_STATE);
    // A loop of links in the error's location, or in the output directory: unresolvable.
    fs::create_symlink("loop", dir.path() / "loop");
    for (const auto& [located, directory] :
         {std::pair<fs::path, fs::path>{dir.path() / "loop" / "x", output},
          std::pair<fs::path, fs::path>{output / "run-record.json",
                                        dir.path() / "loop" / "run-3"}}) {
        const std::optional<Error> unresolved =
            output_directory_refusal(io_error(located), directory, 3);
        ASSERT_TRUE(unresolved.has_value());
        EXPECT_EQ(unresolved->code(), ErrorCode::IO_ERROR) << unresolved->to_json();
        EXPECT_TRUE(contains(unresolved->what(), "cannot be told")) << unresolved->what();
        const json details = json::parse(unresolved->details_json());
        EXPECT_EQ(details.at("output"), "run-3");
        EXPECT_EQ(details.at("cause").at("name"), "IO_ERROR");
        EXPECT_EQ(details.at("cause").at("message"), "cannot use it");
    }
    // A validation failure is answered as it is, whatever its first diagnostic locates: neither
    // blamed on another server nor turned into an IO_ERROR of its own.
    const std::string report =
        R"({"ok":false,"diagnostics":[{"severity":"error","code":9,"name":"IO_ERROR",)"
        R"("category":"environment","message":"cannot read","location":{}}]})";
    for (const fs::path& located : {dir.path() / "loop" / "x", output / "run-record.json"}) {
        praenora::SourceLocation location;
        location.file = utf8(located);
        EXPECT_FALSE(output_directory_refusal(
            Error(ErrorCode::IO_ERROR, "workflow validation failed: 1 error(s)", location, report),
            output, 3))
            << utf8(located);
    }
#endif
}

// A query racing a close: GET /v0/runs/{id}, its documents and /metrics in a loop from several
// threads while DELETE closes that run, and while the registry shuts down. Run under
// ThreadSanitizer (dev-tsan) too.
TEST(RunRegistry, QueriesRaceRemoveAndShutdown) {
    Roots roots;
    RunRegistry registry(fixture_runtime(), roots.settings());
    const ControlApi api(fixture_runtime(), registry, loopback_settings());
    ASSERT_EQ(create(api, "single.yaml", R"(,"ticks":20)").status, 201);

    std::atomic<bool> done{false};
    std::atomic<int> answers{0}, unexpected{0};
    std::vector<std::thread> readers;
    const JoinOnExit joining(done, readers);
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            const char* paths[] = {"/v0/runs",          "/v0/runs/1",         "/v0/runs/1/metrics",
                                   "/v0/runs/1/record", "/v0/runs/1/outputs", "/metrics",
                                   "/v0/runs/2",        "/v0/runs/2/record"};
            while (!done.load()) {
                for (const char* path : paths) {
                    const int status = api.handle(request("GET", path)).status;
                    if (status != 200 && status != 404 && status != 409) {
                        ++unexpected;
                    }
                    ++answers;
                }
                const int stop = api.handle(post_json("/v0/runs/1/stop", "")).status;
                if (stop != 202 && stop != 404) {
                    ++unexpected;
                }
            }
        });
    }
    ASSERT_TRUE(wait_until_over(registry, "1"));
    const Response deleted = api.handle(request("DELETE", "/v0/runs/1"));
    EXPECT_EQ(deleted.status, 204) << deleted.body;
    ASSERT_EQ(create(api, "slow.yaml").status, 201);
    ASSERT_TRUE(wait_until_running(registry, "2"));
    registry.shutdown();
    EXPECT_EQ(registry.find("2")->status().state, WorkflowState::stopped);
    const int before = answers.load();
    EXPECT_TRUE(wait_for([&] { return answers.load() > before + 100; }));
    EXPECT_EQ(unexpected.load(), 0);
}

TEST(SingleRun, ServesTheOneRunOfRunMode) {
    Roots roots;
    WorkflowOptions options;
    options.output_dir = utf8(roots.dir.path() / "out");
    const std::shared_ptr<Workflow> workflow =
        fixture_runtime().open(utf8(roots.configs / "slow.yaml"), options);
    SingleRun source(workflow, utf8(roots.configs / "slow.yaml"), 1700000000123456789LL);
    const ControlApi api(fixture_runtime(), source, loopback_settings());
    std::atomic<bool> done{false};
    std::vector<std::thread> threads;
    const JoinOnExit joining(done, threads);
    threads.emplace_back([&] {
        try {
            workflow->run();
        } catch (const Error&) {
        }
    });
    // However the test ends, the run ends: the driver is joined after this.
    struct StopOnExit {
        Workflow& workflow;
        ~StopOnExit() { workflow.request_stop(); }
    } stopping{*workflow};

    const Response listed = api.handle(request("GET", "/v0/runs"));
    ASSERT_EQ(listed.status, 200) << listed.body;
    const json runs = body_of(listed).at("runs");
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs.at(0).at("id"), "1");
    EXPECT_EQ(runs.at(0).at("run_number"), workflow->run_number());
    EXPECT_EQ(runs.at(0).at("configuration"), "slow.yaml");
    EXPECT_EQ(runs.at(0).at("output"), nullptr);
    EXPECT_EQ(runs.at(0).at("fingerprint"), workflow->fingerprint());
    EXPECT_EQ(runs.at(0).at("created_at"), "2023-11-14T22:13:20.123456789Z");
    EXPECT_EQ(body_of(api.handle(request("GET", "/v0/server"))).at("mode"), "run");
    EXPECT_EQ(api.handle(request("GET", "/v0/runs/2")).status, 404);
    EXPECT_EQ(api.handle(post_json("/v0/runs", R"({"configuration":"slow.yaml"})")).status, 404);
    EXPECT_EQ(api.handle(request("DELETE", "/v0/runs/1")).status, 404);
    EXPECT_EQ(error_of([&] { source.remove("1"); }).code(), ErrorCode::NOT_FOUND);
    EXPECT_EQ(error_of([&] { source.create({}); }).code(), ErrorCode::NOT_FOUND);

    ASSERT_TRUE(wait_for([&] { return workflow->state() == WorkflowState::running; }));
    EXPECT_EQ(body_of(api.handle(request("GET", "/v0/runs/1"))).at("workflow").at("state"),
              "running");
    EXPECT_EQ(api.handle(post_json("/v0/runs/1/stop", "")).status, 202);
    EXPECT_EQ(api.handle(post_json("/v0/runs/1/stop", "")).status, 202);
    threads.front().join();
    EXPECT_EQ(workflow->state(), WorkflowState::stopped);
    const Response record = api.handle(request("GET", "/v0/runs/1/record"));
    ASSERT_EQ(record.status, 200) << record.body;
    EXPECT_EQ(body_of(record).at("execution").at("status"), "stopped");
    EXPECT_EQ(source.list().size(), 1u);
    EXPECT_EQ(source.find("1"), source.list().front());
}

#ifdef PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR
// A run that the registry opens gives the outputs, the message trace, the run record's
// reproducibility section and the fingerprint of the same configuration run as `praenora run
// --trace` runs it: Runtime::open() with the output directory, then Workflow::run().
TEST(RunRegistry, RunGivesTheResultsOfPraenoraRun) {
    ScratchDir dir;
    praenora::RuntimeOptions options;
    options.module_paths = {PRAENORA_EXAMPLE_MODULES_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    const Runtime runtime(options);
    const fs::path example = praenora::test::utf8_path(PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR);

    WorkflowOptions direct;
    direct.output_dir = utf8(dir.path() / "direct");
    direct.trace_messages = true;
    const auto workflow = runtime.open(utf8(example / "workflow.yaml"), direct);
    const praenora::RunSummary summary = workflow->run();
    workflow->close();
    ASSERT_EQ(summary.final_state, WorkflowState::completed);

    RegistrySettings settings;
    settings.configuration_root = utf8(example);
    settings.output_root = utf8(dir.path() / "served");
    RunRegistry registry(runtime, settings);
    const ControlApi api(runtime, registry, loopback_settings());
    const Response created = create(api, "workflow.yaml", R"(,"trace_messages":true)");
    ASSERT_EQ(created.status, 201) << created.body;
    ASSERT_TRUE(wait_until_over(registry, "1"));
    const auto run = registry.find("1");
    EXPECT_EQ(run->status().state, WorkflowState::completed);
    EXPECT_EQ(body_of(created).at("fingerprint"), summary.fingerprint);

    const fs::path served = dir.path() / "served" / "run-1";
    const json direct_record = json::parse(read_file(dir.path() / "direct" / "run-record.json"));
    const json served_record = json::parse(read_file(served / "run-record.json"));
    EXPECT_EQ(served_record.at("fingerprint"), direct_record.at("fingerprint"));
    EXPECT_EQ(served_record.at("reproducibility"), direct_record.at("reproducibility"));
    const auto direct_outputs = files_below(dir.path() / "direct" / "outputs");
    EXPECT_FALSE(direct_outputs.empty());
    EXPECT_TRUE(files_below(served / "outputs") == direct_outputs);
    const std::string direct_trace = read_file(dir.path() / "direct" / "trace" / "messages.jsonl");
    EXPECT_FALSE(direct_trace.empty());
    EXPECT_TRUE(read_file(served / "trace" / "messages.jsonl") == direct_trace);
    EXPECT_EQ(json::parse(run->latest_outputs_json()),
              json::parse(workflow->latest_outputs_json()));
}
#endif
