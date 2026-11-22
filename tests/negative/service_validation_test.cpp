// Runtime services that a run cannot get: validation refuses a required service that no provider
// offers (SERVICE_UNAVAILABLE: none at all, only a lower minor, or only an unusable provider), and
// checks the library file of each provider the run uses without loading it; a provider that does
// not load fails the run before any module is loaded, with the provider loader's error. The
// modules are the fixture modules built from fx-service (tests/fixtures/modules), the providers the
// fixture providers (tests/fixtures/providers) or copies of them in scratch directories.
#include "api/validation.hpp"
#include "engine/engine.hpp"
#include "observability/metrics.hpp"
#include "provider/provider_loader.hpp"

#include "instance_fixtures.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Diagnostic;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Severity;
using praenora::StepState;
using praenora::WorkflowState;
using praenora::detail::Engine;
using praenora::detail::EngineSettings;
using praenora::detail::MetricsRegistry;
using praenora::detail::RuntimeContext;
using praenora::test::CapturedLogs;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// test.single (inputs.source -> a -> outputs.result), three ticks, with step a bound to fixture
// module `module`, recording into out/.
std::string single(const std::string& module, const std::string& parameters = "{}") {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: test.single, version: \"0.1.0\" }\n"
           "run: { ticks: 3, seed: 7 }\n"
           "inputs:\n"
           "  source: { source: json-document, path: source.json }\n"
           "steps:\n"
           "  a:\n"
           "    module: { name: " +
           module + ", version: \"0.1.0\" }\n    parameters: " + parameters +
           "\n"
           "outputs: { directory: out }\n";
}

// Validates `yaml` in `dir` (with its source.json) against `context`.
praenora::test::Validated validate_in(const fs::path& dir, const std::string& yaml,
                                      const RuntimeContext& context) {
    write_text_file(dir / "source.json", "{\"value\": 42}");
    return praenora::test::validate_text(dir, yaml, {}, context);
}

// The one error of `validated`; fails the test unless there is exactly one.
Diagnostic only_error(const praenora::test::Validated& validated) {
    const auto errors = validated.errors();
    EXPECT_EQ(errors.size(), 1u) << validated.report.to_text();
    if (errors.empty()) {
        throw std::runtime_error("no error");
    }
    return errors.front();
}

// A context over the fixture modules whose provider search path is `paths` (UTF-8).
std::unique_ptr<RuntimeContext> context_with(const std::vector<fs::path>& paths) {
    std::vector<std::string> texts;
    for (const fs::path& path : paths) {
        texts.push_back(utf8(path));
    }
    return praenora::test::make_test_context({PRAENORA_TEST_MODULES_DIR}, {}, texts);
}

} // namespace

TEST(ServiceValidation, RequiredServiceWithoutAProviderIsUnavailable) {
    ScratchDir dir;
    const auto context = context_with({dir.path() / "no-providers"});
    const auto validated = validate_in(dir.path(), single("fx-service"), *context);
    EXPECT_FALSE(validated.workflow.has_value());
    const Diagnostic error = only_error(validated);
    EXPECT_EQ(error.code, ErrorCode::SERVICE_UNAVAILABLE);
    EXPECT_EQ(error.step, "a");
    EXPECT_EQ(error.module, "fx-service@0.1.0");
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(error.location.file),
                               praenora::test::fixture_manifest("fx-service")))
        << error.location.file;
    EXPECT_EQ(error.location.pointer, "/requires/services/0");
    EXPECT_TRUE(contains(error.message, "test.echo@0.0")) << error.message;
    EXPECT_TRUE(contains(error.message, "no-providers")) << error.message;

    const json details = json::parse(error.details_json);
    EXPECT_EQ(details.at("service"), "test.echo");
    EXPECT_EQ(details.at("major"), 0);
    EXPECT_EQ(details.at("minor"), 0);
    ASSERT_FALSE(details.at("search_paths").empty());
    EXPECT_TRUE(contains(details.at("search_paths").at(0).get<std::string>(), "no-providers"));
    EXPECT_EQ(details.at("providers"), json::array());
    // The report's JSON carries the details.
    const json report = json::parse(validated.report.to_json());
    EXPECT_EQ(report.at("diagnostics").at(0).at("details"), details);
}

