// The example workflow example.estimate-predict with its predict step bound to the Python module
// example-predictor-python (examples/workflows/estimate-predict/workflow-python.yaml), through
// the public API only, with the example modules and the build tree's provider python: its outputs
// are those of the C++ predictor's run (workflow.yaml), byte for byte but for the start of each
// envelope's id, which names the run (its fingerprint's first 16 hexadecimal digits), since the
// Python module does the same arithmetic in the same order; in its runtime's process and in a
// module host. What its predict step logs is what the C++ predictor logs. Built with
// PRAENORA_WITH_PYTHON_ENVIRONMENT and the examples only.
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/workflow.hpp>

#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
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

// What the step predict logged (records of the logger module.predict without an event), each as
// "<severity> <tick> <body>", in order.
std::vector<std::string> predict_records(const fs::path& log) {
    std::vector<std::string> records;
    std::istringstream lines(read_text(log));
    for (std::string line; std::getline(lines, line);) {
        const json record = json::parse(line);
        const json& attributes = record.at("attributes");
        if (attributes.value("logger", std::string()) != "module.predict" ||
            attributes.contains("praenora.event")) {
            continue;
        }
        const std::string tick =
            attributes.contains("praenora.tick") ? attributes.at("praenora.tick").dump() : "-";
        records.push_back(record.at("severity_text").get<std::string>() + " " + tick + " " +
                          record.at("body").get<std::string>());
    }
    return records;
}

// The text of an output file of a run whose fingerprint is `fingerprint`, with the start of each
// envelope's id, which names the run, made "<run>"; every id must start with the run's name.
std::string without_run_ids(const std::string& text, const std::string& fingerprint) {
    const std::string run = "\"id\":\"" + fingerprint.substr(0, 16) + ":";
    std::size_t ids = 0;
    for (std::size_t at = text.find(run); at != std::string::npos; at = text.find(run, at + 1)) {
        ++ids;
    }
    const std::regex any_run("\"id\":\"[0-9a-f]+:");
    const std::ptrdiff_t all = std::distance(
        std::sregex_iterator(text.begin(), text.end(), any_run), std::sregex_iterator());
    EXPECT_EQ(static_cast<std::ptrdiff_t>(ids), all) << text;
    return std::regex_replace(text, any_run, "\"id\":\"<run>:");
}

struct ExampleRun {
    fs::path out;
    fs::path log;
    json record;
};

// Runs the example's configuration `file` (workflow.yaml or workflow-python.yaml) with every step
// in `mode`, into <dir>/<name>, its records as JSON lines in <dir>/<name>.log.
ExampleRun run_example(const fs::path& dir, const std::string& file, const std::string& name,
                       IsolationMode mode) {
    RuntimeOptions options;
    options.module_paths = {PRAENORA_EXAMPLE_MODULES_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    WorkflowOptions run;
    run.log_level = LogLevel::debug;
    run.log_format = LogFormat::json;
    run.log_file = utf8(dir / (name + ".log"));
    run.output_dir = utf8(dir / name);
    run.isolation = mode;
    const Runtime runtime(options);
    const auto workflow = runtime.open(utf8(utf8_path(PRAENORA_EXAMPLE_WORKFLOW_DIR) / file), run);
    EXPECT_EQ(workflow->run().final_state, WorkflowState::completed) << name;
    workflow->close();
    return {dir / name, dir / (name + ".log"),
            json::parse(read_text(dir / name / "run-record.json"))};
}

} // namespace

TEST(PythonExample, RunsLikeTheCppPredictor) {
    ScratchDir dir;
    const ExampleRun cpp =
        run_example(dir.path(), "workflow.yaml", "cpp", IsolationMode::in_process);
    std::vector<IsolationMode> modes = {IsolationMode::in_process};
#ifndef _WIN32
    modes.push_back(IsolationMode::process);
#endif
    for (const IsolationMode mode : modes) {
        const std::string name =
            mode == IsolationMode::process ? "python-process" : "python-in-process";
        SCOPED_TRACE(name);
        const ExampleRun python = run_example(dir.path(), "workflow-python.yaml", name, mode);
        for (const std::string output : {"predictions", "state"}) {
            const fs::path file = fs::path("outputs") / (output + ".jsonl");
            const std::string expected =
                without_run_ids(read_text(cpp.out / file), cpp.record.at("fingerprint"));
            ASSERT_FALSE(expected.empty()) << output;
            EXPECT_EQ(
                without_run_ids(read_text(python.out / file), python.record.at("fingerprint")),
                expected)
                << output;
        }
        // The predict step is the Python module, which the provider python serves.
        const json& steps = python.record.at("reproducibility").at("steps");
        EXPECT_EQ(steps.at("predict").at("module").at("name"), "example-predictor-python");
        EXPECT_EQ(steps.at("predict").at("services"), json::array({"praenora.python@0"}));
        EXPECT_EQ(steps.at("estimate").at("module"),
                  cpp.record.at("reproducibility").at("steps").at("estimate").at("module"));
        EXPECT_EQ(steps.at("predict").at("parameters"),
                  cpp.record.at("reproducibility").at("steps").at("predict").at("parameters"));
        // It logs what the C++ predictor logs.
        const std::vector<std::string> logged = predict_records(python.log);
        EXPECT_FALSE(logged.empty());
        EXPECT_EQ(logged, predict_records(cpp.log));
    }
}
