// The service checks of the simulation service praenora.simulation@0
// (include/praenora/sdk/simulation.h, docs/specs/simulation-service.md): a parameterized
// GoogleTest suite, SimulationService, that loads a provider of the service through the runtime's
// provider loader and exercises its table: describe, sessions, describe_model, JSON simulate
// (parameter values as initial values, output times), plans (shape, inputs, run, batch order,
// outcomes and failure documents, buffer sizes), kernels (right-hand side against the exact
// derivative, initial state, parameters, quantities, models with events), busy sessions and plans,
// cancellation, NULL and refusing sinks, short host APIs, unknown fields, size limits, object
// lifetime, and concurrent use of one session and of distinct plans and kernels.
//
// The checks (simulation_service_checks.cpp) know nothing of a particular provider or model
// format. Each parameter set, a SimulationCase, names a provider manifest, a model it can open and
// the exact solution the results are compared with. A provider is checked by instantiating the
// suite in a source file of its own:
//
//   INSTANTIATE_TEST_SUITE_P(MyProvider, SimulationService, ::testing::Values(my_case()),
//                            praenora::test::simulation_case_name);
//
// simulation_service_test.cpp instantiates it for the fixture provider test-simulation.
#ifndef PRAENORA_TESTS_CONTRACT_SIMULATION_SERVICE_CHECKS_HPP
#define PRAENORA_TESTS_CONTRACT_SIMULATION_SERVICE_CHECKS_HPP

#include <praenora/sdk/simulation.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace praenora::test {

// The values of the states (or of quantities) as a function of time, states and parameters.
using SimulationSolution =
    std::function<std::vector<double>(double start, double t, const std::vector<double>& parameters,
                                      const std::vector<double>& initial)>;
using SimulationFunction = std::function<std::vector<double>(
    double t, const std::vector<double>& y, const std::vector<double>& parameters)>;
// The model's initial state as a function of the values of the case's parameters.
using SimulationInitialState =
    std::function<std::vector<double>(const std::vector<double>& parameters)>;

// One parameter set of the service checks: a provider, a model it can open, and what its results
// are compared with. Every vector of values follows the order of the ids it belongs to.
struct SimulationCase {
    // The parameter set's name in the test names: letters, digits and '_' only.
    std::string name;
    // The manifest of the provider of praenora.simulation@0 that is checked.
    std::filesystem::path provider_manifest;
    // open_session's "format", one of the provider's model formats.
    std::string model_format;
    // The model text. The model has no events (the kernel checks need its kernel).
    std::string model;
    // describe_model's "states", in its order: the order of y in kernels.
    std::vector<std::string> states;
    // The parameters the checks set (describe_model lists each), and the model's values of them.
    std::vector<std::string> parameters;
    std::vector<double> parameter_values;
    // The quantities the checks read (describe_model lists each).
    std::vector<std::string> quantities;
    // The model's initial state at the model's parameter values.
    std::vector<double> initial_state;
    // When the model's initial state depends on the case's parameters (an initial value that is a
    // parameter, or an initial assignment that uses one): the initial state for given values of
    // them. Unset: the initial state is `initial_state` whatever the parameters.
    SimulationInitialState initial_state_of;
    // The solver settings of the checks' runs, at least {"method": <one of describe's methods>}.
    nlohmann::json solver;
    // The exact state at time t of a run from `start` with the state `initial` at `start` and the
    // values `parameters` of the case's parameters (the model's values for every other one).
    SimulationSolution solution;
    // The exact dy/dt at (t, y) for `parameters`.
    SimulationFunction derivative;
    // The exact values of the case's quantities at (t, y) for `parameters`.
    SimulationFunction quantity_values;
    // Values of the case's parameters for which a run fails numerically (it diverges, for
    // example) before the end time of the checks' runs.
    std::vector<double> failing_parameters;
    // The end time of the checks' runs, which start at 0; the output times lie in [0, end].
    double end_time = 2.0;
    // Optional: a model of the same format that has events. The checks open it, expect
    // describe_model to report "events": true and open_kernel to refuse it. Empty: that check is
    // skipped.
    std::string model_with_events;
    // Results agree with the exact solution when
    // |a - b| <= max(absolute, relative * max(|a|, |b|)).
    double relative_tolerance = 1e-12;
    double absolute_tolerance = 1e-12;
};

// Shown in test failures instead of the bytes of the struct.
void PrintTo(const SimulationCase& simulation_case, std::ostream* out);

// The test-name suffix of a parameter set: its name.
std::string simulation_case_name(const ::testing::TestParamInfo<SimulationCase>& info);

// The suite. SetUp() loads the parameter set's provider (once per process, as the runtime always
// does) and takes its praenora.simulation@0 table.
class SimulationService : public ::testing::TestWithParam<SimulationCase> {
protected:
    void SetUp() override;

    // The table under test.
    const praenora_simulation_v0* table() const { return table_; }

private:
    const praenora_simulation_v0* table_ = nullptr;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_CONTRACT_SIMULATION_SERVICE_CHECKS_HPP
