// The lifecycle of module instances: call order, cleanup after failures and on destruction, state
// errors, timeouts with a cooperative and an uncooperative module, and cancellation from another
// thread. Modules that log "lifecycle:<call>" (fx-lifecycle, fx-fail, fx-create-fail) show which
// calls they received.
#include "instance/module_instance.hpp"
#include "instance/services.hpp"

#include "instance_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/sdk/status.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
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
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

using Calls = std::vector<std::string>;

// The code of the praenora::Error that `call` throws; OK if it throws none.
template <class Call> ErrorCode error_code_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error.code();
    }
    return ErrorCode::OK;
}

// Waits (up to 10 s) until a call into `instance` is running.
void wait_until_busy(const ModuleInstance& instance) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (instance.state() != StepState::busy && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(1));
    }
}

// A thread that waits until a call into `instance` is running, then `delay` more, and then
// requests cancellation.
std::thread cancel_when_busy(ModuleInstance& instance, milliseconds delay) {
    return std::thread([&instance, delay] {
        wait_until_busy(instance);
        std::this_thread::sleep_for(delay);
        instance.request_cancel();
    });
}

} // namespace

TEST(Lifecycle, CallOrder) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-lifecycle"), instance_options(logs));
    EXPECT_EQ(instance.create(create_document("fx-lifecycle")).outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.state(), StepState::created);
    EXPECT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.state(), StepState::ready);
    EXPECT_EQ(instance.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.invoke(invocation_document(1), 1).outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.state(), StepState::finalized);
    instance.destroy();
    EXPECT_EQ(instance.state(), StepState::destroyed);

    EXPECT_EQ(logs.bodies("lifecycle:"),
              (Calls{"lifecycle:create", "lifecycle:initialize", "lifecycle:invoke",
                     "lifecycle:invoke", "lifecycle:finalize", "lifecycle:destroy"}));
}

TEST(Lifecycle, InitializeFailureDestroysWithoutFinalize) {
    CapturedLogs logs;
    {
        ModuleInstance instance(load_fixture_module("fx-fail"), instance_options(logs));
        ASSERT_EQ(instance.create(create_document("fx-fail", {{"phase", "initialize"}})).outcome,
                  CallOutcome::succeeded);
        const CallResult initialized = instance.initialize(initialize_document());
        EXPECT_EQ(initialized.outcome, CallOutcome::failed);
        EXPECT_EQ(initialized.status, PRAENORA_STATUS_FAILED);
        EXPECT_EQ(json::parse(initialized.error_json).at("details").at("phase"), "initialize");
        EXPECT_EQ(instance.state(), StepState::failed);

        // The instance was never initialized: it cannot be invoked or finalized.
        EXPECT_EQ(error_code_of([&] { instance.invoke(invocation_document(0), 0); }),
                  ErrorCode::INVALID_STATE);
        EXPECT_EQ(error_code_of([&] { instance.finalize(); }), ErrorCode::INVALID_STATE);
    } // the destructor destroys it, without finalize

    EXPECT_EQ(logs.bodies("lifecycle:"),
              (Calls{"lifecycle:create", "lifecycle:initialize", "lifecycle:destroy"}));
}

TEST(Lifecycle, CreateFailureSkipsDestroy) {
    CapturedLogs logs;
    {
        ModuleInstance instance(load_fixture_module("fx-create-fail"), instance_options(logs));
        const CallResult created = instance.create(create_document("fx-create-fail"));
        EXPECT_EQ(created.outcome, CallOutcome::failed);
        EXPECT_EQ(created.status, PRAENORA_STATUS_FAILED);
        const json error = json::parse(created.error_json);
        EXPECT_EQ(error.at("code"), 11);
        EXPECT_EQ(error.at("message"), "fx-create-fail: create fails on purpose");
        EXPECT_EQ(instance.state(), StepState::failed);

        EXPECT_EQ(error_code_of([&] { instance.initialize(initialize_document()); }),
                  ErrorCode::INVALID_STATE);
        instance.destroy(); // nothing was created, so there is nothing to destroy
    }

    // fx-create-fail logs every destroy() call, even one with NULL: there was none.
    EXPECT_EQ(logs.bodies("lifecycle:"), (Calls{"lifecycle:create"}));
}

TEST(Lifecycle, InvokeBeforeInitializeIsInvalidState) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-echo"), instance_options(logs));
    // Before create.
    EXPECT_EQ(error_code_of([&] { instance.invoke(invocation_document(0), 0); }),
              ErrorCode::INVALID_STATE);
    EXPECT_EQ(error_code_of([&] { instance.initialize(initialize_document()); }),
              ErrorCode::INVALID_STATE);
    EXPECT_EQ(instance.state(), StepState::loaded);

    ASSERT_EQ(instance.create(create_document("fx-echo")).outcome, CallOutcome::succeeded);
    // After create, before initialize.
    try {
        instance.invoke(invocation_document(0), 0);
        ADD_FAILURE() << "invoke was accepted before initialize";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::INVALID_STATE);
        EXPECT_NE(std::string(error.what()).find("created"), std::string::npos) << error.what();
    }
    EXPECT_EQ(error_code_of([&] { instance.create(create_document("fx-echo")); }),
              ErrorCode::INVALID_STATE);
    EXPECT_EQ(error_code_of([&] { instance.finalize(); }), ErrorCode::INVALID_STATE);
    EXPECT_EQ(instance.state(), StepState::created);
}

