// test-simulation: the reference provider of the simulation service praenora.simulation@0.0
// (include/praenora/sdk/simulation.h; normative: docs/specs/simulation-service.md) for tests. It
// needs no engine: it implements every rule of the service for a toy model format, so that the
// service checks (tests/contract/simulation_service_checks.hpp) and modules that use the service
// can run without one.
//
// Model format "test-decay": the model text is a JSON object
//     {"states": {"<id>": <x0 or a parameter id>, ...},
//      "parameters": {"k_<id>": <k>, ..., "<other id>": <value>, ...},
//      "events": [{"time": <t>, "state": "<id>", "value": <v>}, ...]}
// with at least one state, ids matching [A-Za-z_][A-Za-z0-9_]* (states not "total"), one parameter
// k_<id> for each state <id>, and only those parameters and the ones that states name as their
// initial values, all finite numbers. A state whose initial value names a parameter starts at that
// parameter's value: parameter values take effect as initial values. Each state decays on its own,
// dx/dt = -k_x * x, so a run from `start` has x(t) = x(start) * exp(-k_x * (t - start)), computed
// exactly ("solved analytically"); the only method is "analytic", which has no solver settings.
// The states and the parameters are listed sorted by their ids (UTF-8 bytes); the quantities are
// the states, then "total", the sum of the states in that order. A value that is not finite (a
// negative k that overflows, for example) makes the run fail with reason "nonfinite".
// "events" (optional) declares events, which this provider does not simulate: simulate and prepare
// refuse a model that has them (UNSUPPORTED, reason "events"), a property of the toy format; like
// every provider of the service, open_kernel refuses it too, and describe_model reports them.
//
// Shared compiled models (docs/specs/process-isolation.md#shared-compiled-models). The provider's
// "compiled" form of a model is the canonical parsed model (its JSON document as nlohmann dumps
// it). Where its provider host offers the store (load_compiled and save_compiled), open_session
// looks for the model there first, under the key "test-decay:" and the FNV-1a hash of the model's
// text, and keeps it there after it compiled it; an entry holds the text too, and one that is not
// that model's, or does not parse, is not used (a warning, and the model is compiled). It logs at
// debug through the calling instance's host "test-simulation: compiled the model" or
// "test-simulation: loaded the model from the shared store".
//
// C++17 with nlohmann/json. No exception leaves a function of the provider.
#include "provider_fixture.h"

#include <praenora/sdk/simulation.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

constexpr const char* kFormat = "test-decay";
constexpr const char* kMethod = "analytic";
constexpr const char* kTotal = "total";

// ---------------------------------------------------------------------------------------------
// Failures and the documents that report them.

// A failed call: its status, message and details (an object, to which "service" is added).
struct Failure {
    praenora_status code;
    std::string message;
    json details;
};

[[noreturn]] void fail(praenora_status code, std::string message,
                       json details = json::object()) {
    throw Failure{code, std::move(message), std::move(details)};
}

// The details of a problem at a JSON pointer of a document.
json at_pointer(const json::json_pointer& pointer, json details = json::object()) {
    details["pointer"] = pointer.to_string();
    return details;
}

bool usable(const praenora_sink* sink) noexcept {
    return sink != nullptr && sink->write != nullptr;
}

bool readable(praenora_bytes bytes) noexcept { return bytes.data != nullptr || bytes.size == 0; }

std::string_view view(praenora_bytes bytes) noexcept {
    return bytes.size == 0 ? std::string_view() : std::string_view(bytes.data, bytes.size);
}

// Writes `text` to `sink`; a refused write fails the call with the sink's status.
void write_to(praenora_sink* sink, const std::string& text, const char* what) {
    const praenora_status status = sink->write(sink->context, text.data(), text.size());
    if (status != PRAENORA_STATUS_OK) {
        fail(status, std::string("praenora.simulation: the sink refused ") + what);
    }
}

