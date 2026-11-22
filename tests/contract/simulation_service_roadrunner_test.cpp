// The service checks of praenora.simulation@0 (simulation_service_checks.hpp) against the
// roadrunner provider (src/providers/roadrunner), built with PRAENORA_WITH_ROADRUNNER, with two
// SBML models whose solutions are known exactly (tests/fixtures/models/sbml): two species that
// decay on their own, and a two-compartment model whose initial amount is a parameter (an initial
// assignment). The method is CVODE with tight tolerances, so that the results agree with the exact
// solutions to a relative 1e-9.
#include "simulation_service_checks.hpp"

#include "test_support.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using praenora::test::SimulationCase;

std::string read_model(const char* name) {
    std::ifstream in(praenora::test::fixture_path(std::string("models/sbml/") + name),
                     std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// CVODE's Adams method with tolerances far below the agreement the checks require.
nlohmann::json tight_cvode() {
    return {{"method", "cvode"},
            {"stiffness", "nonstiff"},
            {"relative_tolerance", 1e-12},
            {"absolute_tolerance", 1e-12}};
}

// decay.xml: dx/dt = -k_x * x, dy/dt = -k_y * y in a compartment of size 2; the checks read the
// concentration of x, the rate of reaction ry and the amount of y.
SimulationCase decay() {
    SimulationCase c;
    c.name = "Decay";
    c.provider_manifest = praenora::test::utf8_path(PRAENORA_TEST_ROADRUNNER_MANIFEST);
    c.model_format = "sbml";
    c.model = read_model("decay.xml");
    c.states = {"x", "y"};
    c.parameters = {"k_x", "k_y"};
    c.parameter_values = {0.5, 1.5};
    c.quantities = {"[x]", "ry", "y"};
    c.initial_state = {10.0, 4.0};
    c.solver = tight_cvode();
    c.solution = [](double start, double t, const std::vector<double>& p,
                    const std::vector<double>& initial) {
        return std::vector<double>{initial[0] * std::exp(-p[0] * (t - start)),
                                   initial[1] * std::exp(-p[1] * (t - start))};
    };
    c.derivative = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{-p[0] * y[0], -p[1] * y[1]};
    };
    c.quantity_values = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{y[0] / 2.0, p[1] * y[1], y[1]};
    };
    // x grows as exp(1000 t), beyond the largest double before t = 1.
    c.failing_parameters = {-1000.0, 1.5};
    c.model_with_events = read_model("decay-with-event.xml");
    c.relative_tolerance = 1e-9;
    c.absolute_tolerance = 1e-12;
    return c;
}

// The exact solution of y' = M y for the two-compartment model, from `initial` at `start`, with
// M = [[-(k12 + ke), k21], [k12, -k21]]: exp(M tau) = (e^(l1 tau) (M - l2 I) - e^(l2 tau)
// (M - l1 I)) / (l1 - l2) for the eigenvalues l1 != l2 of M.
std::vector<double> two_compartments_at(double tau, double k12, double k21, double ke,
                                        const std::vector<double>& initial) {
    const double m11 = -(k12 + ke), m12 = k21, m21 = k12, m22 = -k21;
    const double trace = m11 + m22;
    const double determinant = m11 * m22 - m12 * m21;
    const double root = std::sqrt(trace * trace - 4.0 * determinant);
    const double l1 = (trace + root) / 2.0, l2 = (trace - root) / 2.0;
    const double e1 = std::exp(l1 * tau), e2 = std::exp(l2 * tau);
    const auto entry = [&](double m, bool diagonal) {
        return (e1 * (m - (diagonal ? l2 : 0.0)) - e2 * (m - (diagonal ? l1 : 0.0))) / (l1 - l2);
    };
    return {entry(m11, true) * initial[0] + entry(m12, false) * initial[1],
            entry(m21, false) * initial[0] + entry(m22, true) * initial[1]};
}

// two-compartments.xml: A1 starts at the parameter dose; the checks set dose, k12 and ke (k21
// keeps the model's 0.18) and read the concentration of A1 in the central compartment (volume 4),
// the rate of reaction elimination and the amount of A2.
SimulationCase two_compartments() {
    constexpr double k21 = 0.18;
    SimulationCase c;
    c.name = "TwoCompartments";
    c.provider_manifest = praenora::test::utf8_path(PRAENORA_TEST_ROADRUNNER_MANIFEST);
    c.model_format = "sbml";
    c.model = read_model("two-compartments.xml");
    c.states = {"A1", "A2"};
    c.parameters = {"dose", "k12", "ke"};
    c.parameter_values = {100.0, 0.35, 0.12};
    c.quantities = {"[A1]", "elimination", "A2"};
    c.initial_state = {100.0, 0.0};
    c.initial_state_of = [](const std::vector<double>& p) {
        return std::vector<double>{p[0], 0.0};
    };
    c.solver = tight_cvode();
    c.solution = [](double start, double t, const std::vector<double>& p,
                    const std::vector<double>& initial) {
        return two_compartments_at(t - start, p[1], k21, p[2], initial);
    };
    c.derivative = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{-(p[1] + p[2]) * y[0] + k21 * y[1], p[1] * y[0] - k21 * y[1]};
    };
    c.quantity_values = [](double, const std::vector<double>& y, const std::vector<double>& p) {
        return std::vector<double>{y[0] / 4.0, p[2] * y[0], y[1]};
    };
    // A1 grows as exp(1000 t), beyond the largest double before t = 1.
    c.failing_parameters = {100.0, 0.35, -1000.0};
    c.model_with_events = read_model("two-compartments-with-event.xml");
    c.relative_tolerance = 1e-9;
    c.absolute_tolerance = 1e-12;
    return c;
}

} // namespace

namespace praenora::test {

INSTANTIATE_TEST_SUITE_P(Roadrunner, SimulationService,
                         ::testing::Values(decay(), two_compartments()), simulation_case_name);

} // namespace praenora::test