TEST(Cancel, StopFlagStopsCallsAsTheyStart) {
    // The caller's own stop flag closes the gap between its last look at the flag and the start
    // of the next call: a call that starts while it is set is stopped at once.
    std::atomic<bool> stop{false};
    CapturedLogs logs;
    auto options = instance_options(logs);
    options.stop_flag = &stop;
    ModuleInstance instance(load_fixture_module("fx-slow"), options);
    ASSERT_EQ(
        instance
            .create(create_document(
                "fx-slow", {{"sleep_ms", 5000}, {"finalize_sleep_ms", 50}, {"cooperative", true}}))
            .outcome,
        CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);

    stop.store(true);
    const CallResult invoked = instance.invoke(invocation_document(0), 0);
    EXPECT_EQ(invoked.outcome, CallOutcome::cancelled) << invoked.error_json;
    EXPECT_EQ(invoked.status, PRAENORA_STATUS_CANCELLED);
    EXPECT_LT(invoked.duration, milliseconds(1000));

    // Teardown still runs normally while the flag stays set: finalize starts clear.
    const CallResult finalized = instance.finalize();
    EXPECT_EQ(finalized.outcome, CallOutcome::succeeded) << finalized.error_json;
    EXPECT_GE(finalized.duration, milliseconds(50));
}

TEST(Lifecycle, ConcurrentControlCallsAreRefused) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-slow"), instance_options(logs));
    ASSERT_EQ(instance.create(create_document("fx-slow", {{"sleep_ms", 300}})).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);

    // While invoke runs on this thread, another thread's control calls are refused.
    ErrorCode invoke_code = ErrorCode::OK;
    ErrorCode finalize_code = ErrorCode::OK;
    bool diagnostics_returned = true;
    std::thread other([&] {
        wait_until_busy(instance);
        invoke_code = error_code_of([&] { instance.invoke(invocation_document(1), 1); });
        finalize_code = error_code_of([&] { instance.finalize(); });
        diagnostics_returned = instance.diagnostics().has_value();
        instance.destroy(); // refused too: it does nothing but log a warning
    });
    const CallResult result = instance.invoke(invocation_document(0), 0);
    other.join();

    EXPECT_EQ(invoke_code, ErrorCode::INVALID_STATE);
    EXPECT_EQ(finalize_code, ErrorCode::INVALID_STATE);
    EXPECT_FALSE(diagnostics_returned);
    EXPECT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    EXPECT_EQ(instance.state(), StepState::ready);
    bool warned = false;
    for (const auto& event : logs.events()) {
        warned =
            warned || (event.at("severity_text") == "WARN" &&
                       event.at("body").get<std::string>().find("destroy") != std::string::npos);
    }
    EXPECT_TRUE(warned);
}

