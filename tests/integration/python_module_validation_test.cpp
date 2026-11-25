// A Python module in a tree without the provider python, through the public API only (the shared
// library): every Python module requires praenora.python@0.0, so validation refuses its step with
// SERVICE_UNAVAILABLE at the first required service of its manifest. Compiled into the integration
// tests only where PRAENORA_WITH_PYTHON_ENVIRONMENT is off: with it, the installation's provider
// directory, which the runtime always searches, holds the provider python. The Python fixture
// modules build in every tree.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/workflow.hpp>

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::ErrorCode;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::ValidationReport;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

namespace {

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

// fx-py-echo, with an empty provider search path and the environment ignored: no provider offers
// praenora.python@0, and validation says so at /requires/services/0 of the module's manifest.
TEST(PythonModuleValidation, WithoutTheProviderPythonIsServiceUnavailable) {
    ScratchDir dir;
    fs::create_directories(dir.path() / "no-providers");
    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_PYTHON_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.provider_paths = {utf8(dir.path() / "no-providers")};
    options.use_environment = false;
    options.logging.to_stderr = false;
    const Runtime runtime(options);
    write_text_file(dir.path() / "source.json", "{\"value\": 42}");
    write_text_file(dir.path() / "workflow.yaml",
                    "kind: workflow-config\n"
                    "format_version: \"0.1.0\"\n"
                    "workflow: { name: test.single, version: \"0.1.0\" }\n"
                    "run: { ticks: 1 }\n"
                    "inputs:\n"
                    "  source: { source: json-document, path: source.json }\n"
                    "steps:\n"
                    "  a:\n"
                    "    module: { name: fx-py-echo, version: \"0.1.0\" }\n");
    const ValidationReport report = runtime.validate(utf8(dir.path() / "workflow.yaml"));
    ASSERT_FALSE(report.ok()) << report.to_text();
    const praenora::Diagnostic& error = *report.first_error();
    EXPECT_EQ(error.code, ErrorCode::SERVICE_UNAVAILABLE) << report.to_text();
    EXPECT_EQ(error.location.pointer, "/requires/services/0");
    EXPECT_EQ(error.step, "a");
    const json details = json::parse(error.details_json);
    EXPECT_EQ(details.at("service"), "praenora.python");
    EXPECT_EQ(details.at("major"), 0);
    EXPECT_EQ(details.at("minor"), 0);
    EXPECT_EQ(details.at("providers"), json::array());
}
