// The roadrunner provider: offers praenora.simulation@0.0 (include/praenora/sdk/simulation.h;
// normative: docs/specs/simulation-service.md) for SBML models, with libRoadRunner as its engine.
// This file holds the provider interface (src/runtime/provider/provider_abi.h), the service's table
// and the objects it hands out; engine.cpp holds what uses libRoadRunner, documents.cpp what reads
// the service's documents. No exception leaves a function of the provider.
#include "documents.hpp"
#include "engine.hpp"
#include "provider_abi.h"

#include <praenora/sdk/simulation.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using praenora::roadrunner::check_cancelled;
using praenora::roadrunner::check_finite;
using praenora::roadrunner::CompiledModel;
using praenora::roadrunner::fail;
using praenora::roadrunner::Hold;
using praenora::roadrunner::json;
using praenora::roadrunner::KernelModel;
using praenora::roadrunner::RunSettings;
using praenora::roadrunner::RunValues;
using praenora::roadrunner::Simulator;

// The provider's objects. Opaque to modules (simulation.h declares the types).
struct praenora_simulation_session {
    std::shared_ptr<const CompiledModel> model;
    std::unique_ptr<Simulator> simulator; // made by the session's first simulate()
    std::atomic<bool> busy{false};
};

struct praenora_simulation_plan {
    std::shared_ptr<const CompiledModel> model;
    std::unique_ptr<Simulator> simulator;
    RunSettings settings;
    // The inputs: parameters (indices into the model's parameters) then states.
    std::vector<std::size_t> parameters;
    std::vector<std::size_t> states;
    std::atomic<bool> busy{false};
};

namespace {

using praenora::roadrunner::guarded;
using praenora::roadrunner::id_list;
using praenora::roadrunner::index_of;
using praenora::roadrunner::invalid;
using praenora::roadrunner::kFormat;
using praenora::roadrunner::kMethod;
using praenora::roadrunner::log_to;
using praenora::roadrunner::mul_or_max;
using praenora::roadrunner::object_at;
using praenora::roadrunner::only_fields;
using praenora::roadrunner::read_object;
using praenora::roadrunner::read_settings;
using praenora::roadrunner::readable;
using praenora::roadrunner::top_level_fields;
using praenora::roadrunner::unsupported;
using praenora::roadrunner::usable;
using praenora::roadrunner::write_to;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The result document of one simulation.
json result_of(const RunSettings& settings, const json& failure, const std::vector<double>& out,
               std::size_t rows) {
    if (!failure.is_null()) {
        return {{"ok", false}, {"error", failure}};
    }
    const std::size_t columns = settings.columns.size();
    json table = json::array();
    for (std::size_t row = 0; row < rows; ++row) {
        const auto first = out.begin() + static_cast<std::ptrdiff_t>(row * columns);
        table.push_back(
            std::vector<double>(first, first + static_cast<std::ptrdiff_t>(columns)));
    }
    return {{"ok", true},
            {"columns", settings.columns},
            {"rows", std::move(table)},
            {"solver", settings.solver.echo}};
}

// The values of one set of a plan's inputs.
RunValues values_of(const praenora_simulation_plan& plan, const double* inputs) {
    RunValues values;
    std::size_t i = 0;
    for (const std::size_t parameter : plan.parameters) {
        values.parameters.emplace_back(parameter, inputs[i++]);
    }
    for (const std::size_t state : plan.states) {
        values.states.emplace_back(state, inputs[i++]);
    }
    return values;
}

std::size_t run_size(const praenora_simulation_plan& plan) {
    return plan.settings.times.size() * plan.settings.columns.size();
}

// ---------------------------------------------------------------------------------------------
// Kernel functions: the kernel's own libRoadRunner object, no host, no exception.

KernelModel* kernel_of(void* model) noexcept { return static_cast<KernelModel*>(model); }

template <class Body> praenora_status kernel_call(Body&& body) noexcept {
    try {
        body();
        return PRAENORA_STATUS_OK;
    } catch (...) {
        return PRAENORA_STATUS_INTERNAL;
    }
}

praenora_status PRAENORA_CALL kernel_rhs(void* model, double t, const double* y,
                                         double* dydt) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr ||
        (kernel->state_count() != 0 && (y == nullptr || dydt == nullptr))) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return kernel_call([&] { kernel->rhs(t, y, dydt); });
}

