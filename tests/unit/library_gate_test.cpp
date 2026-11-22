// The gate of a module library (loader/library_gate.hpp): shared and exclusive leases served in
// order of arrival, refusals behind an abandoned lease, a late return that frees the library, a
// stop that ends a wait, and one gate per loaded image.
#include "loader/library_gate.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using praenora::detail::LibraryCall;
using praenora::detail::LibraryGate;
using Access = praenora::detail::LibraryGate::Access;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

LibraryCall call_of(const std::string& step, const std::string& call = "invoke") {
    return LibraryCall{"fx-hang@0.1.0", call, "step '" + step + "' of workflow test.chain@0.1.0"};
}

// Waits (up to 10 s) until `count` requests wait at `gate`.
bool wait_for_waiting(const LibraryGate& gate, std::size_t count) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (gate.waiting() != count) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

// A request made on a thread of its own; its grant is kept once the gate answers.
class Request {
public:
    Request(std::shared_ptr<LibraryGate> gate, Access access, LibraryCall call,
            const std::atomic<bool>* stop = nullptr)
        : thread_([this, gate, access, call, stop] {
              LibraryGate::Grant grant = gate->acquire(access, call, stop);
              const std::lock_guard<std::mutex> lock(mutex_);
              grant_ = std::move(grant);
              answered_.store(true);
          }) {}
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool answered() const { return answered_.load(); }

    // Waits for the answer (up to 10 s) and returns it.
    LibraryGate::Grant grant() {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!answered_.load() && Clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(1));
        }
        if (thread_.joinable() && answered_.load()) {
            thread_.join();
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::move(grant_);
    }

private:
    std::mutex mutex_;
    LibraryGate::Grant grant_;
    std::atomic<bool> answered_{false};
    std::thread thread_;
};

} // namespace

TEST(LibraryGate, ExclusiveLeasesTakeTurns) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant first = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(first.lease, nullptr);

    Request second(gate, Access::exclusive, call_of("b"));
    ASSERT_TRUE(wait_for_waiting(*gate, 1));
    EXPECT_FALSE(second.answered());

    first.lease->release();
    const LibraryGate::Grant granted = second.grant();
    EXPECT_NE(granted.lease, nullptr);
    EXPECT_FALSE(granted.refused_by.has_value());
    EXPECT_EQ(gate->waiting(), 0u);
}

TEST(LibraryGate, SharedLeasesAreHeldTogether) {
    const auto gate = std::make_shared<LibraryGate>();
    const LibraryGate::Grant first = gate->acquire(Access::shared, call_of("a"));
    const LibraryGate::Grant second = gate->acquire(Access::shared, call_of("b"));
    EXPECT_NE(first.lease, nullptr);
    EXPECT_NE(second.lease, nullptr);
}

TEST(LibraryGate, ServesRequestsInOrderOfArrival) {
    // A shared request that arrives after a waiting exclusive one waits for it, although it would
    // fit beside the shared lease held now: a stream of shared calls cannot keep an exclusive one
    // waiting for ever.
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::shared, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    Request exclusive(gate, Access::exclusive, call_of("b"));
    ASSERT_TRUE(wait_for_waiting(*gate, 1));
    Request shared(gate, Access::shared, call_of("c"));
    ASSERT_TRUE(wait_for_waiting(*gate, 2));
    EXPECT_FALSE(exclusive.answered());
    EXPECT_FALSE(shared.answered());

    held.lease->release();
    LibraryGate::Grant second = exclusive.grant();
    ASSERT_NE(second.lease, nullptr);
    ASSERT_TRUE(wait_for_waiting(*gate, 1));
    EXPECT_FALSE(shared.answered());

    second.lease->release();
    EXPECT_NE(shared.grant().lease, nullptr);
}

TEST(LibraryGate, SharedRequestsInARowAreGrantedTogether) {
    // Two shared requests wait behind a held exclusive lease; once it is released, both are
    // granted, the second as soon as the first is.
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    Request first(gate, Access::shared, call_of("b"));
    ASSERT_TRUE(wait_for_waiting(*gate, 1));
    Request second(gate, Access::shared, call_of("c"));
    ASSERT_TRUE(wait_for_waiting(*gate, 2));

    held.lease->release();
    const LibraryGate::Grant b = first.grant();
    const LibraryGate::Grant c = second.grant(); // granted while b still holds its lease
    EXPECT_NE(b.lease, nullptr);
    EXPECT_NE(c.lease, nullptr);
    EXPECT_EQ(gate->waiting(), 0u);
}

TEST(LibraryGate, RefusesEveryRequestBehindAnAbandonedExclusiveLease) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    EXPECT_FALSE(gate->refusal(Access::exclusive).has_value());
    held.lease->abandon();

    for (const Access access : {Access::exclusive, Access::shared}) {
        const LibraryGate::Grant refused = gate->acquire(access, call_of("b"));
        EXPECT_EQ(refused.lease, nullptr);
        ASSERT_TRUE(refused.refused_by.has_value());
        EXPECT_EQ(refused.refused_by->call, "invoke");
        EXPECT_EQ(refused.refused_by->by, "step 'a' of workflow test.chain@0.1.0");
        ASSERT_TRUE(gate->refusal(access).has_value());
        EXPECT_EQ(gate->refusal(access)->module, "fx-hang@0.1.0");
    }
    EXPECT_EQ(gate->waiting(), 0u);
}

