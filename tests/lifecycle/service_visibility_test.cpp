// What a module instance's get_service() serves: the provider services of its step
// (InstanceOptions::provider_services, the services its manifest declares that resolved for the
// run) first, then the runtime's built-in services, and nothing else; nothing at all once its
// services are withdrawn. The module is fx-service, which reports what it found
// (tests/fixtures/modules/fx-service); the provider service is test-echo's test.echo@0. And, for
// the message trace, the lookups each call made, when the instance records them.
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
#include <vector>

using nlohmann::json;
using praenora::StepState;
using praenora::detail::CallOutcome;
using praenora::detail::CallResult;
using praenora::detail::InstanceOptions;
using praenora::detail::ModuleInstance;
using praenora::detail::ServiceLookup;
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

// The lookups as JSON, one object per record, to compare them as a whole.
json records(const std::vector<ServiceLookup>& lookups) {
    json all = json::array();
    for (const ServiceLookup& lookup : lookups) {
        all.push_back({{"phase", lookup.phase},
                       {"tick", lookup.tick ? json(*lookup.tick) : json(nullptr)},
                       {"service", lookup.service},
                       {"major", lookup.major},
                       {"provided", lookup.provided},
                       {"count", lookup.count}});
    }
    return all;
}

json record(const char* phase, json tick, const char* service, std::uint32_t major, bool provided,
            std::uint64_t count) {
    return {{"phase", phase}, {"tick", std::move(tick)}, {"service", service},
            {"major", major}, {"provided", provided},    {"count", count}};
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

TEST(ServiceLookups, OneRecordPerServiceAndCall) {
    // fx-service looks up test.other@1 (not served) and test.echo@0 (the provider's) three times
    // in each of its calls. Each call has one record per service, in the order of its first
    // lookup, with the number of lookups it stands for; only invoke's records have a tick. In
    // invoke, fx-service's own lookup of test.echo@0 comes first and is one of four.
    CapturedLogs logs;
    InstanceOptions options = instance_options(logs);
    options.provider_services.add(PRAENORA_TEST_ECHO_SERVICE, 0, echo_table());
    options.record_lookups = true;
    ModuleInstance instance(load_fixture_module("fx-service"), std::move(options));
    const json parameters = {
        {"lookups", {"test.other@1", "test.echo@0"}},
        {"lookup_in", {"create", "initialize", "invoke", "finalize", "destroy", "diagnostics"}},
        {"repeat", 3}};
    ASSERT_EQ(instance.create(create_document("fx-service", parameters)).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    EXPECT_EQ(records(instance.take_lookups()),
              json::array({record("create", nullptr, "test.other", 1, false, 3),
                           record("create", nullptr, "test.echo", 0, true, 3),
                           record("initialize", nullptr, "test.other", 1, false, 3),
                           record("initialize", nullptr, "test.echo", 0, true, 3)}));
    EXPECT_TRUE(instance.take_lookups().empty()); // taken

    static_cast<void>(invoke_once(instance, 4));
    ASSERT_TRUE(instance.diagnostics());
    ASSERT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.destroy(), CallOutcome::succeeded);
    EXPECT_EQ(records(instance.take_lookups()),
              json::array({record("invoke", 4, "test.echo", 0, true, 4),
                           record("invoke", 4, "test.other", 1, false, 3),
                           record("diagnostics", nullptr, "test.other", 1, false, 3),
                           record("diagnostics", nullptr, "test.echo", 0, true, 3),
                           record("finalize", nullptr, "test.other", 1, false, 3),
                           record("finalize", nullptr, "test.echo", 0, true, 3),
                           record("destroy", nullptr, "test.other", 1, false, 3),
                           record("destroy", nullptr, "test.echo", 0, true, 3)}));
}

TEST(ServiceLookups, NotRecordedUnlessAsked) {
    CapturedLogs logs;
    InstanceOptions options = instance_options(logs);
    options.provider_services.add(PRAENORA_TEST_ECHO_SERVICE, 0, echo_table());
    auto instance = ready(options, {"test.other@1"});
    EXPECT_EQ(invoke_once(*instance, 0).at("found"),
              json({{"test.echo@0", true}, {"test.other@1", false}}));
    EXPECT_TRUE(instance->take_lookups().empty());
}

TEST(ServiceLookups, FailedCallKeepsItsLookups) {
    // Without test.echo@0, an invoke with require_echo fails; the lookup it made is recorded.
    CapturedLogs logs;
    InstanceOptions options = instance_options(logs);
    options.record_lookups = true;
    ModuleInstance instance(load_fixture_module("fx-service"), std::move(options));
    ASSERT_EQ(instance.create(create_document("fx-service", {{"require_echo", true}})).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const json data = {{"value", 7}};
    EXPECT_EQ(instance.invoke(invocation_document(2, &data), 2).outcome, CallOutcome::failed);
    EXPECT_EQ(records(instance.take_lookups()),
              json::array({record("invoke", 2, "test.echo", 0, false, 1)}));
}
