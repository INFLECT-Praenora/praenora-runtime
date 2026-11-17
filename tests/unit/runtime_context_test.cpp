// The runtime context built from the runtime options: its catalog (built-in entries, the caller's
// catalog paths, PRAENORA_CATALOG_PATH), its module index, which keeps the caller's module paths
// first when a configuration adds search paths of its own, and its provider index, which searches
// the caller's provider paths first; and the search paths, with their origins and indexes, that
// the indexes and the validated workflow keep.
#include "api/validation.hpp"
#include "core/paths.hpp"
#include "core/semver.hpp"
#include "discovery/module_index.hpp"
#include "platform/platform.hpp"
#include "provider/provider_index.hpp"

#include "module_fixtures.hpp"
#include "scoped_environment.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using praenora::ErrorCode;
using praenora::RuntimeOptions;
using praenora::detail::make_runtime_context;
using praenora::detail::parse_semver;
using praenora::detail::path_to_utf8;
using praenora::test::ScopedEnvironment;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::validate_text;
using praenora::test::write_text_file;

namespace {

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

// Each search path of `paths` as "<origin>:<index>:<path>" ("<origin>:<path>" without an index).
std::vector<std::string> described(const std::vector<praenora::detail::SearchPath>& paths) {
    std::vector<std::string> texts;
    for (const auto& path : paths) {
        texts.push_back(praenora::detail::to_string(path.origin) + ":" +
                        (path.index ? std::to_string(*path.index) + ":" : std::string()) +
                        path_to_utf8(path.path));
    }
    return texts;
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
              praenora::detail::paths_of(
                  praenora::detail::module_search_paths(options.module_paths, {}, true)));

    EXPECT_EQ(described(context.modules.scanned_search_paths()),
              described(praenora::detail::module_search_paths(options.module_paths, {}, true)));

    // The caller's module is still found when the configuration adds a search path.
    const auto found = validate_text(dir.path(), config_with_search_path("fx-echo"), {}, context);
    ASSERT_TRUE(found.workflow.has_value()) << found.report.to_text();
    EXPECT_EQ(found.workflow->steps.at(0).manifest->manifest_path,
              praenora::test::fixture_manifest("fx-echo"));
    // The validated workflow keeps the search paths that validation used, with their origins and
    // indexes: here the list rebuilt with the configuration's path.
    std::vector<std::string> used = {
        "caller:0:" + path_to_utf8(utf8_path(PRAENORA_TEST_MODULES_DIR).lexically_normal()),
        "configuration:0:" + path_to_utf8(dir.path() / "config mods"),
        "environment:0:" + path_to_utf8(environment_dir)};
    if (const auto installation = praenora::detail::installation_module_dir()) {
        used.push_back("installation:" + path_to_utf8(installation->lexically_normal()));
    }
    EXPECT_EQ(described(found.workflow->module_search_paths), used);
    // Without search paths of the configuration's own, those of the context's index.
    write_text_file(dir.path() / "plain" / "source.json", "{}");
    const auto plain = validate_text(dir.path() / "plain",
                                     "kind: workflow-config\n"
                                     "format_version: \"0.1.0\"\n"
                                     "workflow: { name: test.single, version: \"0.1.0\" }\n"
                                     "run: { ticks: 1 }\n"
                                     "inputs:\n"
                                     "  source: { source: json-document, path: source.json }\n"
                                     "steps:\n"
                                     "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n",
                                     {}, context);
    ASSERT_TRUE(plain.workflow.has_value()) << plain.report.to_text();
    used.erase(used.begin() + 1);
    EXPECT_EQ(described(plain.workflow->module_search_paths), used);

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

TEST(RuntimeContext, ProviderSearchOrderPutsTheCallersPathsFirst) {
    ScratchDir dir;
    const fs::path environment_dir = dir.path() / "environment providers";
    const ScopedEnvironment provider_path("PRAENORA_PROVIDER_PATH", path_to_utf8(environment_dir));
    const ScopedEnvironment module_path("PRAENORA_MODULE_PATH", std::nullopt);
    const ScopedEnvironment catalog_path("PRAENORA_CATALOG_PATH", std::nullopt);

    // The caller's provider paths, then PRAENORA_PROVIDER_PATH, then the installation's directory.
    RuntimeOptions options;
    options.provider_paths = {path_to_utf8(dir.path() / "first"),
                              path_to_utf8(dir.path() / "second")};
    std::vector<fs::path> expected = {(dir.path() / "first").lexically_normal(),
                                      (dir.path() / "second").lexically_normal(),
                                      environment_dir.lexically_normal()};
    if (const auto installation = praenora::detail::installation_provider_dir()) {
        expected.push_back(installation->lexically_normal());
    }
    const auto context = make_runtime_context(options);
    EXPECT_EQ(context.providers.search_paths(), expected);
    EXPECT_EQ(context.providers.search_paths(),
              praenora::detail::paths_of(
                  praenora::detail::provider_search_paths(options.provider_paths, true)));
    // With their origins and indexes, as the run's anchors name them.
    std::vector<std::string> scanned = {"caller:0:" + path_to_utf8(expected[0]),
                                        "caller:1:" + path_to_utf8(expected[1]),
                                        "environment:0:" + path_to_utf8(expected[2])};
    if (expected.size() > 3) {
        scanned.push_back("installation:" + path_to_utf8(expected[3]));
    }
    EXPECT_EQ(described(context.providers.scanned_search_paths()), scanned);

    // Without the environment the caller's paths stay first, and PRAENORA_PROVIDER_PATH is left
    // out.
    options.use_environment = false;
    expected.erase(expected.begin() + 2);
    EXPECT_EQ(make_runtime_context(options).providers.search_paths(), expected);
}