praenora_status PRAENORA_CALL kernel_set_parameters(void* model, const double* values) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || (values == nullptr && kernel->parameter_count() != 0)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return kernel_call([&] { kernel->set_parameters(values); });
}

praenora_status PRAENORA_CALL kernel_initial_state(void* model, double* y) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || (y == nullptr && kernel->state_count() != 0)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return kernel_call([&] { kernel->initial_state(y); });
}

praenora_status PRAENORA_CALL kernel_quantities(void* model, double t, const double* y,
                                                double* out) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || (y == nullptr && kernel->state_count() != 0) ||
        (out == nullptr && kernel->quantity_count() != 0)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return kernel_call([&] { kernel->quantities(t, y, out); });
}

// ---------------------------------------------------------------------------------------------
// The service's functions.

const std::string& description() {
    static const std::string text =
        json{{"service", PRAENORA_SIMULATION_SERVICE},
             {"version",
              {{"major", PRAENORA_SIMULATION_SERVICE_MAJOR},
               {"minor", PRAENORA_SIMULATION_SERVICE_MINOR}}},
             {"engine", json::parse(PRAENORA_PROVIDER_ENGINE)},
             {"model_formats", {kFormat}},
             {"methods", {kMethod}}}
            .dump();
    return text;
}

praenora_status PRAENORA_CALL describe(void*, praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    try {
        const std::string& text = description();
        return out_json->write(out_json->context, text.data(), text.size());
    } catch (...) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
}

praenora_status PRAENORA_CALL open_session(void*, const praenora_host_api* host,
                                           praenora_bytes model, praenora_bytes options_json,
                                           praenora_simulation_session** out_session,
                                           praenora_sink* error_json) noexcept {
    if (out_session != nullptr) {
        *out_session = nullptr;
    }
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (out_session == nullptr || !readable(model) || !readable(options_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "open_session: out_session, the model or the options is NULL");
        }
        if (model.size > PRAENORA_SIMULATION_MAX_MODEL_SIZE) {
            fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED,
                 "praenora.simulation: the model text is larger than 64 MiB",
                 {{"size", model.size}, {"limit", PRAENORA_SIMULATION_MAX_MODEL_SIZE}});
        }
        check_cancelled(host);
        const json options = read_object(options_json, "the options", true);
        only_fields(options, json::json_pointer(), {"format"});
        if (options.contains("format")) {
            const json& format = options.at("format");
            if (!format.is_string()) {
                invalid("the model format is not a string", json::json_pointer("/format"));
            }
            if (format.get<std::string>() != kFormat) {
                unsupported("model format '" + format.get<std::string>() + "' is not offered",
                            json::json_pointer("/format"),
                            {{"format", format}, {"model_formats", {kFormat}}});
            }
        }
        auto session = std::make_unique<praenora_simulation_session>();
        session->model = praenora::roadrunner::compiled_model(
            praenora::roadrunner::view(model), host);
        log_to(host, PRAENORA_LOG_DEBUG,
               "roadrunner: opened an SBML model of " +
                   std::to_string(session->model->ids().states.size()) + " states");
        *out_session = session.release();
    });
}

praenora_status PRAENORA_CALL describe_model(praenora_simulation_session* session,
                                             const praenora_host_api* host,
                                             praenora_sink* out_json,
                                             praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (session == nullptr || !usable(out_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "describe_model: the session or out_json is NULL");
        }
        const Hold hold(session->busy, "session");
        check_cancelled(host);
        write_to(out_json, session->model->description(), "the model description");
    });
}

