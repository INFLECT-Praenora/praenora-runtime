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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rr {
class RoadRunner;
class ExecutableModel;
} // namespace rr

namespace praenora::roadrunner {

// Sets libRoadRunner's process-wide configuration (every key of rr::Config, MCJit among them),
// routes its logging to the provider's logger and initializes LLVM, and keeps `host` for its store
// callbacks (the shared compiled models). Called once, from start().
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

// A compiled model: a libRoadRunner object loaded from the model text (compiled, or loaded from a
// saved state of it), which is never simulated and only copied, and what describe_model reports.
// Immutable once made; shared by the sessions, plans and kernels of its text.
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

    // The libRoadRunner object's saved state (saveStateS('b')) in its envelope (state_envelope()),
    // as the shared compiled models keep it. Waits for the engine as copy() does.
    std::string saved_state(const praenora_host_api* host) const;

private:
    RunnerPtr runner_;
    ModelIds ids_;
    std::vector<int> parameter_indices_;
    std::vector<Quantity> quantities_;
    bool events_;
    std::string description_;
};

// The compiled model of an SBML text, from the cache, or else from the shared compiled models of
// the provider host when it offers them and they hold it (logged at debug through `host`:
// "roadrunner: loaded the compiled model from the shared store"), or compiled now ("roadrunner:
// compiled the model") and then kept there, when there are any. An entry there that does not load
// is logged at warn through `host`, and the model is compiled instead. The cache key, which is the
// store's key too, is the SHA-256 of the engine's identity, the load options and the text: nothing
// of the process. Fails with DATA_INVALID (details.reason "model") for a text the provider refuses
// or the engine cannot compile, and with CANCELLED when the host asks the call to stop while it
// waits.
std::shared_ptr<const CompiledModel> compiled_model(std::string_view text,
                                                    const praenora_host_api* host);

// The pieces of the shared compiled models, for compiled_model() and its tests.
//
// An entry: the bytes "praenora-roadrunner-state\n", the size of the engine's identity (u64,
// little-endian) and the identity (the engine object of the provider manifest and the load
// options), the size of the state (u64), the state's SHA-256 (32 bytes), and the state that
// saveStateS('b') wrote. The checks find an entry that is corrupt or cut short, or that another
// engine saved; they do not make an entry made on purpose safe to load.
std::string state_envelope(std::string_view payload);
// The state that `entry` holds, or nullopt with why in `why`: not an envelope of this provider,
// another engine's, a size other than the state's, or a SHA-256 other than the state's.
std::optional<std::string_view> envelope_payload(std::string_view entry, std::string& why);
// The text compiled now, past the cache and the store: the checks and errors of compiled_model().
std::shared_ptr<const CompiledModel> compile_model(std::string_view text,
                                                   const praenora_host_api* host);
// The text's compiled model loaded from `entry` without compiling (the text's checks still run),
// or nullptr with why in `why` when the entry does not load: its envelope's checks fail, or
// libRoadRunner's loadStateS() throws.
std::shared_ptr<const CompiledModel> load_compiled_model(std::string_view text,
                                                         std::string_view entry,
                                                         const praenora_host_api* host,
                                                         std::string& why);

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
