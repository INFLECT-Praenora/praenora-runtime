#include "documents.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <typeinfo>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#include <cstdlib>
#endif

namespace praenora::roadrunner {

// ---------------------------------------------------------------------------------------------
// Failures.

void fail(praenora_status code, std::string message, json details) {
    throw Failure{code, std::move(message), std::move(details)};
}

json at_pointer(const json::json_pointer& pointer, json details) {
    details["pointer"] = pointer.to_string();
    return details;
}

void invalid(const std::string& message, const json::json_pointer& pointer, json details) {
    fail(PRAENORA_STATUS_INVALID_ARGUMENT, "praenora.simulation: " + message,
         at_pointer(pointer, std::move(details)));
}

void unsupported(const std::string& message, const json::json_pointer& pointer, json details) {
    fail(PRAENORA_STATUS_UNSUPPORTED, "praenora.simulation: " + message,
         at_pointer(pointer, std::move(details)));
}

json engine_error(const std::exception& error) {
    std::string type = typeid(error).name();
#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(type.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        type = demangled;
    }
    std::free(demangled);
#endif
    return {{"type", type}, {"message", error.what()}};
}

// ---------------------------------------------------------------------------------------------
// Sinks, inputs and calls.

bool usable(const praenora_sink* sink) noexcept {
    return sink != nullptr && sink->write != nullptr;
}

bool readable(praenora_bytes bytes) noexcept { return bytes.data != nullptr || bytes.size == 0; }

std::string_view view(praenora_bytes bytes) noexcept {
    return bytes.size == 0 ? std::string_view() : std::string_view(bytes.data, bytes.size);
}

void write_to(praenora_sink* sink, const std::string& text, const char* what) {
    const praenora_status status = sink->write(sink->context, text.data(), text.size());
    if (status != PRAENORA_STATUS_OK) {
        fail(status, std::string("praenora.simulation: the sink refused ") + what);
    }
}

void write_error(praenora_sink* error_json, praenora_status code, const std::string& message,
                 json details) noexcept {
    try {
        details["service"] = PRAENORA_SIMULATION_SERVICE;
        const std::string text =
            json{{"code", code}, {"message", message}, {"details", std::move(details)}}.dump();
        static_cast<void>(error_json->write(error_json->context, text.data(), text.size()));
    } catch (...) {
        // Out of memory: the status alone reports the failure.
    }
}

Hold::Hold(std::atomic<bool>& busy, const char* what) : busy_(busy) {
    if (busy_.exchange(true, std::memory_order_acquire)) {
        fail(PRAENORA_STATUS_INVALID_STATE,
             std::string("praenora.simulation: the ") + what + " is busy with another call");
    }
}

Hold::~Hold() { busy_.store(false, std::memory_order_release); }

// ---------------------------------------------------------------------------------------------
// The calling instance's host API.

bool host_has(const praenora_host_api* host, std::size_t offset, std::size_t size) noexcept {
    return host != nullptr && host->struct_size >= offset + size;
}

bool cancel_requested(const praenora_host_api* host) noexcept {
    return host_has(host, offsetof(praenora_host_api, cancel_requested),
                    sizeof host->cancel_requested) &&
           host->cancel_requested != nullptr && host->cancel_requested(host->context) != 0;
}

void check_cancelled(const praenora_host_api* host) {
    if (cancel_requested(host)) {
        fail(PRAENORA_STATUS_CANCELLED, "praenora.simulation: the call was cancelled");
    }
}

void log_to(const praenora_host_api* host, std::int32_t level, const std::string& message) {
    if (!host_has(host, offsetof(praenora_host_api, log_enabled), sizeof host->log_enabled) ||
        host->log == nullptr || host->log_enabled == nullptr ||
        host->log_enabled(host->context, level) == 0) {
        return;
    }
    host->log(host->context, level, message.data(), message.size());
}

// ---------------------------------------------------------------------------------------------
// Documents.

json read_object(praenora_bytes bytes, const char* what, bool empty_is_object) {
    if (bytes.size > PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE) {
        fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED,
             std::string("praenora.simulation: ") + what + " is larger than 16 MiB",
             {{"size", bytes.size}, {"limit", PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE}});
    }
    if (bytes.size == 0 && empty_is_object) {
        return json::object();
    }
    const std::string_view text = view(bytes);
    json document = json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT,
             std::string("praenora.simulation: ") + what + " is not a JSON object",
             at_pointer(json::json_pointer()));
    }
    return document;
}

