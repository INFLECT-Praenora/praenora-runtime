// fx-noop: does nothing; every invoke returns {"outputs":{"out":{"data":{}}}}. The performance
// probe uses it to measure the runtime's own cost per invocation.
#include <praenora/sdk/module.hpp>

#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

class NoopModule final : public sdk::Module {
public:
    explicit NoopModule(const sdk::CreateContext&) {}

    std::string invoke(std::string_view) override { return R"({"outputs":{"out":{"data":{}}}})"; }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(NoopModule)