TEST(ServiceValidation, ProviderOfALowerMinorIsNamed) {
    ScratchDir dir;
    const auto context = praenora::test::make_provider_context({"test-echo"});
    const Diagnostic error =
        only_error(validate_in(dir.path(), single("fx-service-minor"), *context));
    EXPECT_EQ(error.code, ErrorCode::SERVICE_UNAVAILABLE);
    EXPECT_EQ(error.location.pointer, "/requires/services/0");
    EXPECT_TRUE(contains(error.message, "test.echo@0.7")) << error.message;
    EXPECT_TRUE(contains(error.message, "provider test-echo@0.1.0")) << error.message;

    const json details = json::parse(error.details_json);
    EXPECT_EQ(details.at("minor"), 7);
    ASSERT_EQ(details.at("providers").size(), 1u) << details.dump();
    const json& provider = details.at("providers").at(0);
    EXPECT_EQ(provider.at("name"), "test-echo");
    EXPECT_EQ(provider.at("version"), "0.1.0");
    const std::string listed = provider.at("manifest").get<std::string>();
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(listed),
                               praenora::test::fixture_provider_manifest("test-echo")));
    EXPECT_TRUE(contains(provider.at("reason").get<std::string>(), "offers test.echo@0.0"))
        << provider.dump();
}

TEST(ServiceValidation, UnusableProviderIsNamedWithItsReason) {
    ScratchDir dir;
    // Built for another runtime version, and for another provider interface major.
    praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers" / "old",
                                          [](json& manifest) {
                                              manifest["runtime_version"] = "0.1.0";
                                          });
    praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers" / "next",
                                          [](json& manifest) {
                                              manifest["provider"]["name"] = "test-echo-next";
                                              manifest["abi"]["major"] = 1;
                                          });
    const auto context = context_with({dir.path() / "providers"});
    const Diagnostic error = only_error(validate_in(dir.path(), single("fx-service"), *context));
    EXPECT_EQ(error.code, ErrorCode::SERVICE_UNAVAILABLE);
    const json providers = json::parse(error.details_json).at("providers");
    ASSERT_EQ(providers.size(), 2u) << providers.dump();
    // In search order: next/ before old/.
    EXPECT_EQ(providers.at(0).at("name"), "test-echo-next");
    const auto reason = [&providers](std::size_t i) {
        return providers.at(i).at("reason").get<std::string>();
    };
    EXPECT_TRUE(contains(reason(0), "provider interface 1.0")) << providers;
    EXPECT_EQ(providers.at(1).at("name"), "test-echo");
    EXPECT_TRUE(contains(reason(1), "built for runtime 0.1.0")) << providers;
    EXPECT_TRUE(contains(error.message, "built for runtime 0.1.0")) << error.message;
}

TEST(ServiceValidation, OptionalServiceThatDoesNotResolveIsNotUsed) {
    ScratchDir dir;
    const auto context = context_with({dir.path() / "no-providers"});
    const auto validated = validate_in(dir.path(), single("fx-service-optional"), *context);
    ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
    EXPECT_TRUE(validated.report.diagnostics.empty()) << validated.report.to_text();
    EXPECT_TRUE(validated.workflow->services.empty());
    EXPECT_TRUE(validated.workflow->steps.at(0).services.empty());
    const json& section = validated.workflow->reproducibility;
    EXPECT_EQ(section.at("services"), json::object());
    EXPECT_EQ(section.at("steps").at("a").at("services"), json::array());
}

TEST(ServiceValidation, PinnedProviderDigestIsChecked) {
    ScratchDir dir;
    const fs::path manifest = praenora::test::copy_fixture_provider(
        "test-echo", dir.path() / "providers", [](json& document) {
            document["library"]["sha256"] = std::string(64, '0');
        });
    const auto context = context_with({dir.path() / "providers"});
    const auto validated = validate_in(dir.path(), single("fx-service"), *context);
    EXPECT_FALSE(validated.workflow.has_value());
    const Diagnostic error = only_error(validated);
    EXPECT_EQ(error.code, ErrorCode::LIBRARY_DIGEST_MISMATCH);
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(error.location.file), manifest));
    EXPECT_EQ(error.location.pointer, "/library/sha256");
    EXPECT_EQ(error.step, "a");
    EXPECT_EQ(error.module, "fx-service@0.1.0");
    EXPECT_TRUE(contains(error.message, "provider test-echo@0.1.0: ")) << error.message;
}

TEST(ServiceValidation, MissingProviderLibraryIsNotFound) {
    ScratchDir dir;
    const fs::path manifest =
        praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers");
    fs::remove(praenora::detail::load_provider_manifest(manifest).library_path);
    const auto context = context_with({dir.path() / "providers"});
    const Diagnostic error = only_error(validate_in(dir.path(), single("fx-service"), *context));
    EXPECT_EQ(error.code, ErrorCode::LIBRARY_NOT_FOUND);
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(error.location.file), manifest));
    EXPECT_EQ(error.location.pointer, "/library/path");
    EXPECT_TRUE(contains(error.message, "provider test-echo@0.1.0: the provider library"))
        << error.message;
}

