// Module instances through the real loader: documents in and out, sinks, module errors, the size
// limit on outputs, log routing, and runtime services through the host API.
#include "instance/module_instance.hpp"
#include "instance/services.hpp"

#include "instance_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/sdk/status.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::StepState;
using praenora::detail::CallOutcome;
using praenora::detail::CallResult;
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

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Creates and initializes the instance; both calls must succeed.
void start(ModuleInstance& instance, const std::string& create_json) {
    const CallResult created = instance.create(create_json);
    ASSERT_EQ(created.outcome, CallOutcome::succeeded) << created.error_json;
    const CallResult initialized = instance.initialize(initialize_document());
    ASSERT_EQ(initialized.outcome, CallOutcome::succeeded) << initialized.error_json;
    ASSERT_EQ(instance.state(), StepState::ready);
}

} // namespace

TEST(Instance, EchoRoundTrip) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-echo"), instance_options(logs));
    EXPECT_EQ(instance.state(), StepState::loaded);
    EXPECT_EQ(instance.module().manifest().name, "fx-echo");

    const CallResult created = instance.create(create_document("fx-echo"));
    EXPECT_EQ(created.outcome, CallOutcome::succeeded) << created.error_json;
    EXPECT_EQ(created.status, PRAENORA_STATUS_OK);
    EXPECT_EQ(instance.state(), StepState::created);
    const CallResult initialized = instance.initialize(initialize_document());
    EXPECT_EQ(initialized.outcome, CallOutcome::succeeded) << initialized.error_json;
    EXPECT_EQ(instance.state(), StepState::ready);

    const json data = {{"x", 1}, {"text", "h\xC3\xA9llo"}, {"list", {1.5, nullptr, true}}};
    const CallResult result = instance.invoke(invocation_document(0, &data), 0);
    ASSERT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    EXPECT_EQ(result.status, PRAENORA_STATUS_OK);
    EXPECT_EQ(json::parse(result.output), (json{{"outputs", {{"out", {{"data", data}}}}}}));
    EXPECT_TRUE(result.error_json.empty());
    EXPECT_GT(result.duration.count(), 0);
    EXPECT_EQ(instance.state(), StepState::ready);

    // diagnostics() runs between calls, on the instance's thread.
    const auto diagnostics = instance.diagnostics();
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_EQ(json::parse(*diagnostics), (json{{"invocations", 1}}));
    EXPECT_EQ(instance.state(), StepState::ready);

    EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.state(), StepState::finalized);
    instance.destroy();
    EXPECT_EQ(instance.state(), StepState::destroyed);
    instance.destroy(); // at most once: a second call does nothing
    EXPECT_EQ(instance.state(), StepState::destroyed);
}

TEST(Instance, SinkMultiWriteConcatenates) {
    // The C++ adapter writes results in 4096-byte pieces, so this result reaches the host in
    // several writes, which it must join without loss or reordering.
    std::string text;
    for (int i = 0; i < 12000; ++i) {
        text += static_cast<char>('a' + i % 26);
    }
    json numbers = json::array();
    for (int i = 0; i < 1000; ++i) {
        numbers.push_back(i * 7);
    }
    const json data = {{"numbers", numbers}, {"text", text}};
    ASSERT_GT(data.dump().size(), 10u * 1024u);

    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-echo"), instance_options(logs));
    start(instance, create_document("fx-echo"));
    const CallResult result = instance.invoke(invocation_document(0, &data), 0);
    ASSERT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    // fx-echo serializes the data the way nlohmann/json does, so the bytes are predictable.
    EXPECT_EQ(result.output, R"({"outputs":{"out":{"data":)" + data.dump() + "}}}");
}

TEST(Instance, ModuleErrorJsonIsCaptured) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-fail"), instance_options(logs));
    start(instance, create_document("fx-fail", {{"mode", "status"}, {"status", 11}}));

    const CallResult result = instance.invoke(invocation_document(0), 0);
    EXPECT_EQ(result.outcome, CallOutcome::failed);
    EXPECT_EQ(result.status, PRAENORA_STATUS_FAILED);
    ASSERT_FALSE(result.error_json.empty());
    const json error = json::parse(result.error_json);
    EXPECT_EQ(error.at("code"), 11);
    EXPECT_FALSE(error.at("message").get<std::string>().empty());
    EXPECT_EQ(error.at("details").at("phase"), "invoke");
    // A failed invocation leaves the instance initialized, so it is still finalized at the end.
    EXPECT_EQ(instance.state(), StepState::ready);
}

TEST(Instance, ThrowingModuleMapsToInternal) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-fail"), instance_options(logs));
    start(instance, create_document("fx-fail", {{"mode", "throw"}}));

    const CallResult result = instance.invoke(invocation_document(0), 0);
    EXPECT_EQ(result.outcome, CallOutcome::failed);
    EXPECT_EQ(result.status, PRAENORA_STATUS_INTERNAL);
    const json error = json::parse(result.error_json);
    EXPECT_EQ(error.at("code"), PRAENORA_STATUS_INTERNAL);
    EXPECT_TRUE(contains(error.at("message").get<std::string>(), "fx-fail: injected failure"))
        << result.error_json;
}

