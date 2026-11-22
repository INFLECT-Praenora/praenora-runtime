// fx-deep-stack-at-load: a module whose library uses 2 MiB of stack in a static initializer, more
// than a thread's default stack holds on macOS (512 KiB), and less than a program's main thread
// has (8 MiB unless the stack's resource limit says otherwise). It loads where loading runs on a
// stack that large, and crashes the process that loads it elsewhere (its stack overflows). A
// correct module otherwise, whose invoke returns {"outputs":{"out":{"data":{"stack":<n>}}}}, n
// being what the static initializer computed.
#include <praenora/sdk/module.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

constexpr std::size_t kStackBytes = std::size_t{2} << 20;

// Fills a local array of kStackBytes, from its top (where the stack grows from) down, so that a
// stack too small for it is overrun at its guard page, and returns a sum the compiler cannot drop.
int use_stack() {
    volatile unsigned char buffer[kStackBytes];
    for (std::size_t i = kStackBytes; i > 0; i -= 1024) {
        buffer[i - 1] = static_cast<unsigned char>(i >> 10);
    }
    int sum = 0;
    for (std::size_t i = kStackBytes; i > 0; i -= 1024) {
        sum += buffer[i - 1];
    }
    return sum;
}

const int g_stack = use_stack();

class DeepStackAtLoadModule final : public sdk::Module {
public:
    explicit DeepStackAtLoadModule(const sdk::CreateContext&) {}

    std::string invoke(std::string_view) override {
        return R"({"outputs":{"out":{"data":{"stack":)" + std::to_string(g_stack) + "}}}}";
    }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(DeepStackAtLoadModule)