TEST(Lifecycle, DestructorCleansUpWithoutExplicitCalls) {
    CapturedLogs ready_logs;
    {
        ModuleInstance instance(load_fixture_module("fx-lifecycle"), instance_options(ready_logs));
        ASSERT_EQ(instance.create(create_document("fx-lifecycle")).outcome, CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        ASSERT_EQ(instance.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);
    }
    EXPECT_EQ(ready_logs.bodies("lifecycle:"),
              (Calls{"lifecycle:create", "lifecycle:initialize", "lifecycle:invoke",
                     "lifecycle:finalize", "lifecycle:destroy"}));

    // Created but never initialized: destroyed without finalize.
    CapturedLogs created_logs;
    {
        ModuleInstance instance(load_fixture_module("fx-lifecycle"),
                                instance_options(created_logs));
        ASSERT_EQ(instance.create(create_document("fx-lifecycle")).outcome, CallOutcome::succeeded);
    }
    EXPECT_EQ(created_logs.bodies("lifecycle:"), (Calls{"lifecycle:create", "lifecycle:destroy"}));

    // Never created: nothing to call.
    CapturedLogs unused_logs;
    {
        ModuleInstance instance(load_fixture_module("fx-lifecycle"), instance_options(unused_logs));
    }
    EXPECT_TRUE(unused_logs.bodies("lifecycle:").empty());
}

TEST(Timeout, CooperativeModuleTimesOut) {
    CapturedLogs logs;
    auto options = instance_options(logs);
    options.invoke_timeout = milliseconds(100);
    options.cancel_grace = std::chrono::seconds(1);
    ModuleInstance instance(load_fixture_module("fx-slow"), options);
    ASSERT_EQ(
        instance.create(create_document("fx-slow", {{"sleep_ms", 5000}, {"cooperative", true}}))
            .outcome,
        CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);

    const auto start = Clock::now();
    const CallResult result = instance.invoke(invocation_document(0), 0);
    const auto elapsed = Clock::now() - start;
    EXPECT_EQ(result.outcome, CallOutcome::timed_out);
    // fx-slow saw the cancel flag raised at the deadline and gave up.
    EXPECT_EQ(result.status, PRAENORA_STATUS_CANCELLED) << result.error_json;
    EXPECT_GE(result.duration, milliseconds(100));
    EXPECT_LT(result.duration, milliseconds(1500));
    EXPECT_LT(elapsed, milliseconds(1500));
    // The call returned, so the instance is still usable and is finalized and destroyed normally.
    EXPECT_EQ(instance.state(), StepState::ready);
    EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
}

TEST(Timeout, UncooperativeModuleBecomesUnresponsive) {
    // Static: the detached thread polls the gate after the instance is gone. Closed afresh on
    // every run of the test (fx-hang stops polling once it has seen the gate open).
    static TestGate gate;
    gate.close();
    CapturedLogs logs;
    ServiceRegistry services;
    services.add("test.gate", 0, gate.table());
    auto options = instance_options(logs);
    options.invoke_timeout = milliseconds(50);
    options.cancel_grace = milliseconds(50);
    options.services = &services;
    const auto module = load_fixture_module("fx-hang");
    {
        ModuleInstance instance(module, options);
        ASSERT_EQ(instance.create(create_document("fx-hang")).outcome, CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);

        const CallResult result = instance.invoke(invocation_document(0), 0);
        EXPECT_EQ(result.outcome, CallOutcome::unresponsive);
        EXPECT_GE(result.duration, milliseconds(100));
        EXPECT_EQ(instance.state(), StepState::unresponsive);
        EXPECT_TRUE(module->pinned());

        // An unresponsive instance takes no more calls and is never finalized or destroyed.
        EXPECT_EQ(error_code_of([&] { instance.invoke(invocation_document(1), 1); }),
                  ErrorCode::INVALID_STATE);
        EXPECT_EQ(error_code_of([&] { instance.finalize(); }), ErrorCode::INVALID_STATE);
        EXPECT_FALSE(instance.diagnostics().has_value());
        instance.destroy();
        EXPECT_EQ(instance.state(), StepState::unresponsive);
    } // the destructor neither waits for nor joins the detached thread

    // Release the detached thread, so it finishes before the test ends.
    gate.open();
    // The abandoned call returns and gives its library back, so the next test can load it.
    EXPECT_TRUE(praenora::test::wait_until_loadable("fx-hang"));
    EXPECT_GT(gate.seen_open(), 0);
    std::this_thread::sleep_for(milliseconds(20)); // the thread exits right after the call returns
    EXPECT_TRUE(module->pinned());
}

TEST(Cancel, RequestCancelFromOtherThread) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-slow"), instance_options(logs));
    ASSERT_EQ(
        instance.create(create_document("fx-slow", {{"sleep_ms", 5000}, {"cooperative", true}}))
            .outcome,
        CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);

    std::thread canceller = cancel_when_busy(instance, milliseconds(50));
    const CallResult result = instance.invoke(invocation_document(0), 0);
    canceller.join();

    EXPECT_EQ(result.outcome, CallOutcome::cancelled);
    EXPECT_EQ(result.status, PRAENORA_STATUS_CANCELLED) << result.error_json;
    // The canceller's 50 ms start when it sees the instance busy, just before the call is posted.
    EXPECT_GE(result.duration, milliseconds(40));
    EXPECT_LT(result.duration, milliseconds(1000));
    EXPECT_EQ(instance.state(), StepState::ready);
}

TEST(Cancel, StopAffectsOnlyTheCallInFlight) {
    CapturedLogs logs;
    ModuleInstance instance(load_fixture_module("fx-slow"), instance_options(logs, "s"));
    ASSERT_EQ(
        instance
            .create(create_document(
                "fx-slow", {{"sleep_ms", 5000}, {"finalize_sleep_ms", 50}, {"cooperative", true}}))
            .outcome,
        CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);

    std::thread canceller = cancel_when_busy(instance, milliseconds(20));
    const CallResult invoked = instance.invoke(invocation_document(0), 0);
    canceller.join();
    EXPECT_EQ(invoked.outcome, CallOutcome::cancelled) << invoked.error_json;

    // The stop was for that invocation only: finalize, which polls cancel_requested() all through
    // its 50 ms, starts with the flag clear and runs to its end.
    const CallResult finalized = instance.finalize();
    EXPECT_EQ(finalized.outcome, CallOutcome::succeeded) << finalized.error_json;
    EXPECT_GE(finalized.duration, milliseconds(50));
    EXPECT_EQ(instance.state(), StepState::finalized);

    // A stop requested while no call is in flight reaches no call.
    ModuleInstance idle(load_fixture_module("fx-slow"), instance_options(logs, "t"));
    ASSERT_EQ(
        idle.create(create_document("fx-slow", {{"sleep_ms", 30}, {"cooperative", true}}, "t"))
            .outcome,
        CallOutcome::succeeded);
    ASSERT_EQ(idle.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    idle.request_cancel();
    const CallResult result = idle.invoke(invocation_document(0), 0);
    EXPECT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    EXPECT_GE(result.duration, milliseconds(30));
}
