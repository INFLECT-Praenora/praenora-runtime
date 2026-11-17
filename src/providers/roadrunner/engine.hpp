// The roadrunner provider's use of libRoadRunner (C++ API): the process-wide configuration set
// once at start, the checks on model texts, compiled models and their cache, the simulators that
// run Level A time courses, and the kernels of Level B (docs/specs/simulation-service.md, the
// roadrunner provider's section). libRoadRunner's types stay in engine.cpp.
#ifndef PRAENORA_PROVIDERS_ROADRUNNER_ENGINE_HPP
#define PRAENORA_PROVIDERS_ROADRUNNER_ENGINE_HPP

#include "documents.hpp"
#include "provider_abi.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rr {
class RoadRunner;
class ExecutableModel;
} // namespace rr

namespace praenora::roadrunner {

// Sets libRoadRunner's process-wide configuration (every key of rr::Config, MCJit among them),
// routes its logging to the provider's logger and initializes LLVM. Called once, from start().
void start_engine(const praenora_provider_host* host);

// libRoadRunner objects are made, copied and destroyed only under the provider's engine lock
// (engine.cpp): this deleter takes it.
struct RunnerDeleter {
    void operator()(rr::RoadRunner* runner) const noexcept;
};
using RunnerPtr = std::unique_ptr<rr::RoadRunner, RunnerDeleter>;

// What a quantity of the model is, and how to read it.
enum class QuantityKind {
    state,                  // a value of the state vector (index into it)
    species_amount,         // the amount of a floating species that is not a state
    species_concentration,  // the concentration of a floating species
    reaction_rate,          // the rate of a reaction
    parameter               // the value of a global parameter defined by an assignment rule
};

struct Quantity {
    QuantityKind kind;
    int index;              // into the state vector, or the engine's list of its kind
    std::string selection;  // libRoadRunner's selection of the value
};

// A compiled model: a libRoadRunner object loaded from the model text, which is never simulated
// and only copied, and what describe_model reports. Immutable once made; shared by the sessions,
// plans and kernels of its text.
class CompiledModel {
public:
    CompiledModel(RunnerPtr runner, ModelIds ids,
                  std::vector<int> parameter_indices, std::vector<Quantity> quantities,
                  bool events);
    CompiledModel(const CompiledModel&) = delete;
    CompiledModel& operator=(const CompiledModel&) = delete;

    const ModelIds& ids() const noexcept { return ids_; }
    // The engine's index of each of ids().parameters.
    const std::vector<int>& parameter_indices() const noexcept { return parameter_indices_; }
    const std::vector<Quantity>& quantities() const noexcept { return quantities_; }
    bool events() const noexcept { return events_; }
    const std::string& description() const noexcept { return description_; }

    // A new libRoadRunner object of the model, in its original state. Waits for the engine while
    // polling `host` for cancellation.
    RunnerPtr copy(const praenora_host_api* host) const;

private:
    RunnerPtr runner_;
    ModelIds ids_;
    std::vector<int> parameter_indices_;
    std::vector<Quantity> quantities_;
    bool events_;
    std::string description_;
};

// The compiled model of an SBML text, from the cache or compiled now. Fails with DATA_INVALID
// (details.reason "model") for a text the provider refuses or the engine cannot compile, and with
// CANCELLED when the host asks the call to stop while it waits.
std::shared_ptr<const CompiledModel> compiled_model(std::string_view text,
                                                    const praenora_host_api* host);

// Runs Level A time courses of one model: one libRoadRunner object, reset before each run.
// One call at a time (sessions and plans hold it for their calls).
class Simulator {
public:
    Simulator(std::shared_ptr<const CompiledModel> model, const praenora_host_api* host);
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // One run with the output times of `settings` (not solver_steps), from the model's original
    // state with `values` set, into `out` (times x columns values, row-major). Returns the
    // failure document of a numerical failure (then `out` is unspecified), or null.
    json run(const RunSettings& settings, const RunValues& values, double* out,
             const praenora_host_api* host);

    // One run whose rows are the solver's steps, appended to `out` (row-major); `rows` is set.
    json run_steps(const RunSettings& settings, const RunValues& values, std::vector<double>& out,
                   std::size_t& rows, const praenora_host_api* host);

private:
    // Takes a fresh copy of the model when the last run asked for one, then sets the run's values
    // and settings.
    void prepare_run(const RunSettings& settings, const RunValues& values,
                     const praenora_host_api* host);

    std::shared_ptr<const CompiledModel> model_;
    RunnerPtr runner_;
    // Set when a run may have left something in the libRoadRunner object that a reset does not
    // undo (pending delayed events, a run that ended with an exception): the next run takes a
    // fresh copy of the model first.
    bool fresh_copy_needed_ = false;
    std::vector<std::string> selections_;
};

// Level B: what a kernel's `model` points to. Its own libRoadRunner object, reset to the model's
// initial values for the kernel's current parameters.
class KernelModel {
public:
    KernelModel(std::shared_ptr<const CompiledModel> model, std::vector<int> parameters,
                std::vector<Quantity> quantities, const praenora_host_api* host);
    KernelModel(const KernelModel&) = delete;
    KernelModel& operator=(const KernelModel&) = delete;

    std::size_t state_count() const noexcept { return state_count_; }
    std::size_t parameter_count() const noexcept { return parameters_.size(); }
    std::size_t quantity_count() const noexcept { return quantities_.size(); }

    // The kernel functions, without the NULL checks (kernel_rhs() and friends do those). They
    // throw what the engine throws.
    void rhs(double t, const double* y, double* dydt);
    void set_parameters(const double* values);
    void initial_state(double* y) const;
    void quantities(double t, const double* y, double* out);

private:
    std::shared_ptr<const CompiledModel> model_;
    RunnerPtr runner_;
    rr::ExecutableModel* engine_model_ = nullptr;
    std::vector<int> parameters_;
    std::vector<Quantity> quantities_;
    std::size_t state_count_ = 0;
    std::vector<double> initial_;
};

} // namespace praenora::roadrunner

#endif // PRAENORA_PROVIDERS_ROADRUNNER_ENGINE_HPP
