// example-estimator: the estimate step of the example workflow (capability
// example.estimate@0.1.0), built with the module SDK's C++ adapter.
//
// Each invocation receives an observation batch (input `observations`) and, from the second tick
// on, the previous tick's predictions (input `prior`, which the workflow feeds back through a
// delayed connection). For each (series_id, variable_id), in sorted order:
//   - its usable observations (not missing, not censored, with a value), in time order, each
//     update the estimate: with a prior prediction at the observation's time, the estimate is
//     p + gain * (y - p) (basis "blend"); without one it is the observation y (basis
//     "observation"). The prior prediction p interpolates the prior's latent_mean rows of the
//     same series and variable linearly, inside their time range only;
//   - the estimate emitted is the one at its latest usable observation;
//   - with no usable observation, a prior that covers the window's end (the invocation's
//     logical_time) is carried over to it (basis "prior");
//   - otherwise nothing is emitted for it.
// Output `state`: {"time_unit": the batch's, "estimates": [{series_id, variable_id, time, value,
// basis}, ...]}. Parameter: gain in [0, 1] (default 0.5). Only + - * / arithmetic is used, and
// nothing depends on the clock or on chance, so the module is deterministic.
//
// diagnostics(): {"updates", "blends", "carried"}, counted since the instance was created: the
// usable observations, those blended with a prior, and the estimates carried over from one.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace sdk = praenora::sdk;
using nlohmann::json;

using Key = std::pair<std::string, std::string>; // series_id, variable_id

struct Point {
    double time = 0, value = 0;
};

[[noreturn]] void invalid(const std::string& what) {
    throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "example-estimator: " + what);
}

json parse(std::string_view text, const char* what) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::exception& e) {
        invalid(std::string("the ") + what + " is not JSON: " + e.what());
    }
}

// The number at `key` of `object`.
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

// The data of input `port`, or null when the input is absent.
const json* input_data(const json& invocation, const char* port) {
    const auto inputs = invocation.find("inputs");
    if (inputs == invocation.end() || !inputs->is_object()) {
        invalid("the invocation document has no inputs object");
    }
    const auto input = inputs->find(port);
    if (input == inputs->end()) {
        return nullptr;
    }
    const auto data = input->find("data");
    if (data == input->end() || !data->is_object()) {
        invalid(std::string("input ") + port + " has no data object");
    }
    return &*data;
}

// The value of `points` (sorted by time) at `time` by linear interpolation, inside their time
// range only.
std::optional<double> interpolate(const std::vector<Point>& points, double time) {
    if (points.empty() || time < points.front().time || time > points.back().time) {
        return std::nullopt;
    }
    const auto after = std::lower_bound(points.begin(), points.end(), time,
                                        [](const Point& p, double t) { return p.time < t; });
    if (after->time == time) {
        return after->value;
    }
    const Point& b = *after;
    const Point& a = *(after - 1);
    return a.value + (b.value - a.value) * ((time - a.time) / (b.time - a.time));
}

class Estimator final : public sdk::Module {
public:
    explicit Estimator(const sdk::CreateContext& context) : host_(context.host) {
        const json create = parse(context.create_json, "create document");
        const auto parameters = create.find("parameters");
        if (parameters != create.end() && parameters->is_object() && parameters->contains("gain")) {
            gain_ = number_at(*parameters, "gain", "the parameters");
            if (!(gain_ >= 0.0 && gain_ <= 1.0)) {
                invalid("gain must lie in [0, 1]");
            }
        }
    }

