// The runtime context built from the runtime options: its catalog (built-in entries, the caller's
// catalog paths, PRAENORA_CATALOG_PATH) and its module index, which keeps the caller's module
// paths first when a configuration adds search paths of its own.
#include "api/validation.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "discovery/module_index.hpp"
#include "platform/platform.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using praenora::ErrorCode;
using praenora::RuntimeOptions;
using praenora::detail::make_runtime_context;
using praenora::detail::parse_semver;
using praenora::detail::path_to_utf8;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::validate_text;
using praenora::test::write_text_file;

namespace {

// Sets an environment variable (or removes it, for nullopt) and restores the old value when
// destroyed.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::optional<std::string>& value)
        : name_(name), previous_(praenora::detail::environment_variable(name)) {
        set(value);
    }
    ~ScopedEnvironment() { set(previous_); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    void set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        const fs::path name = utf8_path(name_);
        SetEnvironmentVariableW(name.c_str(), value ? utf8_path(*value).c_str() : nullptr);
#else
        if (value) {
            setenv(name_, value->c_str(), 1);
        } else {
            unsetenv(name_);
        }
#endif
    }

    const char* name_;
    std::optional<std::string> previous_;
};

std::string path_list(const std::vector<std::string>& entries) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string list;
    for (const auto& entry : entries) {
        list += (list.empty() ? "" : std::string(1, separator)) + entry;
    }
    return list;
}

// A configuration of test.single whose step `a` uses `module`, with its own module search path
// "config mods".
std::string config_with_search_path(const std::string& module) {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: test.single, version: \"0.1.0\" }\n"
           "run: { ticks: 1 }\n"
           "inputs:\n"
           "  source: { source: json-document, path: source.json }\n"
           "modules: { search_paths: [config mods] }\n"
           "steps:\n"
           "  a: { module: { name: " +
           module + ", version: \"0.1.0\" } }\n";
}

} // namespace

TEST(RuntimeContext, ModuleSearchOrderKeepsTheCallersPaths) {
    ScratchDir dir;
    const fs::path environment_dir = dir.path() / "environment mods";
    fs::create_directories(environment_dir);
    fs::create_directories(dir.path() / "config mods");
    write_text_file(dir.path() / "source.json", "{}");
    const ScopedEnvironment module_path("PRAENORA_MODULE_PATH", path_to_utf8(environment_dir));
    const ScopedEnvironment catalog_path("PRAENORA_CATALOG_PATH", std::nullopt);

    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    ASSERT_TRUE(options.use_environment);
    const auto context = make_runtime_context(options);
    EXPECT_EQ(context.module_paths, options.module_paths);
    EXPECT_TRUE(context.use_environment);
    EXPECT_EQ(context.modules.search_paths(),
              praenora::detail::module_search_paths(options.module_paths, {}, true));

    // The caller's module is still found when the configuration adds a search path.
    const auto found = validate_text(dir.path(), config_with_search_path("fx-echo"), {}, context);
    ASSERT_TRUE(found.workflow.has_value()) << found.report.to_text();
    EXPECT_EQ(found.workflow->steps.at(0).manifest->manifest_path,
              praenora::test::fixture_manifest("fx-echo"));

    // The search order: the caller's paths, the configuration's, PRAENORA_MODULE_PATH, then the
    // installation's directory.
    const auto missing =
        validate_text(dir.path(), config_with_search_path("fx-nowhere"), {}, context);
    const auto errors = missing.errors();
    ASSERT_EQ(errors.size(), 1u) << missing.report.to_text();
    EXPECT_EQ(errors[0].code, ErrorCode::MODULE_NOT_FOUND);
    std::vector<std::string> expected = {path_to_utf8(utf8_path(PRAENORA_TEST_MODULES_DIR)),
                                         path_to_utf8(dir.path() / "config mods"),
                                         path_to_utf8(environment_dir)};
    if (const auto installation = praenora::detail::installation_module_dir()) {
        expected.push_back(path_to_utf8(installation->lexically_normal()));
    }
    std::size_t previous = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto at = errors[0].message.find("'" + expected[i] + "'");
        ASSERT_NE(at, std::string::npos) << expected[i] << " is not in: " << errors[0].message;
        if (i > 0) {
            EXPECT_GT(at, previous) << expected[i] << " comes too early: " << errors[0].message;
        }
        previous = at;
    }
}

TEST(RuntimeContext, CatalogPathsComeFromOptionsAndEnvironment) {
    const auto version = parse_semver("0.1.0");
    const std::string test_catalog = PRAENORA_TEST_CATALOG_DIR;
    const ScopedEnvironment module_path("PRAENORA_MODULE_PATH", std::nullopt);
    {
        const ScopedEnvironment catalog_path("PRAENORA_CATALOG_PATH", test_catalog);
        RuntimeOptions options;
        const auto with_environment = make_runtime_context(options);
        EXPECT_NE(with_environment.catalog.find_workflow("test.single", version), nullptr);
        options.use_environment = false;
        const auto without_environment = make_runtime_context(options);
        EXPECT_EQ(without_environment.catalog.find_workflow("test.single", version), nullptr);
        EXPECT_FALSE(without_environment.use_environment);
        // A directory given both ways is loaded once (twice would be duplicate entries).
        options.use_environment = true;
        options.catalog_paths = {test_catalog};
        const auto both = make_runtime_context(options);
        EXPECT_NE(both.catalog.find_workflow("test.single", version), nullptr);
    }
    // Entries of PRAENORA_CATALOG_PATH that are not directories are skipped, like those of
    // PRAENORA_MODULE_PATH; a catalog path the caller names must exist.
    ScratchDir dir;
    const std::string absent = path_to_utf8(dir.path() / "absent");
    const ScopedEnvironment catalog_path("PRAENORA_CATALOG_PATH",
                                         path_list({absent, test_catalog}));
    RuntimeOptions options;
    const auto skipped = make_runtime_context(options);
    EXPECT_NE(skipped.catalog.find_workflow("test.single", version), nullptr);
    options.catalog_paths = {absent};
    try {
        (void)make_runtime_context(options);
        ADD_FAILURE() << "a missing catalog path was accepted";
    } catch (const praenora::Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::IO_ERROR) << e.what();
    }
}