TEST(ServiceValidation, ProviderScanWarningsAreReported) {
    ScratchDir dir;
    praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers");
    write_text_file(dir.path() / "providers" / "broken.provider.json", "{ not json");
    const auto context = context_with({dir.path() / "providers"});
    const auto validated = validate_in(dir.path(), single("fx-service"), *context);
    ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
    ASSERT_EQ(validated.report.diagnostics.size(), 1u) << validated.report.to_text();
    const Diagnostic& warning = validated.report.diagnostics.front();
    EXPECT_EQ(warning.severity, Severity::warning);
    EXPECT_EQ(warning.code, ErrorCode::PARSE_ERROR);
    EXPECT_TRUE(contains(warning.location.file, "broken.provider.json")) << warning.location.file;
}

namespace {

// A configuration of `workflow` over source.json, with the steps of `steps` (pairs of step id and
// fixture module).
std::string workflow_of(const std::string& workflow,
                        const std::vector<std::pair<std::string, std::string>>& steps) {
    std::string yaml = "kind: workflow-config\n"
                       "format_version: \"0.1.0\"\n"
                       "workflow: { name: " +
                       workflow +
                       ", version: \"0.1.0\" }\n"
                       "run: { ticks: 1, seed: 7 }\n"
                       "inputs:\n"
                       "  source: { source: json-document, path: source.json }\n"
                       "steps:\n";
    for (const auto& [id, module] : steps) {
        yaml += "  " + id + ": { module: { name: " + module + ", version: \"0.1.0\" } }\n";
    }
    return yaml;
}

// test.reverse in a catalog directory of its own below `dir`: test.chain's steps a and b with the
// connection reversed (inputs.source -> b -> a -> outputs.result), so its schedule is b, a.
fs::path reverse_catalog(const fs::path& dir) {
    const fs::path catalog = dir / "catalog";
    write_text_file(catalog / "workflows" / "test.reverse" / "0.1.0.json", R"({
  "kind": "workflow-definition",
  "format_version": "0.1.0",
  "name": "test.reverse",
  "version": "0.1.0",
  "status": "stable",
  "summary": "Two echo steps in a chain whose ids are in the opposite order (tests).",
  "inputs": { "source": { "type": "test.blob", "type_version": "0.1.0" } },
  "steps": {
    "a": { "capability": "test.echo", "capability_version": "0.1.0" },
    "b": { "capability": "test.echo", "capability_version": "0.1.0" }
  },
  "connections": [
    { "from": "inputs.source", "to": "b.in" },
    { "from": "b.out", "to": "a.in" },
    { "from": "a.out", "to": "outputs.result" }
  ],
  "outputs": { "result": { "type": "test.blob", "type_version": "0.1.0" } }
}
)");
    return catalog;
}

} // namespace

// A provider's library problem is reported once, for the first step in schedule order that uses
// the provider: not for the step whose requirement chose it, nor the first in step-id order.
TEST(ServiceValidation, ProviderLibraryProblemIsReportedForTheFirstStepInSchedule) {
    ScratchDir dir;
    // A provider of test.echo@0.7, which serves fx-service (0.0) and fx-service-minor (0.7), so
    // fx-service-minor's requirement chooses it; its library is gone.
    const fs::path manifest = praenora::test::copy_fixture_provider(
        "test-echo", dir.path() / "providers", [](json& document) {
            document["services"] =
                json::array({{{"name", "test.echo"}, {"major", 0}, {"minor", 7}}});
        });
    fs::remove(praenora::detail::load_provider_manifest(manifest).library_path);
    const std::vector<std::string> providers = {utf8(dir.path() / "providers")};

    // test.chain runs a, then b.
    const auto chain =
        praenora::test::make_test_context({PRAENORA_TEST_MODULES_DIR}, {}, providers);
    const Diagnostic in_chain = only_error(validate_in(
        dir.path(), workflow_of("test.chain", {{"a", "fx-service"}, {"b", "fx-service-minor"}}),
        *chain));
    EXPECT_EQ(in_chain.code, ErrorCode::LIBRARY_NOT_FOUND);
    EXPECT_EQ(in_chain.step, "a");
    EXPECT_EQ(in_chain.module, "fx-service@0.1.0");

    // test.reverse runs b, then a.
    const auto reverse = praenora::test::make_test_context(
        {PRAENORA_TEST_MODULES_DIR}, {reverse_catalog(dir.path())}, providers);
    const Diagnostic in_reverse = only_error(validate_in(
        dir.path(), workflow_of("test.reverse", {{"a", "fx-service-minor"}, {"b", "fx-service"}}),
        *reverse));
    EXPECT_EQ(in_reverse.code, ErrorCode::LIBRARY_NOT_FOUND);
    EXPECT_EQ(in_reverse.step, "b");
    EXPECT_EQ(in_reverse.module, "fx-service@0.1.0");
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(in_reverse.location.file), manifest));
}

