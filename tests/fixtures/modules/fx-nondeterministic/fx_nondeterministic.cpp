// fx-nondeterministic: answers every invocation with
// {"outputs":{"out":{"data":{"invocations":N}}}}, where N counts the invocations of all its
// instances since the library was loaded. The counter is static and no instance resets it, so a
// repeated run gives other results: of the contract checks, invoke.determinism fails,
// invoke.expected_outputs (its vectors expect no outputs) and self_test (it has none) are skipped,
// and the others pass (unload is skipped where PRAENORA_DEBUG_KEEP_LIBRARIES=1 keeps libraries
// loaded). An invocation document that is not JSON is refused with INVALID_ARGUMENT. Its
// vectors.json drives it through two ticks.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

// Shared by every instance; never reset.
std::atomic<std::uint64_t> invocations{0};

class NondeterministicModule final : public sdk::Module {
public:
    explicit NondeterministicModule(const sdk::CreateContext&) {}

    std::string invoke(std::string_view invocation_json) override {
        if (!nlohmann::json::accept(invocation_json.begin(), invocation_json.end())) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             "fx-nondeterministic: the invocation document is not JSON");
        }
        const std::uint64_t count = ++invocations;
        return R"({"outputs":{"out":{"data":{"invocations":)" + std::to_string(count) + "}}}}";
    }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(NondeterministicModule)