double finite_number(const json& value, const json::json_pointer& pointer) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        invalid(pointer.to_string() + " is not a finite number", pointer);
    }
    return value.get<double>();
}

const json& object_at(const json& document, const json::json_pointer& pointer) {
    const json& value = document.at(pointer);
    if (!value.is_object()) {
        invalid(pointer.to_string() + " is not an object", pointer);
    }
    return value;
}

void only_fields(const json& object, const json::json_pointer& pointer,
                 std::initializer_list<const char*> known) {
    for (const auto& item : object.items()) {
        if (std::none_of(known.begin(), known.end(),
                         [&item](const char* name) { return item.key() == name; })) {
            invalid("unknown field " + (pointer / item.key()).to_string(), pointer / item.key());
        }
    }
}

void top_level_fields(const json& document, std::initializer_list<const char*> known) {
    const json::json_pointer root;
    for (const auto& item : document.items()) {
        if (item.key() == "inputs" || item.key() == "events_enabled") {
            unsupported(item.key() + " is not offered", root / item.key());
        }
        if (std::none_of(known.begin(), known.end(),
                         [&item](const char* name) { return item.key() == name; })) {
            invalid("unknown field /" + item.key(), root / item.key());
        }
    }
}

std::size_t index_of(const std::vector<std::string>& ids, const std::string& id) {
    return static_cast<std::size_t>(std::find(ids.begin(), ids.end(), id) - ids.begin());
}

std::vector<std::size_t> id_list(const json& value, const json::json_pointer& pointer,
                                 const std::vector<std::string>& allowed, const char* what) {
    if (!value.is_array()) {
        invalid(pointer.to_string() + " is not an array of ids", pointer);
    }
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const json::json_pointer at = pointer / i;
        if (!value.at(i).is_string()) {
            invalid(at.to_string() + " is not a string", at);
        }
        const std::string id = value.at(i).get<std::string>();
        const std::size_t index = index_of(allowed, id);
        if (index == allowed.size()) {
            invalid(std::string("the model has no ") + what + " '" + id + "'", at, {{"id", id}});
        }
        if (std::find(indices.begin(), indices.end(), index) != indices.end()) {
            invalid("'" + id + "' is listed twice", at, {{"id", id}});
        }
        indices.push_back(index);
    }
    return indices;
}