TEST(Instance, OversizedResultIsRefused) {
    CapturedLogs logs;
    auto options = instance_options(logs);
    options.max_output_bytes = 16;
    ModuleInstance instance(load_fixture_module("fx-echo"), options);
    start(instance, create_document("fx-echo"));

    const json data = {{"text", "far more than sixteen bytes of data"}};
    const CallResult result = instance.invoke(invocation_document(0, &data), 0);
    EXPECT_EQ(result.outcome, CallOutcome::failed);
    EXPECT_EQ(result.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_LE(result.output.size(), 16u);
    EXPECT_LE(result.error_json.size(), 16u);
    EXPECT_EQ(instance.state(), StepState::ready);
}

TEST(Instance, LogsRoutedWithAttributes) {
    const std::string fingerprint =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CapturedLogs logs;
    auto options = instance_options(logs, "s");
    options.run_fingerprint = fingerprint;
    ModuleInstance instance(load_fixture_module("fx-lifecycle"), options);
    start(instance, create_document("fx-lifecycle"));
    ASSERT_EQ(instance.invoke(invocation_document(1), 1).outcome, CallOutcome::succeeded);

    const json* invoked = nullptr;
    const json* created = nullptr;
    const auto events = logs.events();
    for (const auto& event : events) {
        if (event.at("body") == "lifecycle:invoke") {
            invoked = &event;
        } else if (event.at("body") == "lifecycle:create") {
            created = &event;
        }
    }
    ASSERT_NE(invoked, nullptr);
    const json& attributes = invoked->at("attributes");
    EXPECT_EQ(attributes.at("praenora.step"), "s");
    EXPECT_EQ(attributes.at("praenora.tick"), 1);
    EXPECT_EQ(attributes.at("praenora.module"), "fx-lifecycle@0.1.0");
    EXPECT_EQ(attributes.at("praenora.run.fingerprint"), fingerprint);
    EXPECT_EQ(attributes.at("logger"), "module.s");
    EXPECT_EQ(invoked->at("severity_text"), "INFO");

    // Outside an invocation there is no tick.
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->at("attributes").at("praenora.step"), "s");
    EXPECT_EQ(created->at("attributes").at("praenora.run.fingerprint"), fingerprint);
    EXPECT_FALSE(created->at("attributes").contains("praenora.tick"));

    // Without a run fingerprint, the attribute is left out.
    CapturedLogs plain_logs;
    ModuleInstance plain(load_fixture_module("fx-lifecycle"), instance_options(plain_logs));
    start(plain, create_document("fx-lifecycle"));
    const auto plain_events = plain_logs.events();
    ASSERT_FALSE(plain_events.empty());
    for (const auto& event : plain_events) {
        EXPECT_FALSE(event.at("attributes").contains("praenora.run.fingerprint")) << event.dump();
    }
}

TEST(Services, LookupReturnsRegisteredTable) {
    static TestGate gate;
    gate.open();
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    EXPECT_EQ(services.find("test.gate", 0), gate.table());

    // Modules reach it through the host API: fx-hang finds the gate open and returns at once.
    CapturedLogs logs;
    auto options = instance_options(logs);
    options.services = &services;
    ModuleInstance instance(load_fixture_module("fx-hang"), options);
    start(instance, create_document("fx-hang"));
    const CallResult result = instance.invoke(invocation_document(0), 0);
    EXPECT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    EXPECT_GT(gate.seen_open(), 0);
}

TEST(Services, UnknownServiceReturnsNull) {
    static TestGate gate;
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    EXPECT_EQ(services.find("test.gate", 1), nullptr); // another major version
    EXPECT_EQ(services.find("test.gates", 0), nullptr);
    EXPECT_EQ(services.find("", 0), nullptr);

    // A module asking for a service the host does not have gets NULL: fx-hang then fails with
    // UNSUPPORTED, with and without a registry.
    for (const bool with_registry : {true, false}) {
        SCOPED_TRACE(with_registry);
        const ServiceRegistry empty;
        CapturedLogs logs;
        auto options = instance_options(logs);
        options.services = with_registry ? &empty : nullptr;
        ModuleInstance instance(load_fixture_module("fx-hang"), options);
        start(instance, create_document("fx-hang"));
        const CallResult result = instance.invoke(invocation_document(0), 0);
        EXPECT_EQ(result.outcome, CallOutcome::failed);
        EXPECT_EQ(result.status, PRAENORA_STATUS_UNSUPPORTED) << result.error_json;
    }
}

TEST(Services, WithdrawnServicesAreNoLongerServed) {
    // An instance whose services are withdrawn (one the engine keeps, never to be called again,
    // while the registry may go away) gets NULL from get_service() from then on, however long the
    // registry lives. fx-hang finds no gate and fails with UNSUPPORTED instead of returning.
    static TestGate gate;
    gate.open();
    CapturedLogs logs;
    auto options = instance_options(logs);
    {
        ServiceRegistry services;
        services.add("test.gate", 0, gate.table());
        options.services = &services;
        ModuleInstance instance(load_fixture_module("fx-hang"), options);
        start(instance, create_document("fx-hang"));
        ASSERT_EQ(instance.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);

        instance.withdraw_services();
        const CallResult result = instance.invoke(invocation_document(1), 1);
        EXPECT_EQ(result.outcome, CallOutcome::failed);
        EXPECT_EQ(result.status, PRAENORA_STATUS_UNSUPPORTED) << result.error_json;
        instance.withdraw_services(); // twice is harmless
    }
}

TEST(Services, RegistrationIsChecked) {
    static TestGate gate;
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    services.add("test.gate", 1, gate.table()); // another major is another service
    EXPECT_EQ(services.find("test.gate", 1), gate.table());
    try {
        services.add("test.gate", 0, gate.table());
        ADD_FAILURE() << "a duplicate registration was accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
    }
    try {
        services.add("test.none", 0, nullptr);
        ADD_FAILURE() << "a NULL table was accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT);
    }
}