// Every required service that does not resolve is reported, for its step, at its own index.
TEST(ServiceValidation, EveryUnavailableServiceIsReportedAtItsIndex) {
    ScratchDir dir;
    const fs::path modules = dir.path() / "modules";
    praenora::test::copy_fixture_module("fx-service", modules, [](json& manifest) {
        manifest["requires"]["services"] = json::array(
            {{{"name", "test.echo"}, {"major", 0}, {"minor", 0}, {"optional", true}},
             {{"name", "test.absent"}, {"major", 0}, {"minor", 0}, {"optional", false}},
             {{"name", "test.missing"}, {"major", 2}, {"minor", 1}, {"optional", false}}});
    });
    // The copy of fx-service comes first on the module search path.
    const auto context = praenora::test::make_test_context(
        {utf8(modules), PRAENORA_TEST_MODULES_DIR}, {},
        {PRAENORA_TEST_PROVIDERS_DIR "/test-echo"});
    const auto validated = validate_in(
        dir.path(), workflow_of("test.chain", {{"a", "fx-service"}, {"b", "fx-service-minor"}}),
        *context);
    const auto errors = validated.errors();
    ASSERT_EQ(errors.size(), 3u) << validated.report.to_text();
    const std::vector<std::tuple<std::string, std::string, std::string>> expected = {
        {"a", "/requires/services/1", "test.absent@0.0"},
        {"a", "/requires/services/2", "test.missing@2.1"},
        {"b", "/requires/services/0", "test.echo@0.7"}};
    for (std::size_t i = 0; i < errors.size(); ++i) {
        const auto& [step, pointer, service] = expected[i];
        SCOPED_TRACE(i);
        EXPECT_EQ(errors[i].code, ErrorCode::SERVICE_UNAVAILABLE);
        EXPECT_EQ(errors[i].step, step);
        EXPECT_EQ(errors[i].location.pointer, pointer);
        EXPECT_TRUE(contains(errors[i].message, service)) << errors[i].message;
    }
    EXPECT_TRUE(contains(errors[0].location.file, "modules")) << errors[0].location.file;
    EXPECT_EQ(json::parse(errors[1].details_json).at("service"), "test.missing");
}

TEST(ServiceValidation, NothingIsLoaded) {
    // A provider name that no test loads: validation does not load it either.
    ScratchDir dir;
    praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers",
                                          [](json& manifest) {
                                              manifest["provider"]["name"] = "test-echo-unloaded";
                                          });
    const auto context = context_with({dir.path() / "providers"});
    const auto validated = validate_in(dir.path(), single("fx-service"), *context);
    ASSERT_TRUE(validated.workflow.has_value()) << validated.report.to_text();
    EXPECT_EQ(validated.workflow->services.at("test.echo@0").provider->name, "test-echo-unloaded");
    EXPECT_EQ(praenora::detail::loaded_provider("test-echo-unloaded"), nullptr);
}

