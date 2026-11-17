// The service checks of praenora.simulation@0 (simulation_service_checks.hpp) against the fixture
// provider test-simulation, whose toy model format "test-decay" is solved exactly:
// dx/dt = -k_x * x for each state x, so x(t) = x(start) * exp(-k_x * (t - start)); its quantities
// are the states and "total", their sum. A state's initial value may be a parameter, and a model
// may declare events (which make it a model without a kernel).
#include "simulation_service_checks.hpp"

#include "provider_fixtures.hpp"

#include <cmath>
#include <vector>

namespace {

using praenora::test::SimulationCase;

// Two states, every parameter and quantity in describe_model's order.
SimulationCase two_states() {
    SimulationCase c;
    c.name = "TwoStates";
    c.provider_manifest = praenora::test::fixture_provider_manifest("test-simulation");
    c.model_format = "test-decay";
    c.model = R"({"states": {"x": 10, "y": 4}, "parameters": {"k_x": 0.5, "k_y": 1.5}})";
    c.states = {"x", "y"};
    c.parameters = {"k_x", "k_y"};
    c.parameter_values = {0.5, 1.5};
    c.quantities = {"x", "y", "total"};
    c.initial_state = {10.0, 4.0};
    c.solver = {{"method", "analytic"}};
    c.solution = [](double start, double t, const std::vector<double>& p,
                    const std::vector<double>& initial) {
        return std::vector<double>{initial[0] * std::exp(-p[0] * (t - start)),
                                   initial[1] * std::exp(-p[1] * (t - start))};
    };
    c.derivative = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{-p[0] * y[0], -p[1] * y[1]};
    };
    c.quantity_values = [](double, const std::vector<double>& y, const std::vector<double>&) {
        return std::vector<double>{y[0], y[1], y[0] + y[1]};
    };
    // x grows as exp(1000 t), beyond the largest double before t = 1.
    c.failing_parameters = {-1000.0, 1.5};
    c.model_with_events = R"({"states": {"x": 10, "y": 4}, "parameters": {"k_x": 0.5, "k_y": 1.5},
                              "events": [{"time": 1, "state": "x", "value": 10}]})";
    return c;
}

// Three states, of which the checks set two parameters, in another order than describe_model's,
// and read two quantities; k_b keeps the model's value 2.
SimulationCase three_states() {
    SimulationCase c;
    c.name = "ThreeStatesSomeParameters";
    c.provider_manifest = praenora::test::fixture_provider_manifest("test-simulation");
    c.model_format = "test-decay";
    c.model = R"({"parameters": {"k_a": 0.1, "k_b": 2, "k_c": 0.75},
                  "states": {"c": 0.25, "b": 2.5, "a": 1}})";
    c.states = {"a", "b", "c"};
    c.parameters = {"k_c", "k_a"};
    c.parameter_values = {0.75, 0.1};
    c.quantities = {"total", "b"};
    c.initial_state = {1.0, 2.5, 0.25};
    c.solver = {{"method", "analytic"}};
    c.solution = [](double start, double t, const std::vector<double>& p,
                    const std::vector<double>& initial) {
        return std::vector<double>{initial[0] * std::exp(-p[1] * (t - start)),
                                   initial[1] * std::exp(-2.0 * (t - start)),
                                   initial[2] * std::exp(-p[0] * (t - start))};
    };
    c.derivative = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{-p[1] * y[0], -2.0 * y[1], -p[0] * y[2]};
    };
    c.quantity_values = [](double, const std::vector<double>& y, const std::vector<double>&) {
        return std::vector<double>{y[0] + y[1] + y[2], y[1]};
    };
    // c grows as exp(800 t), beyond the largest double before t = 1.
    c.failing_parameters = {-800.0, 0.1};
    c.model_with_events = R"({"states": {"a": 1}, "parameters": {"k_a": 0.1},
                              "events": [{"time": 0.5, "state": "a", "value": 2}]})";
    return c;
}

// The initial value of x is the parameter x0, so parameter values change the initial state.
SimulationCase state_from_parameter() {
    SimulationCase c;
    c.name = "StateFromParameter";
    c.provider_manifest = praenora::test::fixture_provider_manifest("test-simulation");
    c.model_format = "test-decay";
    c.model = R"({"states": {"x": "x0", "y": 2},
                  "parameters": {"x0": 10, "k_x": 0.5, "k_y": 0.25}})";
    c.states = {"x", "y"};
    c.parameters = {"x0", "k_x"};
    c.parameter_values = {10.0, 0.5};
    c.quantities = {"x", "total"};
    c.initial_state = {10.0, 2.0};
    c.initial_state_of = [](const std::vector<double>& p) {
        return std::vector<double>{p[0], 2.0};
    };
    c.solver = {{"method", "analytic"}};
    c.solution = [](double start, double t, const std::vector<double>& p,
                    const std::vector<double>& initial) {
        return std::vector<double>{initial[0] * std::exp(-p[1] * (t - start)),
                                   initial[1] * std::exp(-0.25 * (t - start))};
    };
    c.derivative = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{-p[1] * y[0], -0.25 * y[1]};
    };
    c.quantity_values = [](double, const std::vector<double>& y, const std::vector<double>&) {
        return std::vector<double>{y[0], y[0] + y[1]};
    };
    // x grows as exp(1000 t), beyond the largest double before t = 1.
    c.failing_parameters = {10.0, -1000.0};
    c.model_with_events = R"({"states": {"x": "x0"}, "parameters": {"x0": 1, "k_x": 0.5},
                              "events": [{"time": 1, "state": "x", "value": 1}]})";
    return c;
}

} // namespace

namespace praenora::test {

INSTANTIATE_TEST_SUITE_P(TestSimulation, SimulationService,
                         ::testing::Values(two_states(), three_states(), state_from_parameter()),
                         simulation_case_name);

} // namespace praenora::test