praenora_status PRAENORA_CALL simulate(praenora_simulation_session* session,
                                       const praenora_host_api* host, praenora_bytes request_json,
                                       praenora_sink* result_json,
                                       praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (session == nullptr || !usable(result_json) || !readable(request_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "simulate: the session, the request or result_json is NULL");
        }
        const Hold hold(session->busy, "session");
        check_cancelled(host);
        const CompiledModel& model = *session->model;
        const json request = read_object(request_json, "the request", false);
        top_level_fields(request, {"time_domain", "initial_conditions", "parameter_values",
                                   "solver", "output", "checks"});
        RunValues values;
        const json::json_pointer root;
        for (const char* field : {"parameter_values", "initial_conditions"}) {
            if (!request.contains(field)) {
                continue;
            }
            const bool states = std::strcmp(field, "initial_conditions") == 0;
            const std::vector<std::string>& ids =
                states ? model.ids().states : model.ids().parameters;
            for (const auto& item : object_at(request, root / field).items()) {
                const json::json_pointer pointer = root / field / item.key();
                const std::size_t index = index_of(ids, item.key());
                if (index == ids.size()) {
                    invalid(std::string("the model has no ") + (states ? "state" : "parameter") +
                                " '" + item.key() + "'",
                            pointer, {{"id", item.key()}});
                }
                const double value = praenora::roadrunner::finite_number(item.value(), pointer);
                (states ? values.states : values.parameters).emplace_back(index, value);
            }
        }
        const RunSettings settings = read_settings(request, model.ids(), false);
        if (!session->simulator) {
            session->simulator = std::make_unique<Simulator>(session->model, host);
        }
        std::vector<double> out;
        std::size_t rows = 0;
        json failure;
        if (settings.solver_steps) {
            failure = session->simulator->run_steps(settings, values, out, rows, host);
        } else {
            rows = settings.times.size();
            out.assign(rows * settings.columns.size(), 0.0);
            failure = session->simulator->run(settings, values, out.data(), host);
        }
        log_to(host, PRAENORA_LOG_DEBUG,
               "roadrunner: simulated " + std::to_string(rows) + " rows");
        write_to(result_json, result_of(settings, failure, out, rows).dump(), "the result");
    });
}

praenora_status PRAENORA_CALL prepare(praenora_simulation_session* session,
                                      const praenora_host_api* host, praenora_bytes plan_json,
                                      praenora_simulation_plan** out_plan, praenora_sink* info_json,
                                      praenora_sink* error_json) noexcept {
    if (out_plan != nullptr) {
        *out_plan = nullptr;
    }
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (session == nullptr || out_plan == nullptr || !usable(info_json) ||
            !readable(plan_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "prepare: the session, the plan document, out_plan or info_json is NULL");
        }
        const Hold hold(session->busy, "session");
        check_cancelled(host);
        const json document = read_object(plan_json, "the plan", false);
        top_level_fields(document, {"parameters", "initial_conditions", "time_domain", "solver",
                                    "output", "checks"});
        auto plan = std::make_unique<praenora_simulation_plan>();
        plan->model = session->model;
        const CompiledModel& model = *plan->model;
        const json::json_pointer root;
        std::vector<std::string> inputs;
        for (const char* field : {"parameters", "initial_conditions"}) {
            if (!document.contains(field)) {
                continue;
            }
            const bool parameters = std::strcmp(field, "parameters") == 0;
            const std::vector<std::string>& ids =
                parameters ? model.ids().parameters : model.ids().states;
            const std::vector<std::size_t> indices = id_list(
                document.at(field), root / field, ids, parameters ? "parameter" : "state");
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const std::string& id = ids[indices[i]];
                if (std::find(inputs.begin(), inputs.end(), id) != inputs.end()) {
                    invalid("'" + id + "' is an input already", root / field / i, {{"id", id}});
                }
                inputs.push_back(id);
                (parameters ? plan->parameters : plan->states).push_back(indices[i]);
            }
        }
        plan->settings = read_settings(document, model.ids(), true);
        plan->simulator = std::make_unique<Simulator>(plan->model, host);
        write_to(info_json,
                 json{{"inputs", inputs},
                      {"columns", plan->settings.columns},
                      {"rows", plan->settings.times.size()},
                      {"solver", plan->settings.solver.echo}}
                     .dump(),
                 "the plan's description");
        *out_plan = plan.release();
    });
}

// One run of a plan into `out`; a numerical failure fills it with NaN.
json run_once(praenora_simulation_plan& plan, const praenora_host_api* host, const double* values,
              double* out) {
    const json failure = plan.simulator->run(plan.settings, values_of(plan, values), out, host);
    if (!failure.is_null()) {
        std::fill(out, out + run_size(plan), kNaN);
    }
    return failure;
}

