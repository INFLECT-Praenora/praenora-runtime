// fx-hang: blocks, ignoring cancellation, until the test-only service test.gate (major 0) reports
// open. Its parameter `hang` says where it blocks: in "invoke" (the default), which then returns
// {"outputs":{"out":{"data":{}}}}; in "finalize"; in "destroy" (the instance's destructor); or
// "none", which never blocks. Without that service invoke fails with UNSUPPORTED when it would
// block, and finalize and destroy return at once; with the parameter `without_gate` "block"
// (default "fail") they block for good instead, never looking at cancellation (a module host offers
// no test.gate). Lifecycle and engine tests use it to make an instance unresponsive and then
// release its thread. Its manifest declares diagnostics (the adapter's default, {}), so tests can
// check that nobody asks an unresponsive instance for them, and the default concurrency "module",
// which it checks: an invoke or finalize that starts while another invoke, finalize or destroy of
// the library is running fails with FAILED ("entered while another call into the library was
// running"), so tests see calls that overlap.
#include <praenora/sdk/module.hpp>

#include "test_gate.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace sdk = praenora::sdk;

// The calls into the library running now, across all its instances.
std::atomic<int> g_running{0};

// Counts a call as running for as long as it lasts, and tells whether another one was running
// when it started.
class Running {
public:
    Running() noexcept : overlapped_(g_running.fetch_add(1) != 0) {}
    ~Running() { g_running.fetch_sub(1); }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;

    // Fails the call when it overlapped another one.
    void check(const char* call) const {
        if (overlapped_) {
            throw sdk::Error(PRAENORA_STATUS_FAILED,
                             std::string("fx-hang: ") + call +
                                 " entered while another call into the library was running");
        }
    }

private:
    bool overlapped_;
};

class HangModule final : public sdk::Module {
public:
    explicit HangModule(const sdk::CreateContext& context) : host_(context.host) {
        read_parameters(context.create_json);
    }

    ~HangModule() override {
        const Running running;
        if (hang_ == "destroy" && !wait_for_gate()) {
            block_without_gate();
        }
    }

    std::string invoke(std::string_view) override {
        const Running running;
        running.check("invoke");
        if (hang_ == "invoke" && !wait_for_gate()) {
            block_without_gate();
            throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED,
                             "fx-hang: the host offers no test.gate service (major 0)");
        }
        return R"({"outputs":{"out":{"data":{}}}})";
    }

    void finalize() override {
        const Running running;
        running.check("finalize");
        if (hang_ == "finalize" && !wait_for_gate()) {
            block_without_gate();
        }
    }

private:
    // With without_gate "block": never returns, never looks at cancellation.
    void block_without_gate() const {
        while (block_without_gate_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Blocks until the gate is open; false at once when the host offers no gate.
    bool wait_for_gate() const noexcept {
        const auto* gate =
            static_cast<const praenora_test_gate_v0*>(host_.service(PRAENORA_TEST_GATE_SERVICE, 0));
        if (gate == nullptr || gate->header.abi_major != 0 ||
            gate->header.struct_size <
                offsetof(praenora_test_gate_v0, is_open) + sizeof(gate->is_open) ||
            gate->is_open == nullptr) {
            return false;
        }
        while (gate->is_open(gate->context) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    void read_parameters(std::string_view create_json) {
        nlohmann::json document;
        try {
            document = nlohmann::json::parse(create_json.begin(), create_json.end());
        } catch (const nlohmann::json::exception& e) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             std::string("fx-hang: the create document is not valid JSON: ") +
                                 e.what());
        }
        const auto parameters = document.find("parameters");
        if (parameters == document.end() || !parameters->is_object()) {
            return;
        }
        if (parameters->contains("without_gate")) {
            const nlohmann::json& without_gate = parameters->at("without_gate");
            if (without_gate != "fail" && without_gate != "block") {
                throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                                 "fx-hang: without_gate must be fail or block");
            }
            block_without_gate_ = without_gate == "block";
        }
        if (!parameters->contains("hang")) {
            return;
        }
        const nlohmann::json& hang = parameters->at("hang");
        if (!hang.is_string() ||
            (hang != "invoke" && hang != "finalize" && hang != "destroy" && hang != "none")) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             "fx-hang: hang must be invoke, finalize, destroy or none");
        }
        hang_ = hang.get<std::string>();
    }

    sdk::Host host_;
    std::string hang_ = "invoke";
    bool block_without_gate_ = false;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(HangModule)
