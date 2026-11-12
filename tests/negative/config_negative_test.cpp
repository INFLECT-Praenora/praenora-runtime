// Malformed workflow configurations (tests/fixtures/configs/bad/*.yaml): loading each one reports
// an error whose code, line and JSON pointer are given by its sibling <name>.expect.json
// ({"code": "<ERROR NAME>", "line": <line>, "pointer": "<json pointer>"}), located in the file.
// The first error of the report is compared; each fixture has exactly one problem.
#include "config/workflow_config.hpp"
#include "document/yaml_loader.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using praenora::Diagnostic;
using praenora::Severity;
using praenora::ValidationReport;
using praenora::detail::load_workflow_config;

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

fs::path bad_dir() { return praenora::test::fixture_path("configs/bad"); }

// The file names of the bad configurations, sorted.
std::vector<std::string> bad_configs() {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(bad_dir())) {
        const std::string name = entry.path().filename().u8string();
        if (ends_with(name, ".yaml")) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// "duplicate-step-key.yaml" -> "duplicate_step_key"
std::string test_name(const std::string& file_name) {
    std::string name = file_name.substr(0, file_name.size() - std::string(".yaml").size());
    std::replace(name.begin(), name.end(), '-', '_');
    return name;
}

} // namespace

// The test suite is named Config, like the unit tests of configuration loading, so CTest selects
// them together (-R '^Config').
class Config : public ::testing::TestWithParam<std::string> {};

TEST_P(Config, BadFixtures) {
    const fs::path file = bad_dir() / praenora::test::utf8_path(GetParam());
    fs::path expect_path = file;
    expect_path.replace_extension(".expect.json");
    ASSERT_TRUE(fs::exists(expect_path)) << expect_path.u8string();
    const nlohmann::json expect = praenora::detail::load_document_file(expect_path).json;
    const auto code = praenora::error_code_from_name(expect.at("code").get<std::string>());
    ASSERT_TRUE(code.has_value()) << expect.dump();

    ValidationReport report;
    (void)load_workflow_config(file, report);
    ASSERT_FALSE(report.ok()) << "the configuration was accepted";
    const auto first =
        std::find_if(report.diagnostics.begin(), report.diagnostics.end(),
                     [](const Diagnostic& d) { return d.severity == Severity::error; });
    ASSERT_NE(first, report.diagnostics.end());
    EXPECT_EQ(first->code, *code) << report.to_text();
    EXPECT_EQ(first->location.line, expect.at("line").get<std::int64_t>()) << report.to_text();
    EXPECT_GT(first->location.column, 0) << report.to_text();
    EXPECT_EQ(first->location.pointer, expect.at("pointer").get<std::string>()) << report.to_text();
    EXPECT_TRUE(ends_with(first->location.file, GetParam())) << report.to_text();
    EXPECT_FALSE(first->message.empty());
    // The compiler-style text names the file, the line and the error's name.
    EXPECT_NE(report.to_text().find(GetParam() + ":" + std::to_string(first->location.line) + ":"),
              std::string::npos)
        << report.to_text();
}

INSTANTIATE_TEST_SUITE_P(, Config, ::testing::ValuesIn(bad_configs()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             return test_name(info.param);
                         });

TEST(ConfigBadFixtures, CoverTheRequiredCases) {
    const auto names = bad_configs();
    for (const char* required :
         {"yaml-syntax-error.yaml", "duplicate-step-key.yaml", "unknown-top-level-key.yaml",
          "bad-duration-unit.yaml", "duration-not-whole-nanoseconds.yaml", "seed-too-large.yaml",
          "missing-workflow-version.yaml", "json-document-without-path.yaml",
          "missing-ticks.yaml"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), required), names.end()) << required;
    }
}
