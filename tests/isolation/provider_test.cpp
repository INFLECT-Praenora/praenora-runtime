// Service providers in module hosts, white-box: a module host loads and starts the providers of
// its module's services before the module, serves their services through get_service(), and
// passes their log records on to the runtime's loggers with their attributes; their load errors
// keep the provider loader's codes and messages, located in their manifests; a module host that
// names no provider of its load breaks the channel's protocol; and the runtime's process loads only
// the providers of its own steps (opened_providers_for_testing()). The tests pass when one process
// runs them all, and again: a test that checks that a load in a module host opens no provider in
// this process compares the providers opened here before and after it, and the one whose
// precondition is that a provider is not open here yet is skipped when an earlier test opened it.
#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "isolation/child_module.hpp"
#include "isolation/child_settings.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "instance_fixtures.hpp"
#include "isolation_support.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/workflow.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::CallOutcome;
using praenora::detail::ChildProvider;
using praenora::detail::ChildSettings;
using praenora::detail::InstanceOptions;
using praenora::detail::load_manifest;
using praenora::detail::load_provider_manifest;
using praenora::detail::LoadedModule;
using praenora::detail::ModuleInstance;
using praenora::detail::ProviderManifest;
using praenora::detail::ServiceVersion;
using praenora::test::CapturedLogs;
using praenora::test::child_settings;
using praenora::test::create_document;
using praenora::test::fixture_manifest;
using praenora::test::initialize_document;
using praenora::test::instance_options;
using praenora::test::invocation_document;
using praenora::test::ScratchDir;

#ifndef _WIN32

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool opened_here(const std::string& provider) {
    const auto opened = praenora::detail::opened_providers_for_testing();
    return std::find(opened.begin(), opened.end(), provider) != opened.end();
}

std::shared_ptr<const ProviderManifest> provider_manifest(const std::string& name) {
    return std::make_shared<const ProviderManifest>(
        load_provider_manifest(praenora::test::fixture_provider_manifest(name)));
}

// Fixture provider `name`, serving `service` with validation's digest of its library, its records
// going to the logger provider.<name> of `logs`.
ChildProvider fixture_provider(const std::string& name, const ServiceVersion& service,
                               CapturedLogs& logs) {
    ChildProvider provider;
    provider.manifest = provider_manifest(name);
    provider.library_sha256 = praenora::detail::check_provider_library_file(*provider.manifest);
    provider.services = {service};
    provider.logger = logs.hub().logger("provider." + name);
    return provider;
}

// test-simulation, serving praenora.simulation@0.0.
ChildProvider test_simulation(CapturedLogs& logs) {
    return fixture_provider("test-simulation", ServiceVersion{"praenora.simulation", 0, 0}, logs);
}

// The bodies of the events of `logs` that logger `logger` recorded, in order.
std::vector<std::string> bodies_of(const CapturedLogs& logs, const std::string& logger);

// simulating-module's parameters, with their defaults (its manifest's), and a kernel.
json simulation_parameters() {
    return {{"model", R"({"states": {"x": 8, "y": 3}, "parameters": {"k_x": 0.6931471805599453, )"
                      R"("k_y": 0}})"},
            {"format", "test-decay"},
            {"plan",
             {{"parameters", {"k_x"}},
              {"initial_conditions", {"x"}},
              {"time_domain", {{"start", 0}, {"end", 2}}},
              {"output",
               {{"save_at", "explicit_times"}, {"times", {0, 1, 2}}, {"quantities", {"total"}}}}}},
            {"values", {0.6931471805599453, 8}},
            {"kernel", true}};
}

// The events of `logs` that logger `logger` recorded.
std::vector<json> events_of(const CapturedLogs& logs, const std::string& logger) {
    std::vector<json> events;
    for (const json& event : logs.events()) {
        if (event.at("attributes").value("logger", std::string()) == logger) {
            events.push_back(event);
        }
    }
    return events;
}