// The error document of `failure`, written when the sink takes it.
void write_error(praenora_sink* error_json, praenora_status code, const std::string& message,
                 json details = json::object()) noexcept {
    try {
        details["service"] = PRAENORA_SIMULATION_SERVICE;
        const std::string text =
            json{{"code", code}, {"message", message}, {"details", std::move(details)}}.dump();
        static_cast<void>(error_json->write(error_json->context, text.data(), text.size()));
    } catch (...) {
        // Out of memory: the status alone reports the failure.
    }
}

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
                    std::string("praenora.simulation: ") + error.what());
        return PRAENORA_STATUS_INTERNAL;
    } catch (...) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    "praenora.simulation: an unknown exception");
        return PRAENORA_STATUS_INTERNAL;
    }
}

// ---------------------------------------------------------------------------------------------
// The host API of the calling instance.

bool host_has(const praenora_host_api* host, std::size_t offset, std::size_t size) noexcept {
    return host != nullptr && host->struct_size >= offset + size;
}

// Fails with CANCELLED once the host asks the call to stop.
void check_cancelled(const praenora_host_api* host) {
    if (host_has(host, offsetof(praenora_host_api, cancel_requested),
                 sizeof host->cancel_requested) &&
        host->cancel_requested != nullptr && host->cancel_requested(host->context) != 0) {
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
// The model.

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

struct Model {
    std::vector<std::string> states;           // sorted
    std::vector<double> initial;               // the initial value of each state that is a number
    std::vector<std::size_t> initial_parameter; // the parameter a state starts at, or kNone
    std::vector<std::string> parameters;       // sorted
    std::vector<double> values;                // the value of each parameter
    std::vector<std::size_t> rate;             // the parameter k_<state> of each state
    std::vector<std::string> quantities;       // the states, then "total"
    bool events = false;                       // whether the model declares events
};

// The initial state for the parameter values `values`: the initial assignments evaluated.
std::vector<double> initial_state(const Model& model, const std::vector<double>& values) {
    std::vector<double> state = model.initial;
    for (std::size_t i = 0; i < state.size(); ++i) {
        if (model.initial_parameter[i] != kNone) {
            state[i] = values[model.initial_parameter[i]];
        }
    }
    return state;
}

// The index of `id` in `ids`, or ids.size().
std::size_t index_of(const std::vector<std::string>& ids, const std::string& id) {
    return static_cast<std::size_t>(std::find(ids.begin(), ids.end(), id) - ids.begin());
}

bool is_identifier(const std::string& id) {
    const auto letter = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    return !id.empty() && letter(id[0]) && std::all_of(id.begin(), id.end(), [&letter](char ch) {
        return letter(ch) || (ch >= '0' && ch <= '9');
    });
}

[[noreturn]] void model_invalid(const std::string& message, const json::json_pointer& pointer) {
    fail(PRAENORA_STATUS_DATA_INVALID,
         "praenora.simulation: the model is not a test-decay model: " + message,
         at_pointer(pointer, {{"reason", "model"}}));
}

bool finite_number_value(const json& value) {
    return value.is_number() && std::isfinite(value.get<double>());
}

// The model of a document parsed from a model text.
std::shared_ptr<const Model> read_model_document(const json& document) {
    const json::json_pointer root;
    if (document.is_discarded() || !document.is_object()) {
        model_invalid("its text is not a JSON object", root);
    }
    for (const auto& item : document.items()) {
        if (item.key() != "states" && item.key() != "parameters" && item.key() != "events") {
            model_invalid("unknown field " + item.key(), root / item.key());
        }
    }
    if (!document.contains("states") || !document.at("states").is_object() ||
        document.at("states").empty()) {
        model_invalid("\"states\" must be an object of at least one state", root / "states");
    }
    if (!document.contains("parameters") || !document.at("parameters").is_object()) {
        model_invalid("\"parameters\" must be an object", root / "parameters");
    }
    auto model = std::make_shared<Model>();
    const json& parameters = document.at("parameters");
    for (const auto& item : parameters.items()) { // sorted by id
        const json::json_pointer pointer = root / "parameters" / item.key();
        if (!is_identifier(item.key())) {
            model_invalid("parameter id '" + item.key() + "' is not allowed", pointer);
        }
        if (!finite_number_value(item.value())) {
            model_invalid("parameter " + item.key() + " is not a finite number", pointer);
        }
        model->parameters.push_back(item.key());
        model->values.push_back(item.value().get<double>());
    }
    std::vector<bool> used(model->parameters.size(), false);
    for (const auto& item : document.at("states").items()) { // sorted by id
        const json::json_pointer pointer = root / "states" / item.key();
        if (!is_identifier(item.key()) || item.key() == kTotal) {
            model_invalid("state id '" + item.key() + "' is not allowed", pointer);
        }
        model->states.push_back(item.key());
        if (item.value().is_string()) {
            const std::size_t named = index_of(model->parameters, item.value().get<std::string>());
            if (named == model->parameters.size()) {
                model_invalid("the initial value of " + item.key() + " names no parameter",
                              pointer);
            }
            used[named] = true;
            model->initial.push_back(0.0);
            model->initial_parameter.push_back(named);
        } else if (finite_number_value(item.value())) {
            model->initial.push_back(item.value().get<double>());
            model->initial_parameter.push_back(kNone);
        } else {
            model_invalid("the initial value of " + item.key() +
                              " is neither a finite number nor a parameter id",
                          pointer);
        }
        const std::size_t rate = index_of(model->parameters, "k_" + item.key());
        if (rate == model->parameters.size()) {
            model_invalid("parameter k_" + item.key() + " is missing",
                          root / "parameters" / ("k_" + item.key()));
        }
        used[rate] = true;
        model->rate.push_back(rate);
    }
    for (std::size_t i = 0; i < used.size(); ++i) {
        if (!used[i]) {
            model_invalid("parameter " + model->parameters[i] +
                              " is neither k_<state> nor a state's initial value",
                          root / "parameters" / model->parameters[i]);
        }
    }
    if (document.contains("events")) {
        const json& events = document.at("events");
        if (!events.is_array()) {
            model_invalid("\"events\" must be an array", root / "events");
        }
        for (std::size_t i = 0; i < events.size(); ++i) {
            const json& event = events.at(i);
            const json::json_pointer pointer = root / "events" / i;
            if (!event.is_object() || event.size() != 3 || !event.contains("time") ||
                !finite_number_value(event.at("time")) || !event.contains("value") ||
                !finite_number_value(event.at("value")) || !event.contains("state") ||
                !event.at("state").is_string() ||
                index_of(model->states, event.at("state").get<std::string>()) ==
                    model->states.size()) {
                model_invalid("an event is {\"time\", \"state\", \"value\"} with a state id and "
                              "finite numbers",
                              pointer);
            }
        }
        model->events = !events.empty();
    }
    model->quantities = model->states;
    model->quantities.emplace_back(kTotal);
    return model;
}

json parse_model_text(std::string_view text) {
    return json::parse(text.begin(), text.end(), nullptr, false);
}

// ---------------------------------------------------------------------------------------------
// Compiled models: parsed now, or loaded from the shared store of the provider host.

const praenora_provider_host* g_provider_host = nullptr; // set by start(), before any session

bool store_offered() noexcept {
    const praenora_provider_host* host = g_provider_host;
    return host != nullptr &&
           host->struct_size >= offsetof(praenora_provider_host, save_compiled) +
                                    sizeof(praenora_provider_host::save_compiled) &&
           host->load_compiled != nullptr && host->save_compiled != nullptr;
}

// The store's key of a model text.
std::string store_key(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL; // FNV-1a, 64 bits
    for (const char byte : text) {
        hash = (hash ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    }
    static const char digits[] = "0123456789abcdef";
    std::string key = std::string(kFormat) + ":";
    for (int shift = 60; shift >= 0; shift -= 4) {
        key.push_back(digits[(hash >> shift) & 0xF]);
    }
    return key;
}

praenora_status PRAENORA_CALL append_to(void* context, const char* data,
                                        std::size_t size) noexcept {
    try {
        static_cast<std::string*>(context)->append(data == nullptr ? "" : data, size);
        return PRAENORA_STATUS_OK;
    } catch (...) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
}

// The model of `text`, from the shared store when it holds it, or compiled (parsed) now and kept
// there when there is a store.
std::shared_ptr<const Model> compiled_model(praenora_bytes text, const praenora_host_api* host) {
    const std::string_view model_text = view(text);
    const bool store = store_offered();
    const std::string key = store ? store_key(model_text) : std::string();
    if (store) {
        std::string entry;
        praenora_sink sink{&entry, &append_to};
        if (g_provider_host->load_compiled(g_provider_host->context, key.data(), key.size(),
                                           &sink) == 1) {
            const json stored = json::parse(entry, nullptr, false);
            std::string why = "it is not an entry of this provider";
            if (stored.is_object() && stored.contains("text") && stored.at("text").is_string() &&
                stored.contains("model") && stored.at("model").is_string()) {
                why = "it is the entry of another model text";
                if (stored.at("text").get_ref<const std::string&>() == model_text) {
                    try {
                        auto model = read_model_document(
                            parse_model_text(stored.at("model").get_ref<const std::string&>()));
                        log_to(host, PRAENORA_LOG_DEBUG,
                               "test-simulation: loaded the model from the shared store");
                        return model;
                    } catch (const Failure& failure) {
                        why = "its model does not read: " + failure.message;
                    }
                }
            }
            log_to(host, PRAENORA_LOG_WARN,
                   "test-simulation: the model in the shared store does not load (" + why +
                       "); compiling it");
        }
    }
    const json document = parse_model_text(model_text);
    auto model = read_model_document(document);
    log_to(host, PRAENORA_LOG_DEBUG, "test-simulation: compiled the model");
    if (store) {
        try {
            const std::string entry =
                json{{"text", std::string(model_text)}, {"model", document.dump()}}.dump();
            g_provider_host->save_compiled(g_provider_host->context, key.data(), key.size(),
                                           entry.data(), entry.size());
        } catch (const std::exception& error) {
            // Not kept; the model is compiled all the same.
            log_to(host, PRAENORA_LOG_WARN,
                   std::string("test-simulation: the model is not kept in the shared store: ") +
                       error.what());
        }
    }
    return model;
}

// ---------------------------------------------------------------------------------------------
// Documents passed to the service.

// The JSON object in `bytes`; empty bytes are {} when `empty_is_object`.
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

[[noreturn]] void invalid(const std::string& message, const json::json_pointer& pointer,
                          json details = json::object()) {
    fail(PRAENORA_STATUS_INVALID_ARGUMENT, "praenora.simulation: " + message,
         at_pointer(pointer, std::move(details)));
}

[[noreturn]] void unsupported(const std::string& message, const json::json_pointer& pointer,
                              json details = json::object()) {
    fail(PRAENORA_STATUS_UNSUPPORTED, "praenora.simulation: " + message,
         at_pointer(pointer, std::move(details)));
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

// Fails for the first field of `object` (at `pointer`) that is not in `known`.
void only_fields(const json& object, const json::json_pointer& pointer,
                 std::initializer_list<const char*> known) {
    for (const auto& item : object.items()) {
        if (std::none_of(known.begin(), known.end(),
                         [&item](const char* name) { return item.key() == name; })) {
            invalid("unknown field " + (pointer / item.key()).to_string(), pointer / item.key());
        }
    }
}

// The ids of an array of distinct ids, each in `allowed`.
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

// What a request or a plan fixes about a run.
struct Settings {
    double start = 0.0;
    std::vector<double> times;
    bool include_states = true;
    std::vector<std::size_t> quantities; // indices into Model::quantities
    std::vector<std::string> columns;
    json solver = json::object(); // the settings used, as results report them
};

// The fields both requests and plans have: time_domain, solver, output and checks.
Settings read_settings(const json& document, const Model& model) {
    const json::json_pointer root;
    Settings settings;

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
    const double end = finite_number(domain.at("end"), root / "time_domain" / "end");
    if (!(end > settings.start)) {
        invalid("time_domain.end must be later than time_domain.start",
                root / "time_domain" / "end");
    }

    if (document.contains("solver")) {
        const json& solver = object_at(document, root / "solver");
        only_fields(solver, root / "solver",
                    {"method", "stiffness", "relative_tolerance", "absolute_tolerance",
                     "max_steps", "max_step", "min_step", "seed", "event_time_tolerance"});
        if (solver.contains("method")) {
            const json& method = solver.at("method");
            if (!method.is_string()) {
                invalid("solver.method is not a string", root / "solver" / "method");
            }
            if (method.get<std::string>() != kMethod) {
                unsupported("method '" + method.get<std::string>() + "' is not offered",
                            root / "solver" / "method",
                            {{"method", method}, {"methods", {kMethod}}});
            }
        }
        for (const auto& item : solver.items()) {
            if (item.key() != "method") {
                unsupported("method analytic has no setting " + item.key(),
                            root / "solver" / item.key());
            }
        }
    }
    settings.solver = {{"method", kMethod}};

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
        settings.quantities = id_list(output.at("quantities"), out / "quantities",
                                      model.quantities, "quantity");
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
        settings.columns.insert(settings.columns.end(), model.states.begin(), model.states.end());
    }
    for (const std::size_t index : settings.quantities) {
        settings.columns.push_back(model.quantities[index]);
    }
    const std::uint64_t columns = settings.columns.size();
    const std::uint64_t max_rows = PRAENORA_SIMULATION_MAX_RUN_VALUES / columns;
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
        if (times.size() > max_rows) {
            too_many(static_cast<double>(times.size()));
        }
        for (std::size_t i = 0; i < times.size(); ++i) {
            const double t = finite_number(times.at(i), out / "times" / i);
            if (t < settings.start || t > end) {
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
        const double last = std::floor((end - settings.start) / step + 1e-9);
        if (!(last + 1.0 <= static_cast<double>(max_rows))) {
            too_many(last + 1.0);
        }
        const auto count = static_cast<std::size_t>(last) + 1;
        for (std::size_t i = 0; i < count; ++i) {
            const double t = std::min(settings.start + static_cast<double>(i) * step, end);
            if (!settings.times.empty() && !(t > settings.times.back())) {
                invalid("output.step is below the precision of the times: they do not increase",
                        out / "step");
            }
            settings.times.push_back(t);
        }
    } else if (save_at == "solver_steps") {
        unsupported("method analytic takes no steps, so it has no solver_steps output",
                    out / "save_at");
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
                unsupported("checks." + item.key() + " is always on", root / "checks" / item.key());
            }
        }
    }
    return settings;
}

// Fails for the first top-level field of `document` that is not in `known`; `inputs` and
// `events_enabled` are ode-uq-spec fields the service does not map.
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

// Fails for a model with events, which the toy format declares but this provider does not
// simulate: UNSUPPORTED with details.reason "events".
void refuse_events(const Model& model) {
    if (model.events) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "praenora.simulation: test-simulation does not simulate the events of a test-decay "
             "model",
             {{"reason", "events"}});
    }
}

