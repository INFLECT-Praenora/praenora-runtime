// Runs that use runtime services of providers, through validate_workflow() and the engine: each
// step's module sees the provider services its manifest declares and that resolved for the run,
// and the built-in ones, and nothing else; the run's providers are loaded and started before any
// module instance is created; a module whose manifest has format 0.1 sees no provider service.
// The modules are built from fx-service (tests/fixtures/modules), which reports what it found;
// the providers are fixture providers (tests/fixtures/providers).
#include "api/validation.hpp"
#include "engine/engine.hpp"
#include "instance/services.hpp"
#include "observability/metrics.hpp"
#include "provider/provider_loader.hpp"

#include "instance_fixtures.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>

#include "test_gate.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::TickResult;
using praenora::WorkflowState;
using praenora::detail::Engine;
using praenora::detail::EngineSettings;
using praenora::detail::MetricsRegistry;
using praenora::detail::RuntimeContext;
using praenora::detail::ServiceRegistry;
using praenora::test::CapturedLogs;
using praenora::test::ScratchDir;
using praenora::test::TestGate;
using praenora::test::write_text_file;

namespace {

// A configuration of `workflow` (test.single or test.chain) over source.json, one tick, with
// `steps` (a YAML block).
std::string config(const std::string& workflow, const std::string& steps) {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: " +
           workflow +
           ", version: \"0.1.0\" }\n"
           "run: { ticks: 1, seed: 7 }\n"
           "inputs:\n"
           "  source: { source: json-document, path: source.json }\n"
           "steps:\n" +
           steps;
}

// A step block binding `id` to fixture module `module` with `parameters`.
std::string step(const std::string& id, const std::string& module,
                 const std::string& parameters = "{}") {
    return "  " + id + ":\n    module: { name: " + module +
           ", version: \"0.1.0\" }\n    parameters: " + parameters + "\n";
}

// One run against a context: validated, started and run for its tick.
struct ServiceRunFixture {
    ScratchDir dir;
    CapturedLogs logs;
    MetricsRegistry metrics;
    std::unique_ptr<Engine> engine;

    // Validates `yaml` against `context` and builds the engine, with `builtin` as the built-in
    // services. Throws when validation fails (after reporting why).
    Engine& open(const std::string& yaml, const RuntimeContext& context,
                 const ServiceRegistry* builtin = nullptr) {
        write_text_file(dir.path() / "source.json", "{\"value\": 42}");
        auto validated = praenora::test::validate_text(dir.path(), yaml, {}, context);
        if (!validated.workflow) {
            ADD_FAILURE() << validated.report.to_text();
            throw std::runtime_error("validation failed");
        }
        EngineSettings settings;
        settings.logs = &logs.hub();
        settings.metrics = &metrics;
        settings.services = builtin;
        engine = std::make_unique<Engine>(std::move(*validated.workflow), settings);
        return *engine;
    }

    // Starts the run and runs its tick; the data of the workflow output `result`.
    json run_one_tick() {
        engine->start();
        const TickResult tick = engine->run_tick();
        EXPECT_TRUE(tick.finished);
        EXPECT_EQ(tick.outputs, std::vector<std::string>{"result"});
        return json::parse(engine->latest_outputs_json()).at("result").at("data");
    }

    // The bodies of the captured log events, in order.
    std::vector<std::string> bodies() const {
        std::vector<std::string> bodies;
        for (const json& event : logs.events()) {
            bodies.push_back(event.value("body", std::string()));
        }
        return bodies;
    }
};

std::ptrdiff_t index_of(const std::vector<std::string>& items, const std::string& item) {
    const auto found = std::find(items.begin(), items.end(), item);
    return found == items.end() ? -1 : found - items.begin();
}

} // namespace

