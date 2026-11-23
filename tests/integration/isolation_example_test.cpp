// The example workflow example.estimate-predict with its steps in module hosts, through the public
// API only: its output files, its message trace, its run record's reproducibility section and
// fingerprint, and what its modules log, are those of a run in the runtime's process, byte for
// byte; only the run record's execution section says where the steps ran.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/workflow.hpp>

#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::IsolationMode;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;

#ifndef _WIN32

namespace {

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

// Every file below `dir` but the run record, by its path relative to `dir`, with its bytes.
std::map<std::string, std::string> result_files(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().filename() != "run-record.json") {
            files[utf8(entry.path().lexically_relative(dir))] = read_text(entry.path());
        }
    }
    return files;
}

// What the modules logged (records of the loggers module.<step> without an event), each as
// "<logger> <severity> <tick> <body>", in order.
std::vector<std::string> module_records(const fs::path& log) {
    std::vector<std::string> records;
    std::istringstream lines(read_text(log));
    for (std::string line; std::getline(lines, line);) {
        const json record = json::parse(line);
        const json& attributes = record.at("attributes");
        const std::string logger = attributes.value("logger", std::string());
        if (logger.compare(0, 7, "module.") != 0 || attributes.contains("praenora.event")) {
            continue;
        }
        const std::string tick =
            attributes.contains("praenora.tick") ? attributes.at("praenora.tick").dump() : "-";
        records.push_back(logger + " " + record.at("severity_text").get<std::string>() + " " +
                          tick + " " + record.at("body").get<std::string>());
    }
    return records;
}

} // namespace

TEST(IsolationExample, GivesTheBytesOfTheRuntimesProcess) {
    ScratchDir dir;
    const std::string config = utf8(utf8_path(PRAENORA_EXAMPLE_WORKFLOW_DIR) / "workflow.yaml");
    std::map<IsolationMode, fs::path> outs, logs;
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        const std::string name = mode == IsolationMode::process ? "process" : "in-process";
        SCOPED_TRACE(name);
        RuntimeOptions options;
        options.module_paths = {PRAENORA_EXAMPLE_MODULES_DIR};
        options.use_environment = false;
        options.logging.to_stderr = false;
        // The run's logging, over the configuration's: every record, as JSON lines, to a file.
        WorkflowOptions run;
        run.log_level = LogLevel::debug;
        run.log_format = LogFormat::json;
        run.log_file = utf8(dir.path() / (name + ".log"));
        run.output_dir = utf8(dir.path() / name);
        run.trace_messages = true;
        run.isolation = mode;
        const Runtime runtime(options);
        const auto workflow = runtime.open(config, run);
        EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
        outs[mode] = dir.path() / name;
        logs[mode] = dir.path() / (name + ".log");
    }
    const auto in_process = result_files(outs.at(IsolationMode::in_process));
    const auto process = result_files(outs.at(IsolationMode::process));
    std::vector<std::string> names;
    for (const auto& entry : in_process) {
        names.push_back(entry.first);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"outputs/predictions.jsonl", "outputs/state.jsonl",
                                               "trace/messages.jsonl"}));
    EXPECT_EQ(in_process, process);

    const json first =
        json::parse(read_text(outs.at(IsolationMode::in_process) / "run-record.json"));
    const json second = json::parse(read_text(outs.at(IsolationMode::process) / "run-record.json"));
    EXPECT_EQ(first.at("fingerprint"), second.at("fingerprint"));
    EXPECT_EQ(first.at("reproducibility").dump(), second.at("reproducibility").dump());
    EXPECT_EQ(first.at("execution").at("outputs"), second.at("execution").at("outputs"));
    EXPECT_EQ(second.at("execution").at("isolation"),
              json::parse(R"({"estimate":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("predict":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
    EXPECT_EQ(first.at("execution").at("isolation"),
              json::parse(R"({"estimate":{"mode":"in-process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("predict":{"mode":"in-process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));

    const auto logged = module_records(logs.at(IsolationMode::in_process));
    EXPECT_FALSE(logged.empty());
    EXPECT_EQ(logged, module_records(logs.at(IsolationMode::process)));
}

// With one step in a module host, every attribute key of the run's records carries the prefix
// praenora. (but logger): the module host's events and the module records it passes on included.
TEST(IsolationExample, EveryAttributeKeyHasThePrefix) {
    ScratchDir dir;
    // The example's configuration with its step predict in a module host, and its data named by
    // an absolute path.
    const fs::path example = utf8_path(PRAENORA_EXAMPLE_WORKFLOW_DIR);
    std::string config = read_text(example / "workflow.yaml");
    const std::string data = "path: data/observations.csv";
    const std::string level = "    log_level: debug\n";
    ASSERT_NE(config.find(data), std::string::npos);
    ASSERT_NE(config.find(level), std::string::npos);
    config.replace(config.find(data), data.size(),
                   "path: " + json(utf8(example / "data" / "observations.csv")).dump());
    config.insert(config.find(level) + level.size(), "    isolation: { mode: process }\n");
    const fs::path config_file = dir.path() / "workflow.yaml";
    std::ofstream(config_file, std::ios::binary) << config;

    RuntimeOptions options;
    options.module_paths = {PRAENORA_EXAMPLE_MODULES_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    WorkflowOptions run;
    run.log_level = LogLevel::debug;
    run.log_format = LogFormat::json;
    run.log_file = utf8(dir.path() / "run.log");
    run.output_dir = utf8(dir.path() / "out");
    const Runtime runtime(options);
    const auto workflow = runtime.open(utf8(config_file), run);
    ASSERT_EQ(workflow->run().final_state, WorkflowState::completed);
    const std::string fingerprint = workflow->fingerprint();
    workflow->close();
    const json isolation = json::parse(read_text(dir.path() / "out" / "run-record.json"))
                               .at("execution")
                               .at("isolation");
    ASSERT_EQ(isolation.at("estimate").at("mode"), "in-process");
    ASSERT_EQ(isolation.at("predict").at("mode"), "process");

    std::vector<json> started, relayed;
    std::istringstream lines(read_text(dir.path() / "run.log"));
    for (std::string line; std::getline(lines, line);) {
        const json record = json::parse(line);
        const json& attributes = record.at("attributes");
        for (const auto& attribute : attributes.items()) {
            if (attribute.key() != "logger") {
                EXPECT_EQ(attribute.key().rfind("praenora.", 0), 0u) << record.dump();
            }
        }
        if (attributes.at("logger") != "module.predict") {
            continue;
        }
        if (attributes.value("praenora.event", std::string()) == "module_host.started") {
            started.push_back(attributes);
        } else if (!attributes.contains("praenora.event")) {
            relayed.push_back(attributes);
        }
    }
    ASSERT_EQ(started.size(), 1u);
    EXPECT_EQ(started[0].at("praenora.module"), "example-predictor@0.1.0");
    EXPECT_TRUE(started[0].at("praenora.pid").is_number_integer()) << started[0];
    // The module's own records, which its module host passed on, keep the attributes of the
    // runtime's process.
    ASSERT_FALSE(relayed.empty());
    for (const json& attributes : relayed) {
        EXPECT_EQ(attributes.at("praenora.run.fingerprint"), fingerprint);
        EXPECT_EQ(attributes.at("praenora.step"), "predict");
        EXPECT_EQ(attributes.at("praenora.module"), "example-predictor@0.1.0");
        EXPECT_TRUE(attributes.contains("praenora.tick")) << attributes;
    }
}

#endif
