// fx-fail: fails on purpose, as its parameters say (defaults in brackets):
//   phase              create | initialize | invoke | finalize                    [invoke]
//   at_invocation      with phase invoke, the invocation that fails, from 1       [1]
//   mode               status: throw sdk::Error(status)    throw: a std::runtime_error
//                      bad_alloc: a std::bad_alloc         the result modes below [status]
//   status             the status of mode status                                 [11, FAILED]
//   malformed_bytes    the size of the malformed_json result (see below)         [0]
//   diagnostics_bytes  the least size of what diagnostics() reports              [0]
// The result modes apply to phase invoke only; the call succeeds but its result document is
// malformed_json (not JSON: {"outputs":{"out":{"data": and, when malformed_bytes is larger than
// that, a string that never closes, "xxx..., up to malformed_bytes bytes in all), schema_violation
// (out.data is a string, not an object), unknown_port (an extra output port),
// missing_required_output (no out) or no_outputs ({"outputs":{},"diagnostics":{"invocations":N}},
// which is valid for capability test.quiet, whose out is optional). Other invocations return
// {"outputs":{"out":{"data":{"invocations":N}}}}. diagnostics() reports {"invocations":N}, the
// invocations received so far, padded with a "padding" string to diagnostics_bytes bytes.
// Like fx-lifecycle, it logs "lifecycle:<call>" at level info for every call it receives (create,
// initialize, invoke, finalize; destroy from its destructor, while the host API is still valid),
// so tests can check which calls the host makes around a failure.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

bool is_one_of(const std::string& value, std::initializer_list<const char*> allowed) {
    for (const char* candidate : allowed) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

class FailModule final : public sdk::Module {
public:
    explicit FailModule(const sdk::CreateContext& context) : host_(context.host) {
        host_.log(sdk::LogLevel::info, "lifecycle:create");
        read_parameters(context.create_json);
        if (phase_ == "create") {
            fail();
        }
    }

    ~FailModule() override { host_.log(sdk::LogLevel::info, "lifecycle:destroy"); }

    void initialize(const sdk::InitializeContext&) override {
        host_.log(sdk::LogLevel::info, "lifecycle:initialize");
        if (phase_ == "initialize") {
            fail();
        }
    }

    std::string invoke(std::string_view) override {
        host_.log(sdk::LogLevel::info, "lifecycle:invoke");
        ++invocations_;
        if (phase_ == "invoke" && invocations_ == at_invocation_) {
            if (mode_ == "malformed_json") {
                std::string text = R"({"outputs":{"out":{"data":)";
                if (malformed_bytes_ > text.size()) {
                    text += '"';
                    text.append(malformed_bytes_ - text.size(), 'x');
                }
                return text;
            }
            if (mode_ == "schema_violation") {
                return R"({"outputs":{"out":{"data":"fx-fail: not an object"}}})";
            }
            if (mode_ == "unknown_port") {
                return R"({"outputs":{"out":{"data":{}},"undeclared":{"data":{}}}})";
            }
            if (mode_ == "missing_required_output") {
                return R"({"outputs":{}})";
            }
            if (mode_ == "no_outputs") {
                return R"({"outputs":{},"diagnostics":{"invocations":)" +
                       std::to_string(invocations_) + "}}";
            }
            fail();
        }
        return R"({"outputs":{"out":{"data":{"invocations":)" + std::to_string(invocations_) +
               "}}}}";
    }

    void finalize() override {
        host_.log(sdk::LogLevel::info, "lifecycle:finalize");
        if (phase_ == "finalize") {
            fail();
        }
    }

    std::string diagnostics() const override {
        std::string text = "{\"invocations\":" + std::to_string(invocations_);
        const std::string padding = ",\"padding\":\"";
        if (diagnostics_bytes_ > text.size() + padding.size() + 2) {
            text += padding;
            text.append(diagnostics_bytes_ - text.size() - 2, 'x');
            text += '"';
        }
        return text + "}";
    }

private:
    [[noreturn]] void fail() const {
        std::string message = "fx-fail: injected failure in " + phase_;
        if (phase_ == "invoke") {
            message += " at invocation " + std::to_string(invocations_);
        }
        if (mode_ == "throw") {
            throw std::runtime_error(message);
        }
        if (mode_ == "bad_alloc") {
            throw std::bad_alloc();
        }
        throw sdk::Error(status_, message, "{\"phase\":\"" + phase_ + "\"}");
    }

    [[noreturn]] static void invalid(const std::string& what) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "fx-fail: " + what);
    }

    // A size in bytes: an integer from 0 to 16 MiB; 0 when absent.
    static std::size_t size_parameter(const nlohmann::json& parameters, const char* name) {
        if (!parameters.contains(name)) {
            return 0;
        }
        const auto& value = parameters.at(name);
        if (!value.is_number_integer() || value.get<std::int64_t>() < 0 ||
            value.get<std::int64_t>() > (std::int64_t{1} << 24)) {
            invalid(std::string(name) + " must be an integer from 0 to 16777216");
        }
        return static_cast<std::size_t>(value.get<std::int64_t>());
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
        if (parameters.contains("phase")) {
            const auto& phase = parameters.at("phase");
            if (!phase.is_string() || !is_one_of(phase.get<std::string>(),
                                                 {"create", "initialize", "invoke", "finalize"})) {
                invalid("phase must be create, initialize, invoke or finalize");
            }
            phase_ = phase.get<std::string>();
        }
        if (parameters.contains("at_invocation")) {
            const auto& at = parameters.at("at_invocation");
            if (!at.is_number_integer() || at.get<std::int64_t>() < 1) {
                invalid("at_invocation must be an integer of at least 1");
            }
            at_invocation_ = at.get<std::uint64_t>();
        }
        if (parameters.contains("mode")) {
            const auto& mode = parameters.at("mode");
            if (!mode.is_string() ||
                !is_one_of(mode.get<std::string>(),
                           {"status", "throw", "bad_alloc", "malformed_json", "schema_violation",
                            "unknown_port", "missing_required_output", "no_outputs"})) {
                invalid("unknown mode");
            }
            mode_ = mode.get<std::string>();
        }
        if (parameters.contains("status")) {
            const auto& status = parameters.at("status");
            if (!status.is_number_integer() ||
                status.get<std::int64_t>() < std::numeric_limits<praenora_status>::min() ||
                status.get<std::int64_t>() > std::numeric_limits<praenora_status>::max()) {
                invalid("status must be a 32-bit integer");
            }
            status_ = static_cast<praenora_status>(status.get<std::int64_t>());
        }
        malformed_bytes_ = size_parameter(parameters, "malformed_bytes");
        diagnostics_bytes_ = size_parameter(parameters, "diagnostics_bytes");
        if (phase_ != "invoke" &&
            is_one_of(mode_, {"malformed_json", "schema_violation", "unknown_port",
                              "missing_required_output", "no_outputs"})) {
            invalid("mode " + mode_ + " applies to phase invoke only");
        }
    }

    sdk::Host host_;
    std::string phase_ = "invoke";
    std::uint64_t at_invocation_ = 1;
    std::string mode_ = "status";
    praenora_status status_ = PRAENORA_STATUS_FAILED;
    std::size_t malformed_bytes_ = 0;
    std::size_t diagnostics_bytes_ = 0;
    std::uint64_t invocations_ = 0;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(FailModule)
