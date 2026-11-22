// fx-slow: sleeps in invoke, then returns {"outputs":{"out":{"data":{}}}}. Parameters (defaults
// in brackets): sleep_ms, the time to sleep in invoke [100]; finalize_sleep_ms, the time to sleep
// in finalize [0]; cooperative [true]. A cooperative instance polls cancel_requested every
// millisecond while it sleeps and fails with CANCELLED once it is set; otherwise it sleeps the
// whole time, ignoring cancellation. diagnostics() reports {"invocations":N}, the invocations
// received so far. Its instances share nothing, so its manifest declares concurrency "instance".
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace sdk = praenora::sdk;

class SlowModule final : public sdk::Module {
public:
    explicit SlowModule(const sdk::CreateContext& context) : host_(context.host) {
        read_parameters(context.create_json);
    }

    std::string invoke(std::string_view) override {
        ++invocations_;
        sleep("invoke", sleep_ms_);
        return R"({"outputs":{"out":{"data":{}}}})";
    }

    void finalize() override { sleep("finalize", finalize_sleep_ms_); }

    std::string diagnostics() const override {
        return "{\"invocations\":" + std::to_string(invocations_) + "}";
    }

private:
    void sleep(const char* call, std::int64_t milliseconds) const {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(milliseconds);
        if (!cooperative_) {
            std::this_thread::sleep_until(deadline);
            return;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            if (host_.cancel_requested()) {
                const auto slept = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                throw sdk::Error(PRAENORA_STATUS_CANCELLED,
                                 std::string("fx-slow: ") + call + " cancelled after " +
                                     std::to_string(slept.count()) + " ms");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    [[noreturn]] static void invalid(const std::string& what) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "fx-slow: " + what);
    }

    void read_parameters(std::string_view create_json) {
        nlohmann::json parameters;
        try {
            const auto document = nlohmann::json::parse(create_json.begin(), create_json.end());
            if (document.is_object() && document.contains("parameters")) {
                parameters = document.at("parameters");
            }
        } catch (const nlohmann::json::exception& e) {
            invalid(std::string("the create document is not valid JSON: ") + e.what());
        }
        if (parameters.is_null()) {
            return;
        }
        if (!parameters.is_object()) {
            invalid("parameters must be an object");
        }
        sleep_ms_ = milliseconds_parameter(parameters, "sleep_ms", sleep_ms_);
        finalize_sleep_ms_ =
            milliseconds_parameter(parameters, "finalize_sleep_ms", finalize_sleep_ms_);
        if (parameters.contains("cooperative")) {
            const auto& cooperative = parameters.at("cooperative");
            if (!cooperative.is_boolean()) {
                invalid("cooperative must be a boolean");
            }
            cooperative_ = cooperative.get<bool>();
        }
    }

    static std::int64_t milliseconds_parameter(const nlohmann::json& parameters, const char* name,
                                               std::int64_t fallback) {
        if (!parameters.contains(name)) {
            return fallback;
        }
        const auto& value = parameters.at(name);
        if (!value.is_number_integer() || value.get<std::int64_t>() < 0) {
            invalid(std::string(name) + " must be a non-negative integer");
        }
        return value.get<std::int64_t>();
    }

    sdk::Host host_;
    std::int64_t sleep_ms_ = 100;
    std::int64_t finalize_sleep_ms_ = 0;
    bool cooperative_ = true;
    std::int64_t invocations_ = 0;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(SlowModule)
