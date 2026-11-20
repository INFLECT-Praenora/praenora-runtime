// The isolation of steps in validation: a step that runs in a module host (isolation mode
// "process") validates whatever runtime services its module declares, and the providers of those
// services are loaded where the steps that use them run (provider_loads()); on Windows any step in
// a module host is refused (UNSUPPORTED), located at the field that set the step's mode: the
// step's own isolation.mode, the run's, or none for the caller's override. Nothing is loaded.
#include "api/validation.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using praenora::ErrorCode;
using praenora::IsolationMode;
using praenora::WorkflowOptions;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

namespace {

#ifdef _WIN32
bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}
#endif

// test.chain with steps a (fx-echo) and b (bound to `b_module`), with `run` and `b_extra` (YAML
// lines of step b) inserted as given.
std::string chain(const std::string& b_module, const std::string& run,
                  const std::string& b_extra = "") {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: test.chain, version: \"0.1.0\" }\n" +
           run +
           "inputs:\n"
           "  source: { source: json-document, path: source.json }\n"
           "steps:\n"
           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
           "  b:\n"
           "    module: { name: " +
           b_module + ", version: \"0.1.0\" }\n" + b_extra;
}

// Validates `yaml` in `dir` against the fixture modules and the fixture provider test-echo.
praenora::test::Validated validate_in(const fs::path& dir, const std::string& yaml,
                                      const WorkflowOptions& options = {}) {
    static const std::unique_ptr<praenora::detail::RuntimeContext> context =
        praenora::test::make_provider_context({"test-echo"});
    write_text_file(dir / "source.json", "{\"value\": 42}");
    return praenora::test::validate_text(dir, yaml, options, *context);
}

} // namespace

#ifndef _WIN32

TEST(IsolationValidation, AModuleHostStepValidates) {
    ScratchDir dir;
    const auto validated = validate_in(
        dir.path(), chain("fx-echo", "run: { ticks: 1, isolation: { mode: process } }\n"));
    ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
    EXPECT_TRUE(validated.report.diagnostics.empty()) << validated.report.to_text();
    for (const auto& step : validated.workflow->steps) {
        EXPECT_EQ(step.isolation.mode, IsolationMode::process) << step.id;
    }
}

// A module that declares a service it requires (fx-service, served by test-echo) or can use
// (fx-service-optional) runs in a module host as in the runtime's process: the step validates,
// whichever field set its mode, the caller's override included, with the services of the runtime's
// process.
TEST(IsolationValidation, AModuleHostStepWhoseModuleDeclaresServicesValidates) {
    struct Case {
        std::string module, run, b_extra;
        WorkflowOptions options;
    };
    WorkflowOptions override;
    override.isolation = IsolationMode::process;
    const std::vector<Case> cases = {
        {"fx-service", "run: { ticks: 1 }\n", "    isolation: { mode: process }\n", {}},
        {"fx-service", "run: { ticks: 1, isolation: { mode: process } }\n", "", {}},
        {"fx-service-optional", "run: { ticks: 1 }\n", "    isolation: { mode: process }\n", {}},
        {"fx-service", "run: { ticks: 1 }\n", "", override},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.module + " " + c.run + c.b_extra);
        ScratchDir dir;
        const auto validated =
            validate_in(dir.path(), chain(c.module, c.run, c.b_extra), c.options);
        ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
        EXPECT_TRUE(validated.errors().empty()) << validated.report.to_text();
        const auto& steps = validated.workflow->steps;
        const auto b =
            std::find_if(steps.begin(), steps.end(),
                         [](const praenora::detail::ResolvedStep& s) { return s.id == "b"; });
        ASSERT_NE(b, steps.end());
        EXPECT_EQ(b->isolation.mode, IsolationMode::process);
        EXPECT_EQ(b->services, std::vector<std::string>{"test.echo@0"});
        EXPECT_EQ(validated.workflow->services.count("test.echo@0"), 1u);
    }
}