// The values of a run: the parameter values (the model's, then what a request or plan sets), and
// the state at the start (the initial assignments evaluated with those parameter values, then what
// the request's or plan's initial conditions set).
struct Values {
    std::vector<double> parameters;
    std::vector<double> initial;
};

// ---------------------------------------------------------------------------------------------
// Runs.

// Computes one run into `out` (rows * columns values, row-major). Returns the failure document of
// a value that is not finite, or null when every value is finite.
json compute(const Model& model, const Settings& settings, const Values& values, double* out) {
    const std::size_t states = model.states.size();
    const std::size_t columns = settings.columns.size();
    std::vector<double> x(states);
    for (std::size_t row = 0; row < settings.times.size(); ++row) {
        const double t = settings.times[row];
        double* cell = out + row * columns;
        double total = 0.0;
        for (std::size_t i = 0; i < states; ++i) {
            const double rate = values.parameters[model.rate[i]];
            x[i] = values.initial[i] * std::exp(-rate * (t - settings.start));
            total += x[i];
        }
        *cell++ = t;
        if (settings.include_states) {
            cell = std::copy(x.begin(), x.end(), cell);
        }
        for (const std::size_t q : settings.quantities) {
            *cell++ = q < states ? x[q] : total;
        }
    }
    const std::size_t count = settings.times.size() * columns;
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(out[i])) {
            const std::string& column = settings.columns[i % columns];
            const double time = settings.times[i / columns];
            return {{"code", PRAENORA_STATUS_FAILED},
                    {"message", "praenora.simulation: " + column + " is not finite at time " +
                                    json(time).dump()},
                    {"details",
                     {{"reason", "nonfinite"},
                      {"service", PRAENORA_SIMULATION_SERVICE},
                      {"column", column},
                      {"time", time}}}};
        }
    }
    return nullptr;
}