std::vector<std::string> bodies_of(const CapturedLogs& logs, const std::string& logger) {
    std::vector<std::string> bodies;
    for (const json& event : events_of(logs, logger)) {
        bodies.push_back(event.at("body").get<std::string>());
    }
    return bodies;
}

// Waits up to `limit` until `logs` hold the record of a module host's crash (module_host.crashed),
// which the runtime logs once it knows how the module host ended.
bool crash_recorded_within(const CapturedLogs& logs, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        for (const json& event : logs.events()) {
            if (event.at("attributes").value("event", std::string()) == "module_host.crashed") {
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct LoadFailure {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    praenora::SourceLocation location;
    std::string details;
};

// How loading simulating-module in a module host with `provider` fails.
LoadFailure child_failure(const ChildProvider& provider) {
    CapturedLogs logs;
    ChildSettings settings = child_settings(logs);
    settings.providers = {provider};
    try {
        static_cast<void>(LoadedModule::load_in_child(
            load_manifest(fixture_manifest("simulating-module")), settings));
    } catch (const Error& error) {
        return LoadFailure{error.code(), error.what(), error.location(), error.details_json()};
    }
    ADD_FAILURE() << "the load did not fail";
    return LoadFailure{};
}

} // namespace

// The module host serves the services of the providers it started, and nothing in this process
// opens the provider: the lookups are recorded as they are in this process, the module computes
// with the service, and the provider's records, with their attributes, reach the logger the
// runtime gave it.
TEST(Isolation, ProvidersStartInTheModuleHostAndLogThroughTheRuntime) {
    const std::vector<std::string> opened = praenora::detail::opened_providers_for_testing();
    CapturedLogs logs;
    ChildSettings settings = child_settings(logs);
    settings.providers = {test_simulation(logs)};
    const std::string digest = settings.providers.front().library_sha256;
    const auto module =
        LoadedModule::load_in_child(load_manifest(fixture_manifest("simulating-module")), settings);
    InstanceOptions options = instance_options(logs);
    options.record_lookups = true;
    ModuleInstance instance(module, std::move(options));
    ASSERT_EQ(
        instance.create(create_document("simulating-module", simulation_parameters())).outcome,
        CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const praenora::detail::CallResult invoked = instance.invoke(invocation_document(0), 0);
    ASSERT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
    // x(t) = 8 * 2^-t, through the service: the rows at t = 0, 1, 2.
    const json data = json::parse(invoked.output).at("outputs").at("out").at("data");
    EXPECT_EQ(data.at("ok"), true) << data;
    EXPECT_EQ(data.at("rows").size(), 3u) << data;
    EXPECT_EQ(data.at("rates").size(), 2u) << data;
    const auto lookups = instance.take_lookups();
    ASSERT_EQ(lookups.size(), 1u);
    EXPECT_EQ(lookups.front().phase, "create");
    EXPECT_EQ(lookups.front().service, "praenora.simulation");
    EXPECT_TRUE(lookups.front().provided);

    const std::vector<json> records = events_of(logs, "provider.test-simulation");
    ASSERT_EQ(records.size(), 2u) << logs.events().size();
    EXPECT_EQ(records[0].at("body"), "test-simulation started");
    EXPECT_EQ(records[0].at("severity_text"), "INFO");
    EXPECT_EQ(records[0].at("attributes"), json({{"logger", "provider.test-simulation"},
                                                 {"provider", "test-simulation@0.1.0"}}));
    EXPECT_EQ(records[1].at("body"), "provider test-simulation@0.1.0 loaded and started");
    EXPECT_EQ(records[1].at("attributes"), json({{"logger", "provider.test-simulation"},
                                                 {"provider", "test-simulation@0.1.0"},
                                                 {"event", "provider.loaded"},
                                                 {"library_sha256", digest}}));
    EXPECT_EQ(praenora::detail::opened_providers_for_testing(), opened);
}

// The module host records the levels that the runtime's logger of the provider records, no lower,
// and answers the provider's log_enabled() with them, as this process would: test-log-levels says
// at warn whether log_enabled() reports info as recorded, then logs "test-log-levels started" at
// info. A provider without a logger records nothing.
TEST(Isolation, ProviderRecordsKeepTheLevelsOfTheRuntimesLogger) {
    struct Case {
        const char* name;
        std::optional<praenora::LogLevel> level; // none: no logger
        std::vector<std::string> bodies;
    };
    const std::vector<Case> cases = {
        {"warn", praenora::LogLevel::warn, {"test-log-levels: info is not recorded"}},
        {"info",
         praenora::LogLevel::info,
         {"test-log-levels: info is recorded", "test-log-levels started",
          "provider test-log-levels@0.1.0 loaded and started"}},
        {"no logger", std::nullopt, {}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        CapturedLogs logs;
        ChildSettings settings = child_settings(logs);
        ChildProvider provider =
            fixture_provider("test-log-levels", ServiceVersion{"test.echo", 0, 0}, logs);
        if (c.level) {
            praenora::detail::set_log_level(*provider.logger, *c.level);
        } else {
            provider.logger.reset();
        }
        settings.providers = {provider};
        const auto module =
            LoadedModule::load_in_child(load_manifest(fixture_manifest("fx-service")), settings);
        EXPECT_EQ(bodies_of(logs, "provider.test-log-levels"), c.bodies);
    }
}

// A provider runs in its module host's process: one that ends it during a service call crashes
// that call (MODULE_CRASHED, phase invoke, the signal), and one that ends it between calls, from a
// thread of its own, crashes the next call (phase finalize here). test-echo-crashes's echo()
// aborts at once, or, for an input that holds "later", returns and aborts from a thread of its own
// once the file that PRAENORA_TEST_ECHO_CRASH_AFTER names exists: the test makes it after the
// invoke returned, and calls finalize only once the runtime has recorded the crash, so the crash
// is between calls whatever the machine's speed. fx-service echoes the data of its input in invoke.
TEST(Isolation, AProviderThatEndsItsModuleHostCrashesTheCallOrTheNext) {
    for (const std::string when : {"now", "later"}) {
        SCOPED_TRACE(when);
        const praenora::test::OwnSignals own_signals;
        const ScratchDir directory;
        const fs::path trigger = directory.path() / "end-the-module-host";
        // Inherited by the module host, which is started below.
        const praenora::test::ScopedEnvironment crash_after(
            "PRAENORA_TEST_ECHO_CRASH_AFTER", praenora::detail::path_to_utf8(trigger));
        CapturedLogs logs;
        ChildSettings settings = child_settings(logs);
        settings.providers = {
            fixture_provider("test-echo-crashes", ServiceVersion{"test.echo", 0, 0}, logs)};
        const auto module =
            LoadedModule::load_in_child(load_manifest(fixture_manifest("fx-service")), settings);
        const std::int64_t pid = praenora::test::module_host_pid(*module);
        ModuleInstance instance(module, instance_options(logs));
        ASSERT_EQ(instance.create(create_document("fx-service")).outcome, CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        const json data = {{"crash", when}};
        const praenora::detail::CallResult invoked =
            instance.invoke(invocation_document(0, &data), 0);
        std::string phase = "invoke";
        praenora::detail::CallResult crashed = invoked;
        if (when == "later") {
            ASSERT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
            // The provider's thread waits for the file: the module host is still there.
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            EXPECT_TRUE(praenora::test::process_running(pid)) << pid;
            praenora::test::write_text_file(trigger, "");
            // The module host ends between calls; once the runtime has recorded that, the next
            // call reports it.
            EXPECT_TRUE(crash_recorded_within(logs, std::chrono::seconds(10)));
            phase = "finalize";
            crashed = instance.finalize();
        }
        EXPECT_EQ(crashed.outcome, CallOutcome::crashed) << crashed.error_json;
        EXPECT_EQ(json::parse(crashed.details_json),
                  json({{"phase", phase},
                        {"signal", SIGABRT},
                        {"signal_name", praenora::detail::signal_name(SIGABRT)}}));
        EXPECT_EQ(instance.state(), praenora::StepState::crashed);
        EXPECT_TRUE(praenora::test::process_gone(pid)) << pid;
        std::string record;
        for (const json& event : logs.events()) {
            if (event.at("attributes").value("event", std::string()) == "module_host.crashed") {
                record = event.at("body").get<std::string>();
            }
        }
        EXPECT_TRUE(contains(record, when == "now" ? "crashed during invoke" : "between calls"))
            << record;
    }
}

// A provider's load error in a module host is the provider loader's, located in the provider's
// manifest: a library whose SHA-256 is not validation's (LIBRARY_DIGEST_MISMATCH at /library/path,
// before the library opens, as in this process), and a manifest that changed after the runtime
// read it (MANIFEST_INVALID).
TEST(Isolation, AProviderLoadErrorIsLocatedInTheProvidersManifest) {
    const std::vector<std::string> opened = praenora::detail::opened_providers_for_testing();
    CapturedLogs logs;
    ChildProvider provider = test_simulation(logs);
    std::string wrong = provider.library_sha256;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    provider.library_sha256 = wrong;
    const LoadFailure there = child_failure(provider);
    LoadFailure here;
    try {
        // Refused before the library opens: nothing of the provider stays in this process.
        static_cast<void>(
            praenora::detail::load_provider(*provider.manifest, provider.services, wrong, nullptr));
        ADD_FAILURE() << "the load did not fail";
    } catch (const Error& error) {
        here = LoadFailure{error.code(), error.what(), error.location(), error.details_json()};
    }
    EXPECT_EQ(praenora::detail::opened_providers_for_testing(), opened);
    EXPECT_EQ(here.code, ErrorCode::LIBRARY_DIGEST_MISMATCH) << here.message;
    EXPECT_EQ(there.code, here.code) << there.message;
    EXPECT_EQ(there.message, here.message);
    EXPECT_EQ(here.location.file, praenora::detail::path_to_utf8(provider.manifest->manifest_path));
    EXPECT_EQ(there.location.file, here.location.file);
    EXPECT_EQ(there.location.pointer, "/library/path");
    EXPECT_EQ(there.location.pointer, here.location.pointer);
    EXPECT_EQ(json::parse(there.details), json::parse(here.details));

    ChildProvider changed = test_simulation(logs);
    ProviderManifest edited = *changed.manifest;
    edited.manifest_sha256 = std::string(64, '0');
    changed.manifest = std::make_shared<const ProviderManifest>(edited);
    const LoadFailure manifest = child_failure(changed);
    EXPECT_EQ(manifest.code, ErrorCode::MANIFEST_INVALID) << manifest.message;
    EXPECT_EQ(manifest.location.file, praenora::detail::path_to_utf8(edited.manifest_path));
    EXPECT_TRUE(contains(manifest.message, "provider test-simulation@0.1.0: the manifest changed "
                                           "after the runtime read it"))
        << manifest.message;
}

// A module host that sends a provider's log record, or a load error, naming a provider the load
// did not have breaks the channel's protocol: a crash with reason "protocol" during the load.
TEST(Isolation, AModuleHostThatNamesNoProviderOfItsLoadBreaksTheProtocol) {
    const ScratchDir directory;
    fs::create_symlink(praenora::test::utf8_path(PRAENORA_TEST_FAKE_MODULE_HOST),
                       directory.path() / "praenora-module-host");
    const praenora::test::ModuleHostDirectory hook(directory.path());
    for (const char* mode : {"provider-log", "provider-error"}) {
        SCOPED_TRACE(mode);
        const praenora::test::ScopedEnvironment fake("PRAENORA_TEST_FAKE_MODULE_HOST", mode);
        CapturedLogs logs;
        ChildSettings settings = child_settings(logs);
        settings.providers = {test_simulation(logs)};
        try {
            static_cast<void>(LoadedModule::load_in_child(
                load_manifest(fixture_manifest("simulating-module")), settings));
            ADD_FAILURE() << "the load did not fail";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
            EXPECT_EQ(json::parse(error.details_json()),
                      json({{"phase", "load"}, {"reason", "protocol"}}));
        }
        // The record of the crash says what was wrong.
        std::string crashed;
        for (const json& event : logs.events()) {
            if (event.at("attributes").value("event", std::string()) == "module_host.crashed") {
                crashed = event.at("body").get<std::string>();
            }
        }
        EXPECT_TRUE(contains(crashed, "names no provider")) << crashed;
    }
}

// The runtime's process loads only the providers of the steps that run in it: a provider that only
// steps in module hosts use is never opened here, by a run or by validate --load. converting-module
// (step a) uses test-conversion and test-simulation; simulating-module (step b) uses
// test-simulation. A provider stays open in a process for good, so what this test checks needs both
// providers not open here yet: it is skipped when an earlier test (or an earlier repetition of this
// one) opened them.
TEST(Isolation, TheRuntimesProcessLoadsOnlyTheProvidersOfItsSteps) {
    if (opened_here("test-conversion") || opened_here("test-simulation")) {
        GTEST_SKIP() << "test-conversion or test-simulation is open in this process already: "
                        "whether a run opens it cannot be told";
    }
    ScratchDir dir;
    praenora::test::write_text_file(dir.path() / "source.json", "{\"value\": 1}");
    praenora::test::write_text_file(
        dir.path() / "workflow.yaml",
        "kind: workflow-config\n"
        "format_version: \"0.1.0\"\n"
        "workflow: { name: test.chain, version: \"0.1.0\" }\n"
        "run: { ticks: 2 }\n"
        "inputs:\n"
        "  source: { source: json-document, path: source.json }\n"
        "steps:\n"
        "  a:\n"
        "    module: { name: converting-module, version: \"0.1.0\" }\n"
        "    parameters: { simulate: { time_domain: { start: 0, end: 1 }, output: { save_at: "
        "explicit_times, times: [1] } } }\n"
        "    isolation: { mode: process }\n"
        "  b: { module: { name: simulating-module, version: \"0.1.0\" } }\n");
    const std::string config = praenora::detail::path_to_utf8(dir.path() / "workflow.yaml");
    praenora::RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.provider_paths = {PRAENORA_TEST_PROVIDERS_DIR "/test-conversion",
                              PRAENORA_TEST_PROVIDERS_DIR "/test-simulation"};
    options.use_environment = false;
    options.logging.to_stderr = false;
    const praenora::Runtime runtime(options);
    const auto run = [&](std::optional<praenora::IsolationMode> mode) {
        praenora::WorkflowOptions overrides;
        overrides.isolation = mode;
        EXPECT_EQ(runtime.open(config, overrides)->run().final_state,
                  praenora::WorkflowState::completed);
    };

    // Every step in a module host: no provider here.
    run(praenora::IsolationMode::process);
    EXPECT_FALSE(opened_here("test-conversion"));
    EXPECT_FALSE(opened_here("test-simulation"));
    // Step b here: its provider only.
    praenora::ValidateOptions load;
    load.load = true;
    const praenora::ValidationReport report = runtime.validate(config, load);
    EXPECT_TRUE(report.ok()) << report.to_text();
    EXPECT_FALSE(opened_here("test-conversion"));
    EXPECT_TRUE(opened_here("test-simulation"));
    run(std::nullopt);
    EXPECT_FALSE(opened_here("test-conversion"));
    // Every step here: both.
    run(praenora::IsolationMode::in_process);
    EXPECT_TRUE(opened_here("test-conversion"));
}

#endif // _WIN32
