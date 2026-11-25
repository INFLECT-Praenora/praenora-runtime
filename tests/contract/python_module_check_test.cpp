// The contract checks of `praenora modules check` (Runtime::check_module()) on Python modules,
// through the public API only (the shared library), with the Python fixture modules
// (tests/fixtures/modules/python) and the build tree's provider python, which the runtime finds
// without a provider path: a module that passes them with its test vectors, the self test that
// passes, the one that fails, the one the manifest declares and the class lacks, and the module
// without one. The checks run the modules in the test's own process. Built with
// PRAENORA_WITH_PYTHON_ENVIRONMENT only.
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using praenora::CheckResult;
using praenora::ModuleCheckReport;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::test::utf8_path;

namespace {

// Every check id, in the order the report lists them.
const std::vector<std::string> kCheckIds = {
    "manifest.schema",
    "manifest.semantics",
    "library.digest",
    "library.load",
    "library.entry_point",
    "api.header",
    "api.required_functions",
    "describe.schema",
    "describe.identity",
    "lifecycle.create_destroy",
    "lifecycle.full",
    "invoke.result_schema",
    "invoke.expected_outputs",
    "invoke.determinism",
    "invoke.malformed_input",
    "self_test",
    "unload",
};

// The test catalog (test.echo), without the environment and without logging to standard error.
RuntimeOptions check_options() {
    RuntimeOptions options;
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    return options;
}

std::string manifest_of(const std::string& name) {
    const fs::path manifest = utf8_path(PRAENORA_TEST_PYTHON_MODULES_DIR) / utf8_path(name) /
                              utf8_path(name + ".pmm.json");
    const auto text = manifest.u8string();
    return std::string(text.begin(), text.end());
}

std::vector<std::string> ids_of(const ModuleCheckReport& report) {
    std::vector<std::string> ids;
    for (const CheckResult& check : report.checks) {
        ids.push_back(check.id);
    }
    return ids;
}

const CheckResult& check_of(const ModuleCheckReport& report, const std::string& id) {
    for (const CheckResult& check : report.checks) {
        if (check.id == id) {
            return check;
        }
    }
    throw std::runtime_error("the report has no check " + id);
}

// The report, one "<status> <id>: <message>" line per check, for failure messages.
std::string shown(const ModuleCheckReport& report) {
    std::string text;
    for (const CheckResult& check : report.checks) {
        text += check.status + " " + check.id + ": " + check.message + "\n";
    }
    return text;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The status of the unload check: skipped where the environment keeps every module library loaded
// (the sanitizer test runs on macOS do).
std::string expected_unload_status() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* value = std::getenv("PRAENORA_DEBUG_KEEP_LIBRARIES");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && std::string(value) == "1" ? "skip" : "pass";
}

// Every check but those of `others` passes; those have the status they name.
void expect_statuses(const ModuleCheckReport& report,
                     const std::vector<std::pair<std::string, std::string>>& others) {
    EXPECT_EQ(ids_of(report), kCheckIds);
    for (const CheckResult& check : report.checks) {
        std::string expected = check.id == "unload" ? expected_unload_status() : "pass";
        for (const auto& [id, status] : others) {
            if (id == check.id) {
                expected = status;
            }
        }
        EXPECT_EQ(check.status, expected) << check.id << "\n" << shown(report);
    }
}

} // namespace

// fx-py-echo, with the test vectors its manifest names, passes every check; library.load names the
// provider of praenora.python@0, and the module provides no self test.
TEST(ModuleCheck, PythonModulePasses) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(manifest_of("fx-py-echo"));
    EXPECT_TRUE(report.passed()) << shown(report);
    expect_statuses(report, {{"self_test", "skip"}});
    const std::string& load = check_of(report, "library.load").message;
    EXPECT_TRUE(contains(load, "praenora.python@0 from provider python@")) << load;
    EXPECT_TRUE(contains(check_of(report, "invoke.expected_outputs").message, "3 tick(s)"))
        << shown(report);
}

// The class method self_test of fx-py-self-test passes: so does the check.
TEST(ModuleCheck, PythonSelfTestPasses) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(manifest_of("fx-py-self-test"));
    EXPECT_TRUE(report.passed()) << shown(report);
    const CheckResult& self_test = check_of(report, "self_test");
    EXPECT_EQ(self_test.status, "pass") << shown(report);
    EXPECT_TRUE(contains(self_test.message, "self_test() passed (1 checks)")) << self_test.message;
}

// fx-py-self-test-fail's self test reports a failing check: the self_test check fails, naming it.
TEST(ModuleCheck, PythonSelfTestFails) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(manifest_of("fx-py-self-test-fail"));
    EXPECT_FALSE(report.passed());
    const CheckResult& self_test = check_of(report, "self_test");
    EXPECT_EQ(self_test.status, "fail") << shown(report);
    EXPECT_TRUE(contains(self_test.message, "tables: wrong value")) << self_test.message;
}

// fx-py-self-test-missing is built with SELF_TEST and its manifest declares it, but its class
// defines no self_test: the self_test check fails with the class-only object's UNSUPPORTED, and
// only it.
TEST(ModuleCheck, PythonSelfTestMissingFails) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(manifest_of("fx-py-self-test-missing"));
    EXPECT_FALSE(report.passed());
    const CheckResult& self_test = check_of(report, "self_test");
    EXPECT_EQ(self_test.status, "fail") << shown(report);
    EXPECT_TRUE(contains(self_test.message, "self_test")) << self_test.message;
    EXPECT_TRUE(contains(self_test.message, "UNSUPPORTED")) << self_test.message;
    // Without test vectors the three checks of them are skipped; every other check passes.
    expect_statuses(report, {{"self_test", "fail"},
                             {"invoke.result_schema", "skip"},
                             {"invoke.expected_outputs", "skip"},
                             {"invoke.determinism", "skip"}});
}

// A Python module built without SELF_TEST has no self_test in its table: the check is skipped.
TEST(ModuleCheck, PythonWithoutSelfTestSkipsIt) {
    const Runtime runtime(check_options());
    const ModuleCheckReport report = runtime.check_module(manifest_of("fx-py-numpy"));
    const CheckResult& self_test = check_of(report, "self_test");
    EXPECT_EQ(self_test.status, "skip") << shown(report);
    EXPECT_TRUE(contains(self_test.message, "self_test is NULL")) << self_test.message;
    EXPECT_TRUE(report.passed()) << shown(report);
}