// One of a plan's inputs: a parameter or a state's initial value.
struct Input {
    bool parameter;
    std::size_t index;
};

// Holds a session or a plan for one call; fails with INVALID_STATE when another call holds it.
class Hold {
public:
    Hold(std::atomic<bool>& busy, const char* what) : busy_(busy) {
        if (busy_.exchange(true, std::memory_order_acquire)) {
            fail(PRAENORA_STATUS_INVALID_STATE,
                 std::string("praenora.simulation: the ") + what + " is busy with another call");
        }
    }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    ~Hold() { busy_.store(false, std::memory_order_release); }

private:
    std::atomic<bool>& busy_;
};

// What a kernel's model points to. Its results depend only on the arguments and `values`.
struct KernelModel {
    std::shared_ptr<const Model> model;
    std::vector<double> values;          // the current value of every parameter of the model
    std::vector<std::size_t> parameters; // the kernel's parameter i is this parameter
    std::vector<std::size_t> quantities; // indices into Model::quantities
};

std::size_t mul_or_max(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::numeric_limits<std::size_t>::max();
    }
    const std::uint64_t product = a * b;
    return product > std::numeric_limits<std::size_t>::max()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(product);
}

} // namespace

// The provider's objects. Opaque to modules (simulation.h declares the types).
struct praenora_simulation_session {
    std::shared_ptr<const Model> model;
    std::atomic<bool> busy{false};
};

