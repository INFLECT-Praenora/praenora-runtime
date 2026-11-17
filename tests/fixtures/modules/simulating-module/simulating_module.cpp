// simulating-module: a module that uses the simulation service praenora.simulation@0
// (include/praenora/sdk/simulation.h), which its manifest requires, end to end through the
// runtime and a service provider (the fixture provider test-simulation in the tests).
//
// create() gets the service's table from the host API. initialize() opens a session for the
// model of parameter `model` (in format `format`), prepares the plan of parameter `plan` and
// closes the session again: the plan keeps the compiled model on its own. With parameter `kernel`
// true, it also opens a kernel and keeps dy/dt at the plan's start time and the model's initial
// state. Each invoke() runs the plan once with the values of input `in`'s "values", or of
// parameter `values` when the input has none, and writes to output out
//   {"ok": true, "columns": [...], "rows": [[...], ...]} (and "rates": [...] with `kernel`), or
//   {"ok": false, "error": <the service's failure document>} for a numerical failure.
// The destructor closes the plan, as every module closes the service's objects before its
// destroy() returns. A call of the service that fails fails the module call with the service's
// status and its error document as details.cause.
#include <praenora/sdk/module.hpp>
#include <praenora/sdk/simulation.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace sdk = praenora::sdk;

using nlohmann::json;

// A sink that appends what it receives to a string.
struct Collected {
    std::string text;
    praenora_sink sink{this, &Collected::write};

    Collected() = default;
    Collected(const Collected&) = delete;
    Collected& operator=(const Collected&) = delete;

    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        if (context == nullptr || (data == nullptr && size != 0)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        try {
            static_cast<Collected*>(context)->text.append(data == nullptr ? "" : data, size);
            return PRAENORA_STATUS_OK;
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
    }
};

praenora_bytes bytes(const std::string& text) { return praenora_bytes{text.data(), text.size()}; }

json parse(std::string_view text, const char* what) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::exception& e) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                         std::string("simulating-module: ") + what + " is not JSON: " + e.what());
    }
}

// Fails the module call with the status of a failed service call and its error document.
[[noreturn]] void service_failed(const char* call, praenora_status status, const Collected& error) {
    const json cause = json::parse(error.text, nullptr, false);
    throw sdk::Error(status, std::string("simulating-module: ") + call + " failed",
                     json{{"cause", cause.is_discarded() ? json(error.text) : cause}}.dump());
}

std::vector<double> numbers(const json& value, const char* what) {
    if (!value.is_array()) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                         std::string("simulating-module: ") + what + " is not an array");
    }
    std::vector<double> result;
    for (const json& item : value) {
        if (!item.is_number()) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             std::string("simulating-module: ") + what + " holds a non-number");
        }
        result.push_back(item.get<double>());
    }
    return result;
}

