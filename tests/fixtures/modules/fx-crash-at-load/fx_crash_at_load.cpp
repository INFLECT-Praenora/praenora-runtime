// fx-crash-at-load: a module whose library raises SIGSEGV in a static initializer, so loading it
// ends the process that loads it before its entry point is ever looked up. Only for a module host:
// in a test's own process it would end the test. Otherwise a correct module whose invoke returns
// {"outputs":{"out":{"data":{}}}}.
#include <praenora/sdk/module.hpp>

#include <csignal>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

int crash_now() {
    std::raise(SIGSEGV);
    return 0;
}

[[maybe_unused]] const int g_crashed = crash_now();

class CrashAtLoadModule final : public sdk::Module {
public:
    explicit CrashAtLoadModule(const sdk::CreateContext&) {}

    std::string invoke(std::string_view) override { return R"({"outputs":{"out":{"data":{}}}})"; }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(CrashAtLoadModule)