// A run whose provider does not load fails before any module is loaded: the engine throws the
// provider loader's error, located in the provider's manifest, and records it; the step is never
// created.
namespace {

struct FailedRun {
    Error error;
    WorkflowState state;
    StepState step;
    json record;
};

// Validates `yaml` against `context` in `dir`, calls `between` (to change something after the
// validation), then starts the run, which must fail.
template <class Between>
FailedRun start_failing(const fs::path& dir, const std::string& yaml, const RuntimeContext& context,
                        Between between) {
    auto validated = validate_in(dir, yaml, context);
    if (!validated.workflow) {
        ADD_FAILURE() << validated.report.to_text();
        throw std::runtime_error("validation failed");
    }
    between();
    CapturedLogs logs;
    MetricsRegistry metrics;
    EngineSettings settings;
    settings.logs = &logs.hub();
    settings.metrics = &metrics;
    Engine engine(std::move(*validated.workflow), settings);
    std::optional<Error> error;
    try {
        engine.start();
    } catch (const Error& e) {
        error = e;
    }
    if (!error) {
        ADD_FAILURE() << "the run started";
        throw std::runtime_error("no failure");
    }
    std::ifstream in(dir / "out" / "run-record.json", std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return FailedRun{*error, engine.state(), metrics.snapshot().steps.at(0).state,
                     json::parse(text.str())};
}

FailedRun start_failing(const fs::path& dir, const std::string& yaml,
                        const RuntimeContext& context) {
    return start_failing(dir, yaml, context, [] {});
}

void expect_failed_before_any_module(const FailedRun& run, ErrorCode code) {
    EXPECT_EQ(run.error.code(), code) << run.error.to_json();
    EXPECT_EQ(run.state, WorkflowState::failed);
    EXPECT_EQ(run.step, StepState::loaded); // never created
    const json& error = run.record.at("execution").at("error");
    EXPECT_EQ(error.at("code"), static_cast<int>(code)) << error.dump();
    EXPECT_EQ(run.record.at("execution").at("ticks_completed"), 0);
    EXPECT_TRUE(contains(run.error.location().file, ".provider.json")) << run.error.to_json();
}

std::string reason_of(const Error& error) {
    return json::parse(error.details_json()).value("reason", std::string());
}

} // namespace

TEST(ProviderLoadFailure, StartFailureFailsTheRun) {
    ScratchDir dir;
    const auto context = praenora::test::make_provider_context({"test-start-fails"});
    const FailedRun run = start_failing(dir.path(), single("fx-service"), *context);
    expect_failed_before_any_module(run, ErrorCode::LIBRARY_LOAD_FAILED);
    EXPECT_EQ(reason_of(run.error), "start");
    EXPECT_TRUE(contains(run.error.what(), "the engine cannot start")) << run.error.what();
}

TEST(ProviderLoadFailure, IdentityMismatchFailsTheRun) {
    ScratchDir dir;
    const auto context = praenora::test::make_provider_context({"test-identity-mismatch"});
    const FailedRun run = start_failing(dir.path(), single("fx-service"), *context);
    expect_failed_before_any_module(run, ErrorCode::LIBRARY_LOAD_FAILED);
    EXPECT_EQ(reason_of(run.error), "identity");
}

TEST(ProviderLoadFailure, InvalidTableFailsTheRun) {
    ScratchDir dir;
    const auto context = praenora::test::make_provider_context({"test-table-reserved"});
    const FailedRun run = start_failing(dir.path(), single("fx-service"), *context);
    expect_failed_before_any_module(run, ErrorCode::API_TABLE_INVALID);
}

TEST(ProviderLoadFailure, MissingServiceFailsTheRun) {
    // A copy of fx-service that requires test.absent@0, which test-missing-service offers but
    // does not serve.
    ScratchDir dir;
    const fs::path modules = dir.path() / "modules";
    praenora::test::copy_fixture_module("fx-service", modules, [](json& manifest) {
        manifest["requires"]["services"] = json::array(
            {{{"name", "test.absent"}, {"major", 0}, {"minor", 0}, {"optional", false}}});
    });
    auto context = praenora::test::make_test_context(
        {utf8(modules)}, {}, {PRAENORA_TEST_PROVIDERS_DIR "/test-missing-service"});
    const FailedRun run = start_failing(dir.path(), single("fx-service"), *context);
    expect_failed_before_any_module(run, ErrorCode::LIBRARY_LOAD_FAILED);
    EXPECT_EQ(reason_of(run.error), "service");
    EXPECT_EQ(json::parse(run.error.details_json()).at("services"), json({"test.absent@0"}));
}

TEST(ProviderLoadFailure, LibraryChangedAfterValidationFailsTheRun) {
    ScratchDir dir;
    const fs::path manifest =
        praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers");
    const fs::path library = praenora::detail::load_provider_manifest(manifest).library_path;
    const auto context = context_with({dir.path() / "providers"});
    const FailedRun run = start_failing(dir.path(), single("fx-service"), *context, [&] {
        praenora::test::append_to_file(library, "changed");
    });
    expect_failed_before_any_module(run, ErrorCode::LIBRARY_DIGEST_MISMATCH);
    EXPECT_EQ(run.error.location().pointer, "/library/path");
    EXPECT_TRUE(contains(run.error.what(), "changed after validation")) << run.error.what();
}

TEST(ProviderLoadFailure, LibraryRemovedAfterValidationFailsTheRun) {
    ScratchDir dir;
    const fs::path manifest =
        praenora::test::copy_fixture_provider("test-echo", dir.path() / "providers");
    const fs::path library = praenora::detail::load_provider_manifest(manifest).library_path;
    const auto context = context_with({dir.path() / "providers"});
    const FailedRun run = start_failing(dir.path(), single("fx-service"), *context,
                                        [&] { fs::remove(library); });
    expect_failed_before_any_module(run, ErrorCode::LIBRARY_NOT_FOUND);
    EXPECT_EQ(run.error.location().pointer, "/library/path");
}