TEST(LibraryGate, AbandonedSharedLeaseRefusesOnlyExclusiveRequests) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::shared, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    held.lease->abandon();

    EXPECT_NE(gate->acquire(Access::shared, call_of("b")).lease, nullptr);
    EXPECT_FALSE(gate->refusal(Access::shared).has_value());
    const LibraryGate::Grant refused = gate->acquire(Access::exclusive, call_of("c", "self_test"));
    EXPECT_EQ(refused.lease, nullptr);
    ASSERT_TRUE(refused.refused_by.has_value());
    EXPECT_EQ(refused.refused_by->by, "step 'a' of workflow test.chain@0.1.0");
}

TEST(LibraryGate, WaitingRequestIsRefusedOnceTheHolderIsAbandoned) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    Request waiting(gate, Access::exclusive, call_of("b"));
    ASSERT_TRUE(wait_for_waiting(*gate, 1));

    held.lease->abandon();
    const LibraryGate::Grant refused = waiting.grant();
    EXPECT_EQ(refused.lease, nullptr);
    ASSERT_TRUE(refused.refused_by.has_value());
    EXPECT_EQ(refused.refused_by->by, "step 'a' of workflow test.chain@0.1.0");
    EXPECT_EQ(gate->waiting(), 0u);
}

TEST(LibraryGate, WaitingSharedRequestIsRefusedOnceAnExclusiveHolderIsAbandoned) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a", "self_test"));
    ASSERT_NE(held.lease, nullptr);
    Request waiting(gate, Access::shared, call_of("b"));
    ASSERT_TRUE(wait_for_waiting(*gate, 1));

    held.lease->abandon();
    const LibraryGate::Grant refused = waiting.grant();
    EXPECT_EQ(refused.lease, nullptr);
    ASSERT_TRUE(refused.refused_by.has_value());
    EXPECT_EQ(refused.refused_by->call, "self_test");
}

TEST(LibraryGate, AbandonedLeaseThatReturnsFreesTheLibrary) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    held.lease->abandon();
    ASSERT_TRUE(gate->refusal(Access::exclusive).has_value());

    held.lease->release(); // the abandoned call returned after all
    EXPECT_FALSE(gate->refusal(Access::exclusive).has_value());
    EXPECT_NE(gate->acquire(Access::exclusive, call_of("b")).lease, nullptr);
}

TEST(LibraryGate, AbandoningAReleasedLeaseDoesNothing) {
    // The call returned just as its caller gave up on it: the lease is already back.
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    held.lease->release();
    held.lease->abandon();
    EXPECT_FALSE(gate->refusal(Access::exclusive).has_value());
    EXPECT_NE(gate->acquire(Access::exclusive, call_of("b")).lease, nullptr);
}

TEST(LibraryGate, LeaseIsReleasedWhenItGoesAway) {
    const auto gate = std::make_shared<LibraryGate>();
    {
        const LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
        ASSERT_NE(held.lease, nullptr);
    }
    EXPECT_NE(gate->acquire(Access::exclusive, call_of("b")).lease, nullptr);
}

TEST(LibraryGate, StopEndsTheWait) {
    const auto gate = std::make_shared<LibraryGate>();
    LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
    ASSERT_NE(held.lease, nullptr);
    std::atomic<bool> stop{false};
    Request waiting(gate, Access::exclusive, call_of("b"), &stop);
    ASSERT_TRUE(wait_for_waiting(*gate, 1));

    stop.store(true);
    const LibraryGate::Grant stopped = waiting.grant();
    EXPECT_EQ(stopped.lease, nullptr);
    EXPECT_FALSE(stopped.refused_by.has_value());
    EXPECT_EQ(gate->waiting(), 0u);
}

TEST(LibraryGate, StopRaisedBeforeTheWaitStillGrantsAFreeLibrary) {
    // The stop flag only ends a wait; a library that is free is granted at once.
    const auto gate = std::make_shared<LibraryGate>();
    const std::atomic<bool> stop{true};
    EXPECT_NE(gate->acquire(Access::exclusive, call_of("a"), &stop).lease, nullptr);
}

TEST(LibraryGate, OneGatePerImage) {
    static int first_image = 0;
    static int second_image = 0;
    const auto first = LibraryGate::of(&first_image);
    EXPECT_EQ(LibraryGate::of(&first_image), first);
    EXPECT_NE(LibraryGate::of(&second_image), first);
}

TEST(LibraryGate, ImageGetsANewGateOnceNothingHoldsTheOld) {
    // A gate that nothing holds any more is gone; the image's next gate starts free.
    static int image = 0;
    std::weak_ptr<LibraryGate> old;
    {
        const auto gate = LibraryGate::of(&image);
        old = gate;
        const LibraryGate::Grant held = gate->acquire(Access::exclusive, call_of("a"));
        ASSERT_NE(held.lease, nullptr);
        held.lease->abandon();
        held.lease->release();
    }
    EXPECT_TRUE(old.expired());
    const auto renewed = LibraryGate::of(&image);
    EXPECT_FALSE(renewed->refusal(Access::exclusive).has_value());
    EXPECT_EQ(renewed->waiting(), 0u);
}

TEST(LibraryGate, DescribesACall) {
    const LibraryCall call = call_of("a");
    EXPECT_EQ(praenora::detail::to_string(call),
              "invoke by step 'a' of workflow test.chain@0.1.0 (module fx-hang@0.1.0)");
    EXPECT_EQ(praenora::detail::to_json(call),
              (nlohmann::json{{"module", "fx-hang@0.1.0"},
                              {"call", "invoke"},
                              {"by", "step 'a' of workflow test.chain@0.1.0"}}));
}