praenora_status PRAENORA_CALL run(praenora_simulation_plan* plan, const praenora_host_api* host,
                                  const double* values, std::uint64_t value_count, double* out,
                                  std::uint64_t out_count, std::int32_t* outcome,
                                  praenora_sink* failure_json,
                                  praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (plan == nullptr || outcome == nullptr ||
            (failure_json != nullptr && failure_json->write == nullptr)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "run: the plan or outcome is NULL, or failure_json has no write function");
        }
        const std::size_t inputs = plan->parameters.size() + plan->states.size();
        const std::size_t size = run_size(*plan);
        if (value_count != inputs || out_count != size) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "run: the plan takes " + std::to_string(inputs) + " values and writes " +
                     std::to_string(size),
                 {{"value_count", value_count},
                  {"out_count", out_count},
                  {"inputs", inputs},
                  {"outputs", size}});
        }
        if ((values == nullptr && inputs != 0) || (out == nullptr && size != 0)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "run: values or out is NULL");
        }
        const Hold hold(plan->busy, "plan");
        check_cancelled(host);
        check_finite(values, inputs);
        const json failure = run_once(*plan, host, values, out);
        if (failure.is_null()) {
            *outcome = PRAENORA_SIMULATION_RUN_OK;
            return;
        }
        *outcome = PRAENORA_SIMULATION_RUN_FAILED;
        if (failure_json != nullptr) {
            write_to(failure_json, failure.dump(), "the failure document");
        }
    });
}

praenora_status PRAENORA_CALL run_batch(praenora_simulation_plan* plan,
                                        const praenora_host_api* host, const double* values,
                                        std::uint64_t value_count, std::uint64_t set_count,
                                        double* out, std::uint64_t out_count,
                                        std::int32_t* outcomes,
                                        praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (plan == nullptr || (outcomes == nullptr && set_count != 0)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "run_batch: the plan or outcomes is NULL");
        }
        const std::size_t inputs = plan->parameters.size() + plan->states.size();
        const std::size_t size = run_size(*plan);
        const std::size_t all_inputs = mul_or_max(set_count, inputs);
        const std::size_t all_outputs = mul_or_max(set_count, size);
        const std::size_t max = std::numeric_limits<std::size_t>::max();
        if (all_inputs == max || all_outputs == max || value_count != all_inputs ||
            out_count != all_outputs) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "run_batch: each set takes " + std::to_string(inputs) + " values and writes " +
                     std::to_string(size),
                 {{"value_count", value_count},
                  {"out_count", out_count},
                  {"set_count", set_count},
                  {"inputs", inputs},
                  {"outputs", size}});
        }
        if ((values == nullptr && all_inputs != 0) || (out == nullptr && all_outputs != 0)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT, "run_batch: values or out is NULL");
        }
        const Hold hold(plan->busy, "plan");
        check_finite(values, all_inputs);
        for (std::size_t set = 0; set < set_count; ++set) {
            check_cancelled(host); // before work starts, then between the sets
            const double* set_values = inputs == 0 ? values : values + set * inputs;
            const json failure = run_once(*plan, host, set_values, out + set * size);
            outcomes[set] =
                failure.is_null() ? PRAENORA_SIMULATION_RUN_OK : PRAENORA_SIMULATION_RUN_FAILED;
        }
    });
}

