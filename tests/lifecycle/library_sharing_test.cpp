// Instances of one module library loaded more than once in the process, as concurrent runs and
// contract-check sessions load it: their calls wait for each other (features.concurrency
// "module"), the wait is not part of a call's deadline and a stop ends it, a call that would have
// to wait for an abandoned (unresponsive) call is refused, and the library is usable again once the
// abandoned call returns. fx-hang blocks in invoke until the test.gate service opens, and fails a
// call that starts while another call into its library is running.
#include "instance/module_instance.hpp"
#include "instance/services.hpp"
#include "loader/library_gate.hpp"

#include "instance_fixtures.hpp"

#include <praenora/runtime/status.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;
using praenora::StepState;
using praenora::detail::CallOutcome;
using praenora::detail::CallResult;
using praenora::detail::LibraryGate;
using praenora::detail::LoadedModule;
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

// Waits (up to 10 s) until `condition` holds.
bool wait_until(const std::function<bool()>& condition) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

// Creates and initializes `instance` as fx-hang with `hang`.
void start(ModuleInstance& instance, const std::string& hang, const std::string& step) {
    ASSERT_EQ(instance.create(create_document("fx-hang", {{"hang", hang}}, step)).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
}

// Calls made on threads of their own. On the way out, also after a failed assertion, it opens
// `gate`, so that fx-hang returns, and joins the threads.
class Calls {
public:
    explicit Calls(TestGate& gate) : gate_(gate) {}
    Calls(const Calls&) = delete;
    Calls& operator=(const Calls&) = delete;
    ~Calls() { join(); }

    // Invokes `instance` on a thread; `result` receives the outcome.
    void invoke(ModuleInstance& instance, CallResult& result) {
        threads_.emplace_back(
            [&instance, &result] { result = instance.invoke(invocation_document(0), 0); });
    }

    void join() {
        gate_.open();
        for (std::thread& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    TestGate& gate_;
    std::vector<std::thread> threads_;
};

// Options of an fx-hang instance for `step` that is served `services` (test.gate).
praenora::detail::InstanceOptions hang_options(CapturedLogs& logs, const std::string& step,
                                               const ServiceRegistry& services) {
    auto options = instance_options(logs, step);
    options.services = &services;
    options.caller = "step '" + step + "' of workflow test.chain@0.1.0";
    return options;
}

// Waits (up to 10 s) until no abandoned call is left in the library of `module`, then reports
// whether there is none.
bool wait_until_free(const LoadedModule& module) {
    return wait_until(
        [&module] { return !module.gate().refusal(LibraryGate::Access::exclusive).has_value(); });
}

} // namespace

TEST(Lifecycle, CallsIntoOneLibraryWaitForEachOtherAcrossLoads) {
    // Two loads of fx-hang (concurrency "module"), as two runs make them: b's invoke waits until
    // a's has returned, though each instance has its own thread and its own LoadedModule.
    static TestGate gate; // static: fx-hang keeps polling it until it opens
    gate.close();
    ServiceRegistry services;
    services.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    CapturedLogs logs;
    const auto first = load_fixture_module("fx-hang");
    const auto second = load_fixture_module("fx-hang");
    ModuleInstance a(first, hang_options(logs, "a", services));
    ModuleInstance b(second, hang_options(logs, "b", services));
    start(a, "invoke", "a");
    start(b, "none", "b");

    CallResult a_result, b_result;
    {
        Calls calls(gate);
        calls.invoke(a, a_result);
        ASSERT_TRUE(wait_until([] { return gate.polled() > 0; })); // a is in the module
        calls.invoke(b, b_result);
        EXPECT_TRUE(wait_until([&first] { return first->gate().waiting() == 1; }));
        EXPECT_EQ(b.state(), StepState::busy);
    }
    EXPECT_EQ(a_result.outcome, CallOutcome::succeeded) << a_result.error_json;
    EXPECT_EQ(b_result.outcome, CallOutcome::succeeded) << b_result.error_json;
    EXPECT_EQ(b.state(), StepState::ready);
}

TEST(Lifecycle, WaitForTheLibraryIsNotPartOfTheDeadline) {
    // b's invoke has 100 ms; it waits longer than that for a's call, and then has its own 100 ms.
    static TestGate gate;
    gate.close();
    ServiceRegistry services;
    services.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    CapturedLogs logs;
    const auto first = load_fixture_module("fx-hang");
    ModuleInstance a(first, hang_options(logs, "a", services));
    auto b_options = hang_options(logs, "b", services);
    b_options.invoke_timeout = milliseconds(100);
    ModuleInstance b(load_fixture_module("fx-hang"), b_options);
    start(a, "invoke", "a");
    start(b, "none", "b");

    CallResult a_result, b_result;
    {
        Calls calls(gate);
        calls.invoke(a, a_result);
        ASSERT_TRUE(wait_until([] { return gate.polled() > 0; }));
        calls.invoke(b, b_result);
        ASSERT_TRUE(wait_until([&first] { return first->gate().waiting() == 1; }));
        std::this_thread::sleep_for(milliseconds(300));
    }
    EXPECT_EQ(b_result.outcome, CallOutcome::succeeded) << b_result.error_json;
    EXPECT_LT(b_result.duration, milliseconds(300)); // the call itself, without the wait
}

TEST(Lifecycle, StopEndsTheWaitForTheLibrary) {
    // A stop raised while b waits for the library ends b's invoke as cancelled, without calling
    // the module; b stays ready.
    static TestGate gate;
    gate.close();
    ServiceRegistry services;
    services.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    CapturedLogs logs;
    const auto first = load_fixture_module("fx-hang");
    ModuleInstance a(first, hang_options(logs, "a", services));
    std::atomic<bool> stop{false};
    auto b_options = hang_options(logs, "b", services);
    b_options.stop_flag = &stop;
    ModuleInstance b(load_fixture_module("fx-hang"), b_options);
    start(a, "invoke", "a");
    start(b, "none", "b");

    CallResult a_result, b_result;
    {
        Calls calls(gate);
        calls.invoke(a, a_result);
        ASSERT_TRUE(wait_until([] { return gate.polled() > 0; }));
        calls.invoke(b, b_result);
        ASSERT_TRUE(wait_until([&first] { return first->gate().waiting() == 1; }));
        stop.store(true);
        EXPECT_TRUE(wait_until([&b] { return b.state() == StepState::ready; }));
        EXPECT_EQ(gate.seen_open(), 0); // a is still waiting at the closed gate
    }
    EXPECT_EQ(b_result.outcome, CallOutcome::cancelled);
    EXPECT_EQ(b_result.status, PRAENORA_STATUS_CANCELLED);
    EXPECT_FALSE(b_result.called);
    EXPECT_EQ(a_result.outcome, CallOutcome::succeeded) << a_result.error_json;
    EXPECT_TRUE(a_result.called);
}

TEST(Lifecycle, CallsBehindAnAbandonedCallAreRefusedUntilItReturns) {
    // a's invoke never returns in time: a is abandoned, and its call keeps the library. b, of
    // another load, is refused every call (it is not called and stays ready) and, when it goes
    // away, is kept rather than destroyed. Once a's call returns, the library is usable again.
    static TestGate gate; // static: the abandoned call polls it after the test's objects are gone
    gate.close();
    ServiceRegistry services;
    services.add(PRAENORA_TEST_GATE_SERVICE, 0, gate.table());
    CapturedLogs logs;
    const auto first = load_fixture_module("fx-hang");
    {
        auto b = std::make_unique<ModuleInstance>(load_fixture_module("fx-hang"),
                                                  hang_options(logs, "b", services));
        start(*b, "none", "b");
        auto a_options = hang_options(logs, "a", services);
        a_options.invoke_timeout = milliseconds(50);
        a_options.cancel_grace = milliseconds(50);
        ModuleInstance a(first, a_options);
        start(a, "invoke", "a");
        ASSERT_EQ(a.invoke(invocation_document(0), 0).outcome, CallOutcome::unresponsive);

        const CallResult refused = b->invoke(invocation_document(0), 0);
        EXPECT_EQ(refused.outcome, CallOutcome::refused);
        EXPECT_FALSE(refused.called);
        ASSERT_TRUE(refused.refused_by.has_value());
        EXPECT_EQ(refused.refused_by->call, "invoke");
        EXPECT_EQ(refused.refused_by->by, "step 'a' of workflow test.chain@0.1.0");
        EXPECT_EQ(b->state(), StepState::ready);
        EXPECT_EQ(b->finalize().outcome, CallOutcome::refused);
        EXPECT_EQ(b->state(), StepState::ready);
        EXPECT_EQ(b->diagnostics(), std::nullopt);
        EXPECT_EQ(b->destroy(), CallOutcome::refused);
        EXPECT_EQ(b->state(), StepState::ready);
        b.reset(); // kept: neither finalized nor destroyed
    }
    bool kept = false;
    for (const json& event : logs.events()) {
        const json& attributes = event.at("attributes");
        if (attributes.value("event", std::string()) == "instance.kept" &&
            attributes.value("step", std::string()) == "b") {
            kept = true;
            EXPECT_NE(event.value("body", std::string()).find("invoke by step 'a'"),
                      std::string::npos)
                << event.dump();
        }
    }
    EXPECT_TRUE(kept);

    gate.open(); // a's call returns now, and gives the library back
    ASSERT_TRUE(wait_until_free(*first));
    ModuleInstance c(load_fixture_module("fx-hang"), hang_options(logs, "c", services));
    start(c, "none", "c");
    EXPECT_EQ(c.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);
}