// share_compiled_models applies only to a step that runs in a module host: a step that resolves to
// mode "in-process" with it true is refused (CONFIG_INVALID), located at the field that set it (the
// step's, or the run's for each such step); the caller's override "in-process" turns it off for
// every step instead. The run record's isolation lists both fields of every step.
TEST(IsolationValidation, SharingCompiledModelsNeedsAModuleHost) {
    struct Case {
        std::string name, run, b_extra;
        WorkflowOptions options;
        // step: pointer of the refusal
        std::vector<std::pair<std::string, std::string>> refused;
        nlohmann::json isolation; // when valid
    };
    WorkflowOptions in_process;
    in_process.isolation = IsolationMode::in_process;
    const char* const kRefused = "applies only to a step that runs in a module host";
    const std::vector<Case> cases = {
        {"the step's field",
         "run: { ticks: 1 }\n",
         "    isolation: { share_compiled_models: true }\n",
         {},
         {{"b", "/steps/b/isolation/share_compiled_models"}},
         nullptr},
        {"the run's field",
         "run: { ticks: 1, isolation: { share_compiled_models: true } }\n",
         "    isolation: { mode: process }\n",
         {},
         {{"a", "/run/isolation/share_compiled_models"}},
         nullptr},
        {"both in module hosts",
         "run: { ticks: 1, isolation: { mode: process, share_compiled_models: true } }\n",
         "    isolation: { share_compiled_models: false }\n",
         {},
         {},
         {{"a", {{"mode", "process"}, {"share_compiled_models", true}, {"sandbox", "none"}}},
          {"b", {{"mode", "process"}, {"share_compiled_models", false}, {"sandbox", "none"}}}}},
        {"the override in-process",
         "run: { ticks: 1, isolation: { mode: process, share_compiled_models: true } }\n",
         "",
         in_process,
         {},
         {{"a", {{"mode", "in-process"}, {"share_compiled_models", false}, {"sandbox", "none"}}},
          {"b", {{"mode", "in-process"}, {"share_compiled_models", false}, {"sandbox", "none"}}}}},
        {"false in the runtime's process",
         "run: { ticks: 1 }\n",
         "    isolation: { mode: in-process, share_compiled_models: false }\n",
         {},
         {},
         {{"a", {{"mode", "in-process"}, {"share_compiled_models", false}, {"sandbox", "none"}}},
          {"b", {{"mode", "in-process"}, {"share_compiled_models", false}, {"sandbox", "none"}}}}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        ScratchDir dir;
        const auto validated =
            validate_in(dir.path(), chain("fx-echo", c.run, c.b_extra), c.options);
        std::vector<std::pair<std::string, std::string>> refused;
        for (const auto& error : validated.errors()) {
            EXPECT_EQ(error.code, ErrorCode::CONFIG_INVALID) << error.message;
            EXPECT_NE(error.message.find(kRefused), std::string::npos) << error.message;
            EXPECT_NE(error.message.find("step '" + error.step + "'"), std::string::npos)
                << error.message;
            refused.emplace_back(error.step, error.location.pointer);
        }
        EXPECT_EQ(refused, c.refused) << validated.report.to_text();
        if (c.refused.empty()) {
            ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
            EXPECT_EQ(validated.workflow->isolation, c.isolation);
        } else {
            EXPECT_FALSE(validated.workflow.has_value());
        }
    }
}

// The restricted sandbox profile applies only to a step that runs in a module host: a step that
// resolves to mode "in-process" with it is refused (CONFIG_INVALID), located at the field that set
// it (the step's, or the run's for each such step); the caller's override "in-process" sets every
// step's profile to none instead. Where the platform has no restricted profile (anything but macOS
// in this version), a step in a module host with it is refused (UNSUPPORTED) at that field. The run
// record's isolation lists every step's profile.
TEST(IsolationValidation, TheRestrictedSandboxNeedsAModuleHost) {
    struct Refusal {
        std::string step, pointer;
        ErrorCode code;
    };
    struct Case {
        std::string name, run, b_extra;
        WorkflowOptions options;
        std::vector<Refusal> refused;
        nlohmann::json isolation; // when valid
    };
    WorkflowOptions in_process;
    in_process.isolation = IsolationMode::in_process;
    const auto steps = [](const char* mode, const char* a, const char* b) {
        return nlohmann::json{
            {"a", {{"mode", mode}, {"share_compiled_models", false}, {"sandbox", a}}},
            {"b", {{"mode", mode}, {"share_compiled_models", false}, {"sandbox", b}}}};
    };
    const std::vector<Case> cases = {
        {"the step's field",
         "run: { ticks: 1 }\n",
         "    isolation: { sandbox: restricted }\n",
         {},
         {{"b", "/steps/b/isolation/sandbox", ErrorCode::CONFIG_INVALID}},
         nullptr},
        {"the run's field",
         "run: { ticks: 1, isolation: { sandbox: restricted } }\n",
         "    isolation: { mode: process }\n",
         {},
         {{"a", "/run/isolation/sandbox", ErrorCode::CONFIG_INVALID}
#if !defined(__APPLE__)
          ,
          {"b", "/run/isolation/sandbox", ErrorCode::UNSUPPORTED}
#endif
         },
         nullptr},
        {"both in module hosts",
         "run: { ticks: 1, isolation: { mode: process, sandbox: restricted } }\n",
         "    isolation: { sandbox: none }\n",
         {},
#if defined(__APPLE__)
         {},
         steps("process", "restricted", "none")},
#else
         {{"a", "/run/isolation/sandbox", ErrorCode::UNSUPPORTED}},
         nullptr},
#endif
        {"the override in-process",
         "run: { ticks: 1, isolation: { mode: process, sandbox: restricted } }\n",
         "",
         in_process,
         {},
         steps("in-process", "none", "none")},
        {"none in the runtime's process",
         "run: { ticks: 1 }\n",
         "    isolation: { mode: in-process, sandbox: none }\n",
         {},
         {},
         steps("in-process", "none", "none")},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        ScratchDir dir;
        const auto validated =
            validate_in(dir.path(), chain("fx-echo", c.run, c.b_extra), c.options);
        std::vector<std::pair<std::string, std::string>> refused, expected;
        for (const auto& error : validated.errors()) {
            const auto refusal =
                std::find_if(c.refused.begin(), c.refused.end(), [&error](const Refusal& r) {
                    return r.step == error.step && r.pointer == error.location.pointer;
                });
            ASSERT_NE(refusal, c.refused.end()) << error.message;
            EXPECT_EQ(error.code, refusal->code) << error.message;
            EXPECT_NE(error.message.find(refusal->code == ErrorCode::CONFIG_INVALID
                                             ? "sandbox \"restricted\" applies only to a step "
                                               "that runs in a module host"
                                             : "available only on macOS"),
                      std::string::npos)
                << error.message;
            EXPECT_NE(error.message.find("step '" + error.step + "'"), std::string::npos)
                << error.message;
            refused.emplace_back(error.step, error.location.pointer);
        }
        for (const Refusal& r : c.refused) {
            expected.emplace_back(r.step, r.pointer);
        }
        std::sort(refused.begin(), refused.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(refused, expected) << validated.report.to_text();
        if (c.refused.empty()) {
            ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
            EXPECT_EQ(validated.workflow->isolation, c.isolation);
        } else {
            EXPECT_FALSE(validated.workflow.has_value());
        }
    }
}

// The providers a run loads in the runtime's process serve only its steps in mode "in-process";
// a step in a module host has its own loaded there. converting-module (step a, in a module host)
// uses test-conversion and test-simulation; simulating-module (step b, in the runtime's process)
// uses test-simulation only.
TEST(IsolationValidation, ProvidersAreLoadedWhereTheirStepsRun) {
    static const std::unique_ptr<praenora::detail::RuntimeContext> context =
        praenora::test::make_provider_context({"test-simulation", "test-conversion"});
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{\"value\": 42}");
    const std::string yaml = "kind: workflow-config\n"
                             "format_version: \"0.1.0\"\n"
                             "workflow: { name: test.chain, version: \"0.1.0\" }\n"
                             "run: { ticks: 1 }\n"
                             "inputs:\n"
                             "  source: { source: json-document, path: source.json }\n"
                             "steps:\n"
                             "  a:\n"
                             "    module: { name: converting-module, version: \"0.1.0\" }\n"
                             "    isolation: { mode: process }\n"
                             "  b: { module: { name: simulating-module, version: \"0.1.0\" } }\n";
    const auto validated = praenora::test::validate_text(dir.path(), yaml, {}, *context);
    ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
    const praenora::detail::ValidatedWorkflow& workflow = *validated.workflow;
    // name: [service keys]
    const auto described = [](const std::vector<praenora::detail::ProviderLoad>& loads) {
        std::vector<std::string> text;
        for (const auto& load : loads) {
            std::string keys;
            for (const std::string& key : load.keys) {
                keys += (keys.empty() ? "" : " ") + key;
            }
            text.push_back(load.provider->name + ": " + keys);
        }
        return text;
    };
    EXPECT_EQ(described(praenora::detail::provider_loads(workflow, IsolationMode::in_process)),
              (std::vector<std::string>{"test-simulation: praenora.simulation@0"}));
    EXPECT_EQ(described(praenora::detail::provider_loads(workflow, IsolationMode::process)),
              (std::vector<std::string>{"test-conversion: praenora.model-conversion@0",
                                        "test-simulation: praenora.simulation@0"}));
    ASSERT_EQ(workflow.steps.size(), 2u);
    EXPECT_EQ(workflow.steps[0].id, "a");
    EXPECT_EQ(described(praenora::detail::provider_loads(workflow, workflow.steps[0].services)),
              (std::vector<std::string>{"test-conversion: praenora.model-conversion@0",
                                        "test-simulation: praenora.simulation@0"}));
    // Each load keeps validation's digest of its provider's library.
    for (const auto& load : praenora::detail::provider_loads(workflow, IsolationMode::process)) {
        EXPECT_EQ(load.library_sha256, workflow.services.at(load.keys.front()).library_sha256);
    }
}

#else

// On Windows a step cannot run in a module host in this version.
TEST(IsolationValidation, AModuleHostStepIsUnsupportedOnWindows) {
    ScratchDir dir;
    const auto validated = validate_in(
        dir.path(), chain("fx-echo", "run: { ticks: 1 }\n", "    isolation: { mode: process }\n"));
    EXPECT_FALSE(validated.workflow.has_value());
    const auto errors = validated.errors();
    ASSERT_EQ(errors.size(), 1u) << validated.report.to_text();
    EXPECT_EQ(errors.front().code, ErrorCode::UNSUPPORTED);
    EXPECT_EQ(errors.front().step, "b");
    EXPECT_EQ(errors.front().location.pointer, "/steps/b/isolation/mode");
    EXPECT_TRUE(contains(errors.front().message, "Windows")) << errors.front().message;
}

#endif
