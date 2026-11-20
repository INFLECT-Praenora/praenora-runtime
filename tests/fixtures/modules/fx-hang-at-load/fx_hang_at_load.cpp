// fx-hang-at-load: a module whose library never finishes loading: a static initializer sleeps for
// good, so loading it blocks the process that loads it before its entry point is ever looked up.
// Only for a module host, whose load a stop ends by killing it: in a test's own process it would
// hang the test. Otherwise a correct module whose invoke returns {"outputs":{"out":{"data":{}}}}.
#include <praenora/sdk/module.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace sdk = praenora::sdk;

int hang_now() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

[[maybe_unused]] const int g_hung = hang_now();

class HangAtLoadModule final : public sdk::Module {
public:
    explicit HangAtLoadModule(const sdk::CreateContext&) {}

    std::string invoke(std::string_view) override { return R"({"outputs":{"out":{"data":{}}}})"; }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(HangAtLoadModule)