    std::string invoke(std::string_view invocation_json) override {
        const json invocation = parse(invocation_json, "invocation document");
        const double window_end = number_at(invocation, "logical_time", "the invocation");
        const json* batch = input_data(invocation, "observations");
        if (batch == nullptr) {
            invalid("the required input observations is missing");
        }

        // The usable observations and the prior's latent_mean rows, by series and variable.
        std::map<Key, std::vector<Point>> observed, prior;
        const auto rows = batch->find("observations");
        if (rows == batch->end() || !rows->is_array()) {
            invalid("the observation batch has no observations array");
        }
        for (const json& row : *rows) {
            const auto missing = row.find("missing");
            const auto censoring = row.find("censoring");
            const auto value = row.find("value");
            const bool usable = (missing == row.end() || *missing == false) &&
                                (censoring == row.end() || *censoring == "none") &&
                                value != row.end() && value->is_number();
            if (!usable) {
                continue;
            }
            observed[{string_at(row, "series_id", "an observation"),
                      string_at(row, "variable_id", "an observation")}]
                .push_back({number_at(row, "time", "an observation"), value->get<double>()});
        }
        if (const json* predictions = input_data(invocation, "prior")) {
            const auto prior_rows = predictions->find("predictions");
            if (prior_rows == predictions->end() || !prior_rows->is_array()) {
                invalid("the prior has no predictions array");
            }
            for (const json& row : *prior_rows) {
                const auto kind = row.find("prediction_kind");
                if (kind == row.end() || *kind != "latent_mean") {
                    continue;
                }
                prior[{string_at(row, "series_id", "a prediction"),
                       string_at(row, "variable_id", "a prediction")}]
                    .push_back({number_at(row, "time", "a prediction"),
                                number_at(row, "value", "a prediction")});
            }
        }
        const auto by_time = [](const Point& a, const Point& b) { return a.time < b.time; };
        std::vector<Key> keys;
        for (auto& [key, points] : observed) {
            std::stable_sort(points.begin(), points.end(), by_time);
            keys.push_back(key);
        }
        for (auto& [key, points] : prior) {
            std::stable_sort(points.begin(), points.end(), by_time);
            if (observed.count(key) == 0) {
                keys.push_back(key);
            }
        }
        std::sort(keys.begin(), keys.end());

        json estimates = json::array();
        std::uint64_t updates = 0, blends = 0, carried = 0;
        for (const Key& key : keys) {
            const auto prior_points = prior.find(key);
            const std::vector<Point> none;
            const std::vector<Point>& points =
                prior_points != prior.end() ? prior_points->second : none;
            json estimate = {{"series_id", key.first}, {"variable_id", key.second}};
            const auto observations = observed.find(key);
            if (observations != observed.end()) {
                for (const Point& y : observations->second) {
                    ++updates;
                    estimate["time"] = y.time;
                    if (const auto p = interpolate(points, y.time)) {
                        ++blends;
                        estimate["value"] = *p + gain_ * (y.value - *p);
                        estimate["basis"] = "blend";
                    } else {
                        estimate["value"] = y.value;
                        estimate["basis"] = "observation";
                    }
                }
            } else if (const auto p = interpolate(points, window_end)) {
                ++carried;
                estimate["time"] = window_end;
                estimate["value"] = *p;
                estimate["basis"] = "prior";
            } else {
                continue; // nothing to estimate from
            }
            estimates.push_back(std::move(estimate));
        }
        updates_ += updates;
        blends_ += blends;
        carried_ += carried;
        if (host_.log_enabled(sdk::LogLevel::debug)) {
            host_.log(sdk::LogLevel::debug, std::to_string(estimates.size()) + " estimates from " +
                                                std::to_string(updates) + " observations (" +
                                                std::to_string(blends) + " blended), " +
                                                std::to_string(carried) + " carried over");
        }

        json result;
        result["outputs"]["state"]["data"] = {
            {"time_unit", string_at(*batch, "time_unit", "the observation batch")},
            {"estimates", std::move(estimates)}};
        return result.dump();
    }

    std::string diagnostics() const override {
        return json{{"updates", updates_}, {"blends", blends_}, {"carried", carried_}}.dump();
    }

private:
    sdk::Host host_;
    double gain_ = 0.5;
    std::uint64_t updates_ = 0, blends_ = 0, carried_ = 0;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(Estimator)