struct praenora_simulation_plan {
    std::shared_ptr<const Model> model;
    Settings settings;
    std::vector<Input> inputs;
    std::atomic<bool> busy{false};
};

namespace {

// ---------------------------------------------------------------------------------------------
// Kernel functions: no allocation, no exception, no host.

KernelModel* kernel_of(void* model) noexcept { return static_cast<KernelModel*>(model); }

praenora_status PRAENORA_CALL kernel_rhs(void* model, double, const double* y,
                                         double* dydt) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || y == nullptr || dydt == nullptr) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    const Model& m = *kernel->model;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        dydt[i] = -kernel->values[m.rate[i]] * y[i];
    }
    return PRAENORA_STATUS_OK;
}

praenora_status PRAENORA_CALL kernel_set_parameters(void* model, const double* values) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || (values == nullptr && !kernel->parameters.empty())) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    // Nothing is derived from the parameters but the initial state, which initial_state
    // evaluates each time.
    for (std::size_t i = 0; i < kernel->parameters.size(); ++i) {
        kernel->values[kernel->parameters[i]] = values[i];
    }
    return PRAENORA_STATUS_OK;
}

praenora_status PRAENORA_CALL kernel_initial_state(void* model, double* y) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || y == nullptr) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    // The initial assignments, evaluated with the current parameter values.
    const Model& m = *kernel->model;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        y[i] = m.initial_parameter[i] != kNone ? kernel->values[m.initial_parameter[i]]
                                               : m.initial[i];
    }
    return PRAENORA_STATUS_OK;
}

