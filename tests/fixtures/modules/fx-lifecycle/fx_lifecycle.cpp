// fx-lifecycle: logs "lifecycle:<call>" at level info for create, initialize, invoke, finalize
// and destroy, so tests can check the order of lifecycle calls. invoke returns
// {"outputs":{"out":{"data":{}}}}.
#include <praenora/sdk/module.hpp>

#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

class LifecycleModule final : public sdk::Module {
public:
    explicit LifecycleModule(const sdk::CreateContext& context) : host_(context.host) {
        host_.log(sdk::LogLevel::info, "lifecycle:create");
    }

    ~LifecycleModule() override { host_.log(sdk::LogLevel::info, "lifecycle:destroy"); }

    void initialize(const sdk::InitializeContext&) override {
        host_.log(sdk::LogLevel::info, "lifecycle:initialize");
    }

    std::string invoke(std::string_view) override {
        host_.log(sdk::LogLevel::info, "lifecycle:invoke");
        return R"({"outputs":{"out":{"data":{}}}})";
    }

    void finalize() override { host_.log(sdk::LogLevel::info, "lifecycle:finalize"); }

private:
    sdk::Host host_;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(LifecycleModule)
