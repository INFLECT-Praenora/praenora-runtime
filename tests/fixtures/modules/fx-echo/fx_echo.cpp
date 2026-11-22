// fx-echo: copies inputs.in.data to outputs.out.data, or writes {} when the optional input `in`
// is absent. diagnostics() reports {"invocations":N}. The adapter writes results to the host in
// 4096-byte pieces, so a large echo arrives in several sink writes.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

class EchoModule final : public sdk::Module {
public:
    explicit EchoModule(const sdk::CreateContext&) {}

    std::string invoke(std::string_view invocation_json) override {
        ++invocations_;
        nlohmann::json invocation;
        try {
            invocation = nlohmann::json::parse(invocation_json.begin(), invocation_json.end());
        } catch (const nlohmann::json::parse_error& e) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             std::string("fx-echo: the invocation document is not JSON: ") +
                                 e.what());
        }
        nlohmann::json data = nlohmann::json::object();
        const auto inputs = invocation.find("inputs");
        if (inputs != invocation.end() && inputs->is_object()) {
            const auto in = inputs->find("in");
            if (in != inputs->end() && in->is_object() && in->contains("data")) {
                data = in->at("data");
            }
        }
        nlohmann::json result;
        result["outputs"]["out"]["data"] = std::move(data);
        return result.dump();
    }

    std::string diagnostics() const override {
        return "{\"invocations\":" + std::to_string(invocations_) + "}";
    }

private:
    std::uint64_t invocations_ = 0;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(EchoModule)
