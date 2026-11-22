// Helpers for tests of the validation sequence (api/validation.hpp): a runtime context over the
// test catalog and the fixture modules, and configurations written to scratch directories.
// Header-only; white-box (it uses the runtime's internal headers).
#ifndef PRAENORA_TESTS_SUPPORT_VALIDATION_FIXTURES_HPP
#define PRAENORA_TESTS_SUPPORT_VALIDATION_FIXTURES_HPP

#include "api/validation.hpp"
#include "catalog/catalog.hpp"
#include "discovery/module_index.hpp"
#include "provider/provider_index.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace praenora::test {

// The provider search paths of `provider_paths` (UTF-8) without the environment, and without the
// installation's directory: a build tree builds its providers (roadrunner, antimony) where its
// runtime finds those of an installation, and on Windows a white-box test executable, which holds
// the runtime's objects, sits beside the runtime DLL and so would find them too. A white-box test
// sees only the providers it names.
inline std::vector<detail::SearchPath>
test_provider_search_paths(const std::vector<std::string>& provider_paths) {
    std::vector<detail::SearchPath> paths = detail::provider_search_paths(provider_paths, false);
    paths.erase(std::remove_if(paths.begin(), paths.end(),
                               [](const detail::SearchPath& path) {
                                   return path.origin == detail::SearchPathOrigin::installation;
                               }),
                paths.end());
    return paths;
}

// A runtime context whose catalog is the built-in one plus tests/fixtures/catalog (and
// `extra_catalogs`), whose module search path is `module_paths` (by default the fixture modules)
// and whose provider search path is `provider_paths` (UTF-8; by default none), without the
// environment. The installation's module directory is searched last, as always; its provider
// directory is not searched (test_provider_search_paths()).
inline std::unique_ptr<detail::RuntimeContext>
make_test_context(std::vector<std::string> module_paths = {PRAENORA_TEST_MODULES_DIR},
                  std::vector<std::filesystem::path> extra_catalogs = {},
                  const std::vector<std::string>& provider_paths = {}) {
    std::vector<std::filesystem::path> catalogs = {utf8_path(PRAENORA_TEST_CATALOG_DIR)};
    catalogs.insert(catalogs.end(), extra_catalogs.begin(), extra_catalogs.end());
    auto context = std::make_unique<detail::RuntimeContext>();
    context->catalog = detail::Catalog::load(catalogs);
    context->modules =
        detail::ModuleIndex::scan(detail::module_search_paths(module_paths, {}, false));
    context->providers = detail::ProviderIndex::scan(test_provider_search_paths(provider_paths));
    context->module_paths = std::move(module_paths);
    context->use_environment = false;
    return context;
}

// A runtime context over the fixture modules whose provider search path is the directories of
// the fixture providers `providers` (provider_fixtures.hpp), in that order.
inline std::unique_ptr<detail::RuntimeContext>
make_provider_context(const std::vector<std::string>& providers) {
    std::vector<std::string> paths;
    for (const std::string& name : providers) {
        paths.push_back(std::string(PRAENORA_TEST_PROVIDERS_DIR) + "/" + name);
    }
    return make_test_context({PRAENORA_TEST_MODULES_DIR}, {}, paths);
}

// The shared context over the fixture modules.
inline const detail::RuntimeContext& test_context() {
    static const std::unique_ptr<detail::RuntimeContext> context = make_test_context();
    return *context;
}

// The result of validating one configuration.
struct Validated {
    std::optional<detail::ValidatedWorkflow> workflow;
    ValidationReport report;

    // The error diagnostics, in order.
    std::vector<Diagnostic> errors() const {
        std::vector<Diagnostic> result;
        std::copy_if(report.diagnostics.begin(), report.diagnostics.end(),
                     std::back_inserter(result),
                     [](const Diagnostic& d) { return d.severity == Severity::error; });
        return result;
    }
};

// Writes `yaml` to `<dir>/workflow.yaml` and validates it.
inline Validated validate_text(const std::filesystem::path& dir, const std::string& yaml,
                               const WorkflowOptions& options = {},
                               const detail::RuntimeContext& context = test_context()) {
    const std::filesystem::path file = dir / "workflow.yaml";
    write_text_file(file, yaml);
    Validated result;
    result.workflow = detail::validate_workflow(file, context, options, result.report);
    return result;
}

// A long-form observation CSV with two series over the first three ticks of a unit time base.
inline const char* observations_csv() {
    return "series_id,variable_id,time,value,unit\n"
           "S1,x,0.0,10,count\n"
           "S2,x,0.5,20,count\n"
           "S1,x,1.25,11,count\n"
           "S2,x,2.5,21,count\n";
}

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_VALIDATION_FIXTURES_HPP