namespace {

// A positive finite number.
double positive_number(const json& value, const json::json_pointer& pointer) {
    const double number = finite_number(value, pointer);
    if (!(number > 0.0)) {
        invalid(pointer.to_string() + " must be positive", pointer);
    }
    return number;
}

// The settings of the method cvode, CVODE's (SUNDIALS) as libRoadRunner drives it.
SolverSettings read_solver(const json& document) {
    const json::json_pointer at = json::json_pointer() / "solver";
    SolverSettings settings;
    if (document.contains("solver")) {
        const json& solver = object_at(document, at);
        only_fields(solver, at,
                    {"method", "stiffness", "relative_tolerance", "absolute_tolerance",
                     "max_steps", "max_step", "min_step", "seed", "event_time_tolerance"});
        if (solver.contains("method")) {
            const json& method = solver.at("method");
            if (!method.is_string()) {
                invalid("solver.method is not a string", at / "method");
            }
            if (method.get<std::string>() != kMethod) {
                unsupported("method '" + method.get<std::string>() + "' is not offered",
                            at / "method", {{"method", method}, {"methods", {kMethod}}});
            }
        }
        for (const char* unmapped : {"seed", "event_time_tolerance"}) {
            if (solver.contains(unmapped)) {
                unsupported(std::string("method cvode has no setting ") + unmapped,
                            at / unmapped);
            }
        }
        if (solver.contains("stiffness")) {
            const json& stiffness = solver.at("stiffness");
            const std::string value = stiffness.is_string() ? stiffness.get<std::string>() : "";
            if (value == "stiff" || value == "nonstiff") {
                settings.stiff = value == "stiff";
            } else if (value == "auto") {
                unsupported("method cvode does not switch between stiff and nonstiff by itself",
                            at / "stiffness");
            } else {
                invalid("solver.stiffness is \"stiff\", \"nonstiff\" or \"auto\"",
                        at / "stiffness");
            }
        }
        if (solver.contains("relative_tolerance")) {
            settings.relative_tolerance =
                positive_number(solver.at("relative_tolerance"), at / "relative_tolerance");
        }
        if (solver.contains("absolute_tolerance")) {
            settings.absolute_tolerance =
                positive_number(solver.at("absolute_tolerance"), at / "absolute_tolerance");
        }
        if (solver.contains("max_steps")) {
            const json& value = solver.at("max_steps");
            const double steps = value.is_number() ? value.get<double>() : 0.0;
            const double most = static_cast<double>(std::numeric_limits<std::int32_t>::max());
            if (!value.is_number() || !std::isfinite(steps) || steps != std::floor(steps) ||
                steps < 1.0 || steps > most) {
                invalid("solver.max_steps is an integer from 1 to 2147483647",
                        at / "max_steps");
            }
            settings.max_steps = static_cast<std::int32_t>(steps);
        }
        if (solver.contains("max_step")) {
            settings.max_step = positive_number(solver.at("max_step"), at / "max_step");
        }
        if (solver.contains("min_step")) {
            const double step = finite_number(solver.at("min_step"), at / "min_step");
            if (step < 0.0) {
                invalid("solver.min_step must not be negative", at / "min_step");
            }
            if (settings.max_step.has_value() && step > *settings.max_step) {
                invalid("solver.min_step is larger than solver.max_step", at / "min_step");
            }
            settings.min_step = step;
        }
    }
    settings.echo = {{"method", kMethod},
                     {"stiffness", settings.stiff ? "stiff" : "nonstiff"},
                     {"relative_tolerance", settings.relative_tolerance},
                     {"absolute_tolerance", settings.absolute_tolerance},
                     {"max_steps", settings.max_steps}};
    if (settings.max_step.has_value()) {
        settings.echo["max_step"] = *settings.max_step;
    }
    if (settings.min_step.has_value()) {
        settings.echo["min_step"] = *settings.min_step;
    }
    return settings;
}

} // namespace

