// example-predictor: the predict step of the example workflow (capability example.predict@0.1.0),
// built with the module SDK's C++ adapter.
//
// For each estimate (value x0 at time t0) of its input `state`, it integrates the logistic
// equation dx/dt = r * x * (1 - x / K) (r: growth_rate, K: capacity) from t0 with the classical
// fixed-step Runge-Kutta method (RK4, step step_size), and emits the value at the times
// t_j = window_end + j * output_interval, j = 0 ... floor(horizon / output_interval), where
// window_end is the invocation's logical_time. The last step before each t_j is shortened so that
// it lands exactly on t_j. Each row carries the estimate's series_id and variable_id, its unit if
// it has one, and prediction_kind "latent_mean". Output `predictions`: {"issued_at": window_end,
// "time_unit": the state's, "predictions": [...]}, the rows of each estimate in turn.
// Parameters (defaults in brackets), all > 0: growth_rate [0.3], capacity [100], horizon [4.0],
// step_size [0.05], output_interval [0.5]. Only + - * / arithmetic is used, and nothing depends
// on the clock or on chance, so the module is deterministic.
//
// diagnostics(): {"predictions", "steps"}, counted since the instance was created: the rows
// emitted and the integration steps taken.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;
using nlohmann::json;

// A horizon may need at most this many integration steps, and give at most this many rows per
// estimate, so that an invocation stays bounded.
constexpr double kMaxSteps = 1e7;
constexpr double kMaxRows = 1e5;

[[noreturn]] void invalid(const std::string& what) {
    throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "example-predictor: " + what);
}

json parse(std::string_view text, const char* what) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::exception& e) {
        invalid(std::string("the ") + what + " is not JSON: " + e.what());
    }
}

double number_at(const json& object, const char* key, const char* what) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) {
        invalid(std::string(what) + " has no number " + key);
    }
    return found->get<double>();
}

std::string string_at(const json& object, const char* key, const char* what) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        invalid(std::string(what) + " has no string " + key);
    }
    return found->get<std::string>();
}

class Predictor final : public sdk::Module {
public:
    explicit Predictor(const sdk::CreateContext& context) : host_(context.host) {
        const json create = parse(context.create_json, "create document");
        const auto parameters = create.find("parameters");
        if (parameters != create.end() && parameters->is_object()) {
            read(*parameters, "growth_rate", rate_);
            read(*parameters, "capacity", capacity_);
            read(*parameters, "horizon", horizon_);
            read(*parameters, "step_size", step_);
            read(*parameters, "output_interval", interval_);
        }
        if (horizon_ / step_ > kMaxSteps || horizon_ / interval_ > kMaxRows) {
            invalid("the horizon needs too many steps or rows; make step_size or "
                    "output_interval larger");
        }
    }

    std::string invoke(std::string_view invocation_json) override {
        const json invocation = parse(invocation_json, "invocation document");
        const double window_end = number_at(invocation, "logical_time", "the invocation");
        const auto inputs = invocation.find("inputs");
        if (inputs == invocation.end() || !inputs->is_object() || !inputs->contains("state")) {
            invalid("the required input state is missing");
        }
        const json& input = inputs->at("state");
        const auto data = input.find("data");
        if (data == input.end() || !data->is_object()) {
            invalid("input state has no data object");
        }
        const json& state = *data;
        const auto estimates = state.find("estimates");
        if (estimates == state.end() || !estimates->is_array()) {
            invalid("the state has no estimates array");
        }

        json rows = json::array();
        const std::uint64_t steps_before = steps_;
        for (const json& estimate : *estimates) {
            const std::string series = string_at(estimate, "series_id", "an estimate");
            const std::string variable = string_at(estimate, "variable_id", "an estimate");
            const auto unit = estimate.find("unit");
            double time = number_at(estimate, "time", "an estimate");
            double value = number_at(estimate, "value", "an estimate");
            if (time > window_end) {
                invalid("an estimate at time " + std::to_string(time) +
                        " lies after the window's end, " + std::to_string(window_end));
            }
            for (std::uint64_t j = 0;; ++j) {
                const double offset = static_cast<double>(j) * interval_;
                if (offset > horizon_ + horizon_ * 1e-9) {
                    break; // j > floor(horizon / output_interval), up to rounding
                }
                const double target = window_end + offset;
                advance(time, value, target);
                json row = {{"series_id", series},
                            {"variable_id", variable},
                            {"time", target},
                            {"prediction_kind", "latent_mean"},
                            {"value", value}};
                if (unit != estimate.end() && unit->is_string()) {
                    row["unit"] = *unit;
                }
                rows.push_back(std::move(row));
            }
        }
        predictions_ += rows.size();
        if (host_.log_enabled(sdk::LogLevel::debug)) {
            host_.log(sdk::LogLevel::debug, std::to_string(rows.size()) + " predictions for " +
                                                std::to_string(estimates->size()) + " estimates (" +
                                                std::to_string(steps_ - steps_before) +
                                                " integration steps)");
        }

        json result;
        result["outputs"]["predictions"]["data"] = {
            {"issued_at", window_end},
            {"time_unit", string_at(state, "time_unit", "the state")},
            {"predictions", std::move(rows)}};
        return result.dump();
    }

    std::string diagnostics() const override {
        return json{{"predictions", predictions_}, {"steps", steps_}}.dump();
    }

private:
    // Reads the positive number `key` of `parameters` into `target`, when it is there.
    static void read(const json& parameters, const char* key, double& target) {
        if (!parameters.contains(key)) {
            return;
        }
        target = number_at(parameters, key, "the parameters");
        if (!(target > 0.0)) {
            invalid(std::string(key) + " must be greater than 0");
        }
    }

    // dx/dt of the logistic equation.
    double slope(double x) const { return rate_ * x * (1.0 - x / capacity_); }

    // One classical Runge-Kutta step of length h from x.
    double rk4(double x, double h) const {
        const double k1 = slope(x);
        const double k2 = slope(x + 0.5 * h * k1);
        const double k3 = slope(x + 0.5 * h * k2);
        const double k4 = slope(x + h * k3);
        return x + (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
    }

    // Integrates (time, value) forward to `target` in steps of step_size, the last one shortened
    // so that it ends exactly on `target`. (A remainder within a relative 1e-9 of a whole step is
    // taken as that last step, rather than leaving a step of almost nothing.)
    void advance(double& time, double& value, double target) {
        while (time < target) {
            const double remaining = target - time;
            if (remaining <= step_ + step_ * 1e-9) {
                value = rk4(value, remaining);
                time = target;
            } else {
                value = rk4(value, step_);
                time = time + step_;
            }
            ++steps_;
        }
    }

    sdk::Host host_;
    double rate_ = 0.3, capacity_ = 100.0, horizon_ = 4.0, step_ = 0.05, interval_ = 0.5;
    std::uint64_t predictions_ = 0, steps_ = 0;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(Predictor)