praenora_status PRAENORA_CALL open_kernel(praenora_simulation_session* session,
                                          const praenora_host_api* host,
                                          praenora_bytes options_json,
                                          praenora_simulation_kernel* out_kernel,
                                          praenora_sink* error_json) noexcept {
    const bool fits =
        out_kernel != nullptr && out_kernel->struct_size >= PRAENORA_SIMULATION_KERNEL_V0_SIZE;
    if (fits) {
        out_kernel->model = nullptr;
    }
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        if (session == nullptr || !fits || !readable(options_json)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "open_kernel: the session or the options is NULL, or out_kernel is NULL or "
                 "smaller than a v0 kernel");
        }
        const Hold hold(session->busy, "session");
        check_cancelled(host);
        const CompiledModel& model = *session->model;
        if (model.events()) {
            // A kernel is the continuous right-hand side only: it cannot apply events.
            fail(PRAENORA_STATUS_UNSUPPORTED,
                 "praenora.simulation: the model has events, which a kernel cannot apply",
                 {{"reason", "events"}});
        }
        const json options = read_object(options_json, "the kernel options", true);
        only_fields(options, json::json_pointer(), {"parameters", "quantities"});
        const json::json_pointer root;
        std::vector<int> parameters;
        if (options.contains("parameters")) {
            for (const std::size_t index : id_list(options.at("parameters"), root / "parameters",
                                                   model.ids().parameters, "parameter")) {
                parameters.push_back(model.parameter_indices()[index]);
            }
        } else {
            parameters = model.parameter_indices();
        }
        std::vector<praenora::roadrunner::Quantity> quantities;
        if (options.contains("quantities")) {
            for (const std::size_t index : id_list(options.at("quantities"), root / "quantities",
                                                   model.ids().quantities, "quantity")) {
                quantities.push_back(model.quantities()[index]);
            }
        } else {
            quantities = model.quantities();
        }
        auto kernel = std::make_unique<KernelModel>(session->model, std::move(parameters),
                                                    std::move(quantities), host);
        out_kernel->struct_size = static_cast<std::uint32_t>(
            std::min<std::size_t>(out_kernel->struct_size, sizeof(praenora_simulation_kernel)));
        out_kernel->reserved = 0u;
        out_kernel->state_count = kernel->state_count();
        out_kernel->parameter_count = kernel->parameter_count();
        out_kernel->quantity_count = kernel->quantity_count();
        out_kernel->rhs = &kernel_rhs;
        out_kernel->set_parameters = &kernel_set_parameters;
        out_kernel->initial_state = &kernel_initial_state;
        out_kernel->quantities = &kernel_quantities;
        out_kernel->model = kernel.release();
    });
}

void PRAENORA_CALL close_kernel(praenora_simulation_kernel* kernel) noexcept {
    if (kernel != nullptr && kernel->model != nullptr) {
        delete kernel_of(kernel->model);
        kernel->model = nullptr;
    }
}

void PRAENORA_CALL close_plan(praenora_simulation_plan* plan) noexcept { delete plan; }

void PRAENORA_CALL close_session(praenora_simulation_session* session) noexcept {
    delete session;
}

const praenora_simulation_v0 simulation_table = {
    {PRAENORA_SIMULATION_SERVICE_MAJOR, PRAENORA_SIMULATION_SERVICE_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_simulation_v0)), 0u},
    nullptr,
    &describe,
    &open_session,
    &describe_model,
    &simulate,
    &prepare,
    &run,
    &run_batch,
    &open_kernel,
    &close_kernel,
    &close_plan,
    &close_session,
};

// ---------------------------------------------------------------------------------------------
// The provider.

const char kDescription[] = "{\"name\":\"" PRAENORA_PROVIDER_NAME
                            "\",\"version\":\"" PRAENORA_PROVIDER_VERSION
                            "\",\"services\":" PRAENORA_PROVIDER_SERVICES
                            ",\"engine\":" PRAENORA_PROVIDER_ENGINE "}";

praenora_status PRAENORA_CALL provider_describe(praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return out_json->write(out_json->context, kDescription, std::strlen(kDescription));
}

praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                    praenora_sink* error_json) noexcept {
    try {
        praenora::roadrunner::start_engine(host);
        return PRAENORA_STATUS_OK;
    } catch (const std::exception& error) {
        if (usable(error_json)) {
            praenora::roadrunner::write_error(
                error_json, PRAENORA_STATUS_INTERNAL,
                std::string("roadrunner: libRoadRunner did not start: ") + error.what(),
                {{"engine_error", praenora::roadrunner::engine_error(error)}});
        }
    } catch (...) {
        if (usable(error_json)) {
            praenora::roadrunner::write_error(error_json, PRAENORA_STATUS_INTERNAL,
                                              "roadrunner: libRoadRunner did not start");
        }
    }
    return PRAENORA_STATUS_INTERNAL;
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) noexcept {
    const std::size_t length = std::strlen(PRAENORA_SIMULATION_SERVICE);
    if (major == PRAENORA_SIMULATION_SERVICE_MAJOR && name != nullptr && name_size == length &&
        std::memcmp(name, PRAENORA_SIMULATION_SERVICE, length) == 0) {
        return &simulation_table;
    }
    return nullptr;
}

const praenora_provider_api_v0 provider_table = {
    {PRAENORA_PROVIDER_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_provider_api_v0)), 0u},
    &provider_describe,
    &start,
    &get_service,
};

} // namespace

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) {
    return &provider_table;
}