RunSettings read_settings(const json& document, const ModelIds& ids, bool plan) {
    const json::json_pointer root;
    RunSettings settings;

    if (!document.contains("time_domain")) {
        invalid("the time domain is missing", root / "time_domain");
    }
    const json& domain = object_at(document, root / "time_domain");
    only_fields(domain, root / "time_domain", {"start", "end"});
    for (const char* bound : {"start", "end"}) {
        if (!domain.contains(bound)) {
            invalid(std::string("time_domain.") + bound + " is missing",
                    root / "time_domain" / bound);
        }
    }
    settings.start = finite_number(domain.at("start"), root / "time_domain" / "start");
    settings.end = finite_number(domain.at("end"), root / "time_domain" / "end");
    if (!(settings.end > settings.start)) {
        invalid("time_domain.end must be later than time_domain.start",
                root / "time_domain" / "end");
    }

    settings.solver = read_solver(document);

    if (!document.contains("output")) {
        invalid("the output is missing", root / "output");
    }
    const json::json_pointer out = root / "output";
    const json& output = object_at(document, out);
    only_fields(output, out,
                {"save_at", "times", "step", "quantities", "include_states", "dense_output"});
    if (!output.contains("save_at") || !output.at("save_at").is_string()) {
        invalid("output.save_at must name how output times are chosen", out / "save_at");
    }
    if (output.contains("include_states")) {
        if (!output.at("include_states").is_boolean()) {
            invalid("output.include_states is not a boolean", out / "include_states");
        }
        settings.include_states = output.at("include_states").get<bool>();
    }
    if (output.contains("quantities")) {
        settings.quantities =
            id_list(output.at("quantities"), out / "quantities", ids.quantities, "quantity");
    }
    if (output.contains("dense_output")) {
        if (!output.at("dense_output").is_boolean()) {
            invalid("output.dense_output is not a boolean", out / "dense_output");
        }
        if (output.at("dense_output").get<bool>()) {
            unsupported("dense output is not offered", out / "dense_output");
        }
    }
    settings.columns = {"time"};
    if (settings.include_states) {
        settings.columns.insert(settings.columns.end(), ids.states.begin(), ids.states.end());
    }
    for (const std::size_t index : settings.quantities) {
        settings.columns.push_back(ids.quantities[index]);
    }
    const std::uint64_t columns = settings.columns.size();
    settings.max_rows = PRAENORA_SIMULATION_MAX_RUN_VALUES / columns;
    const auto too_many = [&](double rows) {
        fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED,
             "praenora.simulation: a run would have more than 4194304 output values",
             {{"rows", rows}, {"columns", columns}, {"limit", PRAENORA_SIMULATION_MAX_RUN_VALUES}});
    };

    const std::string save_at = output.at("save_at").get<std::string>();
    if (save_at == "explicit_times") {
        if (output.contains("step")) {
            invalid("output.step applies only to regular_grid", out / "step");
        }
        if (!output.contains("times") || !output.at("times").is_array() ||
            output.at("times").empty()) {
            invalid("output.times must be a non-empty array of times", out / "times");
        }
        const json& times = output.at("times");
        if (times.size() > settings.max_rows) {
            too_many(static_cast<double>(times.size()));
        }
        for (std::size_t i = 0; i < times.size(); ++i) {
            const double t = finite_number(times.at(i), out / "times" / i);
            if (t < settings.start || t > settings.end) {
                invalid("output time " + json(t).dump() + " is outside the time domain",
                        out / "times" / i);
            }
            if (!settings.times.empty() && !(t > settings.times.back())) {
                invalid("output times must increase", out / "times" / i);
            }
            settings.times.push_back(t);
        }
    } else if (save_at == "regular_grid") {
        if (output.contains("times")) {
            invalid("output.times applies only to explicit_times", out / "times");
        }
        if (!output.contains("step")) {
            invalid("output.step is missing", out / "step");
        }
        const double step = finite_number(output.at("step"), out / "step");
        if (!(step > 0.0)) {
            invalid("output.step must be positive", out / "step");
        }
        const double last = std::floor((settings.end - settings.start) / step + 1e-9);
        if (!(last + 1.0 <= static_cast<double>(settings.max_rows))) {
            too_many(last + 1.0);
        }
        const auto count = static_cast<std::size_t>(last) + 1;
        for (std::size_t i = 0; i < count; ++i) {
            const double t =
                std::min(settings.start + static_cast<double>(i) * step, settings.end);
            if (!settings.times.empty() && !(t > settings.times.back())) {
                invalid("output.step is below the precision of the times: they do not increase",
                        out / "step");
            }
            settings.times.push_back(t);
        }
    } else if (save_at == "solver_steps") {
        if (plan) {
            unsupported("a plan has a fixed number of rows, so it has no solver_steps output",
                        out / "save_at");
        }
        for (const char* field : {"times", "step"}) {
            if (output.contains(field)) {
                invalid(std::string("output.") + field + " does not apply to solver_steps",
                        out / field);
            }
        }
        settings.solver_steps = true;
    } else if (save_at == "target_times") {
        unsupported("target_times is not offered: give the times as explicit_times",
                    out / "save_at");
    } else {
        invalid("output.save_at '" + save_at + "' is not a way of choosing output times",
                out / "save_at");
    }

    if (document.contains("checks")) {
        const json& checks = object_at(document, root / "checks");
        only_fields(checks, root / "checks", {"reject_nonfinite", "enforce_time_domain"});
        for (const auto& item : checks.items()) {
            if (!item.value().is_boolean()) {
                invalid("checks." + item.key() + " is not a boolean", root / "checks" / item.key());
            }
            if (!item.value().get<bool>()) {
                unsupported("checks." + item.key() + " is always on",
                            root / "checks" / item.key());
            }
        }
    }
    return settings;
}

void check_finite(const double* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "praenora.simulation: input value " + std::to_string(i) + " is not finite",
                 {{"index", i}});
        }
    }
}

std::size_t mul_or_max(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::numeric_limits<std::size_t>::max();
    }
    const std::uint64_t product = a * b;
    return product > std::numeric_limits<std::size_t>::max()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(product);
}

} // namespace praenora::roadrunner