class SimulatingModule final : public sdk::Module {
public:
    explicit SimulatingModule(const sdk::CreateContext& context) : host_(context.host) {
        const json parameters =
            parse(context.create_json, "the create document").value("parameters", json::object());
        model_ = parameters.value("model", std::string());
        format_ = parameters.value("format", std::string("test-decay"));
        plan_json_ = parameters.value("plan", json::object()).dump();
        values_ = numbers(parameters.value("values", json::array()), "parameter values");
        kernel_ = parameters.value("kernel", false);

        // The table, once; tables live until the process ends.
        table_ = static_cast<const praenora_simulation_v0*>(
            host_.service(PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR));
        if (table_ == nullptr || table_->header.abi_major != PRAENORA_SIMULATION_SERVICE_MAJOR ||
            table_->header.reserved != 0u ||
            table_->header.struct_size < PRAENORA_SIMULATION_V0_BASE_SIZE) {
            throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED,
                             "simulating-module: the runtime serves no usable "
                             "praenora.simulation@0");
        }
    }

    ~SimulatingModule() override { table_->close_plan(plan_); }

    void initialize(const sdk::InitializeContext&) override {
        const praenora_host_api* host = host_.api();
        const std::string options = json{{"format", format_}}.dump();
        Collected error;
        praenora_simulation_session* session = nullptr;
        praenora_status status = table_->open_session(table_->context, host, bytes(model_),
                                                      bytes(options), &session, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            service_failed("open_session", status, error);
        }
        // The session is needed only to prepare: the plan keeps the compiled model on its own.
        struct Closer {
            const praenora_simulation_v0* table;
            praenora_simulation_session* session;
            ~Closer() { table->close_session(session); }
        } closer{table_, session};

        Collected info;
        status = table_->prepare(session, host, bytes(plan_json_), &plan_, &info.sink, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            service_failed("prepare", status, error);
        }
        const json described = parse(info.text, "the plan's description");
        inputs_ = described.at("inputs").size();
        columns_ = described.at("columns");
        rows_ = described.at("rows").get<std::size_t>();

        if (kernel_) {
            const double start =
                parse(plan_json_, "the plan").at("time_domain").at("start").get<double>();
            // Closed when this block ends, whatever happens in it.
            struct KernelCloser {
                const praenora_simulation_v0* table;
                praenora_simulation_kernel kernel{};
                ~KernelCloser() { table->close_kernel(&kernel); }
            } open{table_};
            praenora_simulation_kernel& kernel = open.kernel;
            kernel.struct_size = static_cast<std::uint32_t>(sizeof kernel);
            status = table_->open_kernel(session, host, bytes("{}"), &kernel, &error.sink);
            if (status != PRAENORA_STATUS_OK) {
                service_failed("open_kernel", status, error);
            }
            std::vector<double> y(kernel.state_count);
            rates_.assign(kernel.state_count, 0.0);
            status = kernel.initial_state(kernel.model, y.data());
            if (status == PRAENORA_STATUS_OK) {
                status = kernel.rhs(kernel.model, start, y.data(), rates_.data());
            }
            if (status != PRAENORA_STATUS_OK) {
                throw sdk::Error(status, "simulating-module: a kernel function failed");
            }
        }
    }

    std::string invoke(std::string_view invocation_json) override {
        const json invocation = parse(invocation_json, "the invocation document");
        std::vector<double> values = values_;
        const json in = invocation.value("inputs", json::object()).value("in", json::object());
        const json data = in.value("data", json::object());
        if (data.is_object() && data.contains("values")) {
            values = numbers(data.at("values"), "input values");
        }
        if (values.size() != inputs_) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             "simulating-module: the plan takes " + std::to_string(inputs_) +
                                 " values, not " + std::to_string(values.size()));
        }

        std::vector<double> out(rows_ * columns_.size());
        std::int32_t outcome = PRAENORA_SIMULATION_RUN_OK;
        Collected failure, error;
        const praenora_status status =
            table_->run(plan_, host_.api(), values.empty() ? nullptr : values.data(),
                        values.size(), out.data(), out.size(), &outcome, &failure.sink,
                        &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            service_failed("run", status, error);
        }

        json result = json::object();
        if (outcome == PRAENORA_SIMULATION_RUN_OK) {
            json rows = json::array();
            for (std::size_t row = 0; row < rows_; ++row) {
                const auto first =
                    out.begin() + static_cast<std::ptrdiff_t>(row * columns_.size());
                rows.push_back(std::vector<double>(
                    first, first + static_cast<std::ptrdiff_t>(columns_.size())));
            }
            result = {{"ok", true}, {"columns", columns_}, {"rows", std::move(rows)}};
            if (kernel_) {
                result["rates"] = rates_;
            }
        } else {
            result = {{"ok", false}, {"error", parse(failure.text, "the failure document")}};
        }
        json document;
        document["outputs"]["out"]["data"] = std::move(result);
        return document.dump();
    }

private:
    const sdk::Host& host_;
    const praenora_simulation_v0* table_ = nullptr;
    praenora_simulation_plan* plan_ = nullptr;
    std::string model_, format_, plan_json_;
    std::vector<double> values_;
    bool kernel_ = false;
    std::size_t inputs_ = 0, rows_ = 0;
    json columns_;
    std::vector<double> rates_;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(SimulatingModule)
