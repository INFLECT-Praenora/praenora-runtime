// What a module instance's get_service() serves: the provider services of its step
// (InstanceOptions::provider_services, the services its manifest declares that resolved for the
// run) first, then the runtime's built-in services, and nothing else; nothing at all once its
// services are withdrawn. The module is fx-service, which reports what it found
// (tests/fixtures/modules/fx-service); the provider service is test-echo's test.echo@0.
#include "instance/module_instance.hpp"
#include "instance/services.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "instance_fixtures.hpp"
#include "provider_fixtures.hpp"

#include <praenora/runtime/status.hpp>

#include "test_echo.h"
#include "test_gate.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using nlohmann::json;
using praenora::StepState;
using praenora::detail::CallOutcome;
using praenora::detail::CallResult;
using praenora::detail::InstanceOptions;
using praenora::detail::ModuleInstance;
using praenora::detail::ServiceRegistry;
using praenora::test::CapturedLogs;
using praenora::test::create_document;
using praenora::test::initialize_document;
using praenora::test::instance_options;
using praenora::test::invocation_document;
using praenora::test::load_fixture_module;
using praenora::test::TestGate;

namespace {

// test.echo@0 of the fixture provider test-echo, loaded and started in this process.
const void* echo_table() {
    const auto manifest = praenora::detail::load_provider_manifest(
        praenora::test::fixture_provider_manifest("test-echo"));
    const auto& provider = praenora::detail::load_provider(
        manifest, {{PRAENORA_TEST_ECHO_SERVICE, 0, 0}}, std::nullopt, nullptr);
    return provider.service(PRAENORA_TEST_ECHO_SERVICE, 0);
}

// What fx-service saw in one invocation with input {"value": 7}: {"echo", "found"}.
json invoke_once(ModuleInstance& instance, std::uint64_t tick) {
    const json data = {{"value", 7}};
    const CallResult result = instance.invoke(invocation_document(tick, &data), tick);
    EXPECT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    return json::parse(result.output).at("outputs").at("out").at("data");
}

// A ready instance of fx-service for `options`, looking up `lookups` besides test.echo@0.
std::unique_ptr<ModuleInstance> ready(InstanceOptions options, const json& lookups) {
    auto instance =
        std::make_unique<ModuleInstance>(load_fixture_module("fx-service"), std::move(options));
    EXPECT_EQ(instance->create(create_document("fx-service", {{"lookups", lookups}})).outcome,
              CallOutcome::succeeded);
    EXPECT_EQ(instance->initialize(initialize_document()).outcome, CallOutcome::succeeded);
    return instance;
}

} // namespace

TEST(ServiceVisibility, ProviderServicesBesideTheBuiltInOnes) {
    CapturedLogs logs;
    TestGate gate;
    ServiceRegistry builtin;
    builtin.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    InstanceOptions options = instance_options(logs);
    options.services = &builtin;
    options.provider_services.add(PRAENORA_TEST_ECHO_SERVICE, 0, echo_table());

    auto instance = ready(options, {"test.gate@0", "test.echo@1", "test.other@1"});
    const json seen = invoke_once(*instance, 0);
    EXPECT_EQ(seen.at("echo"), json({{"value", 7}})); // through test-echo's table
    EXPECT_EQ(seen.at("found"), json({{"test.echo@0", true},
                                      {"test.gate@0", true},
                                      {"test.echo@1", false},
                                      {"test.other@1", false}}));
}

TEST(ServiceVisibility, ProviderServicesComeBeforeBuiltInOnes) {
    // A built-in service of the same name and major is not what the module gets: the provider's
    // table is, as the run's fingerprint records it.
    CapturedLogs logs;
    const praenora_api_header header_only{0u, 0u, sizeof(praenora_api_header), 0u};
    ServiceRegistry builtin;
    builtin.add(PRAENORA_TEST_ECHO_SERVICE, 0, &header_only);
    InstanceOptions options = instance_options(logs);
    options.services = &builtin;
    options.provider_services.add(PRAENORA_TEST_ECHO_SERVICE, 0, echo_table());
    auto instance = ready(options, json::array());
    EXPECT_EQ(invoke_once(*instance, 0).at("echo"), json({{"value", 7}}));

    // Without the provider service, the built-in table is served (it has no echo function).
    InstanceOptions without = instance_options(logs);
    without.services = &builtin;
    auto plain = ready(without, json::array());
    const json seen = invoke_once(*plain, 0);
    EXPECT_EQ(seen.at("found").at("test.echo@0"), true);
    EXPECT_TRUE(seen.at("echo").is_null());
}

TEST(ServiceVisibility, UndeclaredServicesAreNotServed) {
    // The provider is in the process, but this instance's step declared nothing (as a module with a
    // manifest of format 0.1): the module gets no provider service.
    static_cast<void>(echo_table());
    CapturedLogs logs;
    auto instance = ready(instance_options(logs), {"test.echo@0"});
    const json seen = invoke_once(*instance, 0);
    EXPECT_EQ(seen.at("found"), json({{"test.echo@0", false}}));
    EXPECT_TRUE(seen.at("echo").is_null());
}

TEST(ServiceVisibility, WithdrawnServicesAreNoLongerServed) {
    CapturedLogs logs;
    TestGate gate;
    ServiceRegistry builtin;
    builtin.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    InstanceOptions options = instance_options(logs);
    options.services = &builtin;
    options.provider_services.add(PRAENORA_TEST_ECHO_SERVICE, 0, echo_table());
    auto instance = ready(options, {"test.gate@0"});
    EXPECT_EQ(invoke_once(*instance, 0).at("found"),
              json({{"test.echo@0", true}, {"test.gate@0", true}}));

    // Withdrawn: neither the provider service nor the built-in one, from now on.
    instance->withdraw_services();
    const json seen = invoke_once(*instance, 1);
    EXPECT_EQ(seen.at("found"), json({{"test.echo@0", false}, {"test.gate@0", false}}));
    EXPECT_TRUE(seen.at("echo").is_null());
    EXPECT_EQ(instance->state(), StepState::ready);
}