TEST(ServiceRun, ModulesSeeTheServicesTheyDeclare) {
    // test-echo serves test.echo@0 (it comes first); test-reordered serves test.other@1, which
    // only step a declares. Step b's module declares test.echo@0 only: it does not get
    // test.other@1, although the run loaded a provider of it, and it gets the built-in test.gate.
    const auto context = praenora::test::make_provider_context({"test-echo", "test-reordered"});
    TestGate gate;
    ServiceRegistry builtin;
    builtin.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    ServiceRunFixture run;
    Engine& engine = run.open(
        config("test.chain",
               step("a", "fx-service", "{ lookups: [\"test.other@1\", \"test.gate@0\"] }") +
                   step("b", "fx-service-optional",
                        "{ lookups: [\"test.other@1\", \"test.gate@0\"] }")),
        *context, &builtin);

    const json result = run.run_one_tick();
    const json& a = result.at("echo"); // step b echoed what step a wrote
    EXPECT_EQ(a.at("echo"), json({{"value", 42}}));
    EXPECT_EQ(a.at("found"),
              json({{"test.echo@0", true}, {"test.other@1", true}, {"test.gate@0", true}}));
    EXPECT_EQ(result.at("found"),
              json({{"test.echo@0", true}, {"test.other@1", false}, {"test.gate@0", true}}));
    EXPECT_EQ(engine.state(), WorkflowState::completed);

    const json reproducibility = engine.run_record().at("reproducibility");
    EXPECT_EQ(reproducibility.at("services").at("test.echo@0").at("provider").at("name"),
              "test-echo");
    EXPECT_EQ(reproducibility.at("services").at("test.other@1").at("provider").at("name"),
              "test-reordered");
    EXPECT_EQ(reproducibility.at("services").at("test.other@1").at("minor"), 2);
    EXPECT_EQ(reproducibility.at("steps").at("a").at("services"),
              json({"test.echo@0", "test.other@1"}));
    EXPECT_EQ(reproducibility.at("steps").at("b").at("services"), json({"test.echo@0"}));
}

TEST(ServiceRun, ProvidersStartBeforeAnyModuleIsCreated) {
    // test-order logs "test-order started" from start(), the first time it is loaded in the
    // process; fx-service logs "fx-service:create" from create().
    const bool first_load = praenora::detail::loaded_provider("test-order") == nullptr;
    const auto context = praenora::test::make_provider_context({"test-order"});
    ServiceRunFixture run;
    run.open(config("test.single", step("a", "fx-service")), *context);
    const json result = run.run_one_tick();
    EXPECT_EQ(result.at("found").at("test.echo@0"), true);
    EXPECT_TRUE(result.at("echo").is_null()); // test-order's tables are only a header
    ASSERT_NE(praenora::detail::loaded_provider("test-order"), nullptr);

    const auto bodies = run.bodies();
    const std::ptrdiff_t created = index_of(bodies, "fx-service:create");
    ASSERT_GE(created, 0);
    if (first_load) {
        const std::ptrdiff_t started = index_of(bodies, "test-order started");
        ASSERT_GE(started, 0);
        EXPECT_LT(started, created);
        const std::ptrdiff_t loaded =
            index_of(bodies, "provider test-order@0.1.0 loaded and started");
        ASSERT_GE(loaded, 0);
        EXPECT_LT(loaded, created);
    }
}

TEST(ServiceRun, ModuleWithAFormat01ManifestSeesNoProviderService) {
    const auto context = praenora::test::make_provider_context({"test-echo"});
    ServiceRunFixture run;
    Engine& engine = run.open(config("test.single", step("a", "fx-service-format-0-1",
                                                          "{ lookups: [\"test.echo@1\"] }")),
                              *context);
    EXPECT_EQ(engine.run_record(), nullptr);
    const json result = run.run_one_tick();
    EXPECT_EQ(result.at("found"), json({{"test.echo@0", false}, {"test.echo@1", false}}));
    EXPECT_TRUE(result.at("echo").is_null());
    const json reproducibility = engine.run_record().at("reproducibility");
    EXPECT_EQ(reproducibility.at("services"), json::object());
    EXPECT_EQ(reproducibility.at("steps").at("a").at("services"), json::array());
}

TEST(ServiceRun, StopRequestedBeforeStartLoadsNoProvider) {
    // A run stopped before it starts ends stopped, without loading its providers: test-start-fails
    // would fail the run if it were loaded.
    const auto context = praenora::test::make_provider_context({"test-start-fails"});
    ServiceRunFixture run;
    Engine& engine = run.open(config("test.single", step("a", "fx-service")), *context);
    engine.request_stop();
    EXPECT_NO_THROW(engine.start());
    EXPECT_EQ(engine.state(), WorkflowState::stopped);
    const auto summary = engine.summary();
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, praenora::ErrorCode::RUN_STOPPED);
    EXPECT_EQ(index_of(run.bodies(), "fx-service:create"), -1);
}