praenora_status PRAENORA_CALL kernel_quantities(void* model, double, const double* y,
                                                double* out) noexcept {
    KernelModel* kernel = kernel_of(model);
    if (kernel == nullptr || y == nullptr || (out == nullptr && !kernel->quantities.empty())) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    const std::size_t states = kernel->model->states.size();
    double total = 0.0;
    for (std::size_t i = 0; i < states; ++i) {
        total += y[i];
    }
    for (std::size_t i = 0; i < kernel->quantities.size(); ++i) {
        const std::size_t q = kernel->quantities[i];
        out[i] = q < states ? y[q] : total;
    }
    return PRAENORA_STATUS_OK;
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
        session->model = compiled_model(model, host);
        log_to(host, PRAENORA_LOG_DEBUG,
            "test-simulation: opened a test-decay model of " +
                std::to_string(session->model->states.size()) + " states");
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
        const Model& model = *session->model;
        write_to(out_json,
              json{{"events", model.events},
                   {"states", model.states},
                   {"parameters", model.parameters},
                   {"quantities", model.quantities}}
                  .dump(),
              "the model description");
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
        const Model& model = *session->model;
        refuse_events(model);
        const json request = read_object(request_json, "the request", false);
        top_level_fields(request, {"time_domain", "initial_conditions", "parameter_values",
                                   "solver", "output", "checks"});
        // The values each field sets, by index.
        std::vector<std::pair<std::size_t, double>> set[2];
        const json::json_pointer root;
        for (const char* field : {"initial_conditions", "parameter_values"}) {
            if (!request.contains(field)) {
                continue;
            }
            const bool states = std::strcmp(field, "initial_conditions") == 0;
            const std::vector<std::string>& ids = states ? model.states : model.parameters;
            for (const auto& item : object_at(request, root / field).items()) {
                const json::json_pointer pointer = root / field / item.key();
                const std::size_t index = index_of(ids, item.key());
                if (index == ids.size()) {
                    invalid(std::string("the model has no ") + (states ? "state" : "parameter") +
                                " '" + item.key() + "'",
                            pointer, {{"id", item.key()}});
                }
                set[states ? 0 : 1].emplace_back(index, finite_number(item.value(), pointer));
            }
        }
        // Parameter values take effect as initial values; then initial conditions replace state
        // values at the start.
        Values values{model.values, {}};
        for (const auto& [index, value] : set[1]) {
            values.parameters[index] = value;
        }
        values.initial = initial_state(model, values.parameters);
        for (const auto& [index, value] : set[0]) {
            values.initial[index] = value;
        }
        const Settings settings = read_settings(request, model);
        std::vector<double> out(settings.times.size() * settings.columns.size());
        const json failure = compute(model, settings, values, out.data());
        json result;
        if (failure.is_null()) {
            json rows = json::array();
            for (std::size_t row = 0; row < settings.times.size(); ++row) {
                const auto first = out.begin() + static_cast<std::ptrdiff_t>(
                                                      row * settings.columns.size());
                rows.push_back(std::vector<double>(
                    first, first + static_cast<std::ptrdiff_t>(settings.columns.size())));
            }
            result = {{"ok", true},
                      {"columns", settings.columns},
                      {"rows", std::move(rows)},
                      {"solver", settings.solver}};
        } else {
            result = {{"ok", false}, {"error", failure}};
        }
        log_to(host, PRAENORA_LOG_DEBUG,
            "test-simulation: simulated " + std::to_string(settings.times.size()) + " rows");
        write_to(result_json, result.dump(), "the result");
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
        refuse_events(*session->model);
        const json document = read_object(plan_json, "the plan", false);
        top_level_fields(document, {"parameters", "initial_conditions", "time_domain", "solver",
                                    "output", "checks"});
        auto plan = std::make_unique<praenora_simulation_plan>();
        plan->model = session->model;
        const Model& model = *plan->model;
        const json::json_pointer root;
        std::vector<std::string> inputs;
        for (const char* field : {"parameters", "initial_conditions"}) {
            if (!document.contains(field)) {
                continue;
            }
            const bool parameters = std::strcmp(field, "parameters") == 0;
            const std::vector<std::string>& ids = parameters ? model.parameters : model.states;
            const std::vector<std::size_t> indices = id_list(
                document.at(field), root / field, ids, parameters ? "parameter" : "state");
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const std::string& id = ids[indices[i]];
                if (std::find(inputs.begin(), inputs.end(), id) != inputs.end()) {
                    invalid("'" + id + "' is an input already", root / field / i, {{"id", id}});
                }
                inputs.push_back(id);
                plan->inputs.push_back(Input{parameters, indices[i]});
            }
        }
        plan->settings = read_settings(document, model);
        write_to(info_json,
              json{{"inputs", inputs},
                   {"columns", plan->settings.columns},
                   {"rows", plan->settings.times.size()},
                   {"solver", plan->settings.solver}}
                  .dump(),
              "the plan's description");
        *out_plan = plan.release();
    });
}

