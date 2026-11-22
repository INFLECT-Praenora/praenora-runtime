// What the roadrunner provider's service functions share apart from libRoadRunner: failures and
// the error documents that report them, sinks, the calling instance's host API, the JSON documents
// passed to the service (options, requests, plans) and the settings of a run they describe
// (docs/specs/simulation-service.md).
#ifndef PRAENORA_PROVIDERS_ROADRUNNER_DOCUMENTS_HPP
#define PRAENORA_PROVIDERS_ROADRUNNER_DOCUMENTS_HPP

#include <praenora/sdk/simulation.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::roadrunner {

using nlohmann::json;

// ---------------------------------------------------------------------------------------------
// Failures.

// A failed call: its status, message and details (an object, to which "service" is added).
struct Failure {
    praenora_status code;
    std::string message;
    json details;
};

[[noreturn]] void fail(praenora_status code, std::string message, json details = json::object());

// The details of a problem at a JSON pointer of a document.
json at_pointer(const json::json_pointer& pointer, json details = json::object());

[[noreturn]] void invalid(const std::string& message, const json::json_pointer& pointer,
                          json details = json::object());
[[noreturn]] void unsupported(const std::string& message, const json::json_pointer& pointer,
                              json details = json::object());

// {"type", "message"} of an exception the engine raised.
json engine_error(const std::exception& error);

// ---------------------------------------------------------------------------------------------
// Sinks, inputs and calls.

bool usable(const praenora_sink* sink) noexcept;
bool readable(praenora_bytes bytes) noexcept;
std::string_view view(praenora_bytes bytes) noexcept;

// Writes `text` to `sink`; a refused write fails the call with the sink's status.
void write_to(praenora_sink* sink, const std::string& text, const char* what);

// Writes an error document of the pmp-error shape, when the sink takes it.
void write_error(praenora_sink* error_json, praenora_status code, const std::string& message,
                 json details = json::object()) noexcept;

// Runs `body`, turning what it throws into a status and an error document.
template <class Body> praenora_status guarded(praenora_sink* error_json, Body&& body) noexcept {
    try {
        body();
        return PRAENORA_STATUS_OK;
    } catch (const Failure& failure) {
        write_error(error_json, failure.code, failure.message, failure.details);
        return failure.code;
    } catch (const std::bad_alloc&) {
        write_error(error_json, PRAENORA_STATUS_RESOURCE_EXHAUSTED,
                    "praenora.simulation: out of memory");
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    std::string("praenora.simulation: ") + error.what(),
                    json{{"engine_error", engine_error(error)}});
        return PRAENORA_STATUS_INTERNAL;
    } catch (...) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    "praenora.simulation: an unknown exception");
        return PRAENORA_STATUS_INTERNAL;
    }
}

// Holds a session or a plan for one call; fails with INVALID_STATE when another call holds it.
class Hold {
public:
    Hold(std::atomic<bool>& busy, const char* what);
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    ~Hold();

private:
    std::atomic<bool>& busy_;
};

// ---------------------------------------------------------------------------------------------
// The calling instance's host API.

// Whether the host is not NULL and its struct_size covers the callback at `offset`.
bool host_has(const praenora_host_api* host, std::size_t offset, std::size_t size) noexcept;

// Whether the host asks the call to stop (never for a host that has no cancel_requested).
bool cancel_requested(const praenora_host_api* host) noexcept;

// Fails with CANCELLED once the host asks the call to stop.
void check_cancelled(const praenora_host_api* host);

// Logs through the host, when it has a logger that records `level`.
void log_to(const praenora_host_api* host, std::int32_t level, const std::string& message);

// ---------------------------------------------------------------------------------------------
// Documents.

// The JSON object in `bytes`, at most PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE bytes; empty bytes
// are {} when `empty_is_object`.
json read_object(praenora_bytes bytes, const char* what, bool empty_is_object);

double finite_number(const json& value, const json::json_pointer& pointer);
const json& object_at(const json& document, const json::json_pointer& pointer);

// Fails for the first field of `object` (at `pointer`) that is not in `known`.
void only_fields(const json& object, const json::json_pointer& pointer,
                 std::initializer_list<const char*> known);

// Fails for the first top-level field that is not in `known`; `inputs` and `events_enabled` are
// ode-uq-spec fields the service does not map (UNSUPPORTED).
void top_level_fields(const json& document, std::initializer_list<const char*> known);

// The index of `id` in `ids`, or ids.size().
std::size_t index_of(const std::vector<std::string>& ids, const std::string& id);

// The indices of an array of distinct ids, each in `allowed`.
std::vector<std::size_t> id_list(const json& value, const json::json_pointer& pointer,
                                 const std::vector<std::string>& allowed, const char* what);

// The ids of a model, as describe_model lists them.
struct ModelIds {
    std::vector<std::string> states;
    std::vector<std::string> parameters;
    std::vector<std::string> quantities;
};

// The solver settings of a run: CVODE's, under the names of the service. `echo` is what results
// report (the settings used, after defaults).
struct SolverSettings {
    bool stiff = true;
    double relative_tolerance = 1e-6;
    double absolute_tolerance = 1e-12;
    std::int32_t max_steps = 20000;
    std::optional<double> max_step;
    std::optional<double> min_step;
    json echo = json::object();
};

// What a request or a plan fixes about a run.
struct RunSettings {
    double start = 0.0;
    double end = 0.0;
    std::vector<double> times;  // the output times; empty for solver_steps
    bool solver_steps = false;  // the solver's own steps (simulate only)
    bool include_states = true;
    std::vector<std::size_t> quantities;  // indices into ModelIds::quantities
    std::vector<std::string> columns;
    std::uint64_t max_rows = 0;  // the most rows a run may have (the run value limit)
    SolverSettings solver;
};

// The method the provider offers, and the only one.
inline constexpr const char* kMethod = "cvode";
inline constexpr const char* kFormat = "sbml";

// Reads time_domain, solver, output and checks of a request or plan. A plan cannot ask for the
// solver's steps (`plan`).
RunSettings read_settings(const json& document, const ModelIds& ids, bool plan);

// The values of one run, set over the model's: parameters (indices into ModelIds::parameters) and
// states at the start (indices into ModelIds::states).
struct RunValues {
    std::vector<std::pair<std::size_t, double>> parameters;
    std::vector<std::pair<std::size_t, double>> states;
};

// Fails unless every one of `count` values is finite (details.index: the first that is not).
void check_finite(const double* values, std::size_t count);

// The product of a and b, or SIZE_MAX when it does not fit.
std::size_t mul_or_max(std::uint64_t a, std::uint64_t b);

} // namespace praenora::roadrunner

#endif // PRAENORA_PROVIDERS_ROADRUNNER_DOCUMENTS_HPP