// Applies one set of a plan's inputs to the model's values: the parameters first, as initial
// values, then the initial conditions.
Values values_of(const praenora_simulation_plan& plan, const double* inputs) {
    const Model& model = *plan.model;
    Values values{model.values, {}};
    for (std::size_t i = 0; i < plan.inputs.size(); ++i) {
        if (plan.inputs[i].parameter) {
            values.parameters[plan.inputs[i].index] = inputs[i];
        }
    }
    values.initial = initial_state(model, values.parameters);
    for (std::size_t i = 0; i < plan.inputs.size(); ++i) {
        if (!plan.inputs[i].parameter) {
            values.initial[plan.inputs[i].index] = inputs[i];
        }
    }
    return values;
}

// Fails unless every one of `count` values is finite.
void check_finite(const double* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "praenora.simulation: input value " + std::to_string(i) + " is not finite",
                 {{"index", i}});
        }
    }
}

std::size_t run_size(const praenora_simulation_plan& plan) {
    return plan.settings.times.size() * plan.settings.columns.size();
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
        const std::size_t inputs = plan->inputs.size();
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
        const json failure =
            compute(*plan->model, plan->settings, values_of(*plan, values), out);
        if (failure.is_null()) {
            *outcome = PRAENORA_SIMULATION_RUN_OK;
            return;
        }
        std::fill(out, out + size, std::numeric_limits<double>::quiet_NaN());
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
        const std::size_t inputs = plan->inputs.size();
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
            double* set_out = out + set * size;
            const double* set_values = inputs == 0 ? values : values + set * inputs;
            const json failure =
                compute(*plan->model, plan->settings, values_of(*plan, set_values), set_out);
            outcomes[set] =
                failure.is_null() ? PRAENORA_SIMULATION_RUN_OK : PRAENORA_SIMULATION_RUN_FAILED;
            if (!failure.is_null()) {
                std::fill(set_out, set_out + size, std::numeric_limits<double>::quiet_NaN());
            }
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
        if (session->model->events) {
            // A kernel is the continuous right-hand side only: it cannot apply events.
            fail(PRAENORA_STATUS_UNSUPPORTED,
                 "praenora.simulation: the model has events, which a kernel cannot apply",
                 {{"reason", "events"}});
        }
        const json options = read_object(options_json, "the kernel options", true);
        only_fields(options, json::json_pointer(), {"parameters", "quantities"});
        auto kernel = std::make_unique<KernelModel>();
        kernel->model = session->model;
        const Model& model = *kernel->model;
        kernel->values = model.values;
        const json::json_pointer root;
        if (options.contains("parameters")) {
            kernel->parameters =
                id_list(options.at("parameters"), root / "parameters", model.parameters,
                        "parameter");
        } else {
            for (std::size_t i = 0; i < model.parameters.size(); ++i) {
                kernel->parameters.push_back(i);
            }
        }
        if (options.contains("quantities")) {
            kernel->quantities = id_list(options.at("quantities"), root / "quantities",
                                         model.quantities, "quantity");
        } else {
            for (std::size_t i = 0; i < model.quantities.size(); ++i) {
                kernel->quantities.push_back(i);
            }
        }
        out_kernel->struct_size = static_cast<std::uint32_t>(
            std::min<std::size_t>(out_kernel->struct_size, sizeof(praenora_simulation_kernel)));
        out_kernel->reserved = 0u;
        out_kernel->state_count = model.states.size();
        out_kernel->parameter_count = kernel->parameters.size();
        out_kernel->quantity_count = kernel->quantities.size();
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

praenora_status PRAENORA_CALL start(const praenora_provider_host* host, praenora_sink*) noexcept {
    g_provider_host = host;
    provider_fixture_log(host, PRAENORA_LOG_INFO, "test-simulation started");
    return PRAENORA_STATUS_OK;
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) noexcept {
    if (major == PRAENORA_SIMULATION_SERVICE_MAJOR &&
        provider_fixture_name_is(name, name_size, PRAENORA_SIMULATION_SERVICE)) {
        return &simulation_table;
    }
    return nullptr;
}

const praenora_provider_api_v0 provider_table = {
    {PRAENORA_PROVIDER_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_provider_api_v0)), 0u},
    &provider_fixture_describe,
    &start,
    &get_service,
};

} // namespace

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) {
    return &provider_table;
}
