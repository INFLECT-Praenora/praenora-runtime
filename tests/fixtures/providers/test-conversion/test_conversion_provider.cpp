// test-conversion: the reference provider of the model conversion service
// praenora.model-conversion@0.0 (include/praenora/sdk/model_conversion.h; normative:
// docs/specs/model-conversion-service.md) for tests. It needs no engine: it implements every rule
// of the service for two toy formats of one model, so that the service checks
// (tests/contract/model_conversion_service_checks.hpp) and modules that use the service can run
// without one.
//
// Format "test-decay": the JSON model format of test-simulation, without events:
//     {"states": {"<id>": <x0 or a parameter id>, ...}, "parameters": {"<id>": <value>, ...}}
// with its rules: at least one state, ids matching [A-Za-z_][A-Za-z0-9_]* (states not "total"),
// one parameter k_<id> for each state <id>, only those parameters and the ones that states name,
// finite numbers. "events" may be there, as test-simulation reads it; a model whose events are
// not empty is UNSUPPORTED at /events: test-decay-text has no events.
//
// Format "test-decay-text": lines, separated by LF (a CR before the LF belongs to the line end),
// of words separated by spaces and tabs; '#' starts a comment that runs to the end of its line.
// Each line is empty or one of
//     state <id> <number or parameter id>
//     parameter <id> <number>
//     hold <milliseconds>              (an integer from 0 to 10000, without leading zeros)
// with the ids and rules of test-decay; numbers are JSON numbers. A line whose first word is
// "import" refuses the source before anything else is read (DATA_INVALID, details.rule "import").
//
// Both formats: a leading UTF-8 byte order mark is skipped, CR LF line ends are accepted, and a
// source has one module, "main". test-decay-text -> test-decay writes the model as canonical JSON,
// every number a double, which test-simulation opens; test-decay -> test-decay-text writes the
// state lines, then the parameter lines, each sorted by id (UTF-8 bytes), numbers as canonical
// JSON writes them, each line ending in LF. Comments and hold lines never reach the output: each
// hold line adds the warning "hold <milliseconds> ignored in output" to the report and holds the
// provider's lock that long. Anything else malformed is DATA_INVALID with details.engine_error
// {"type": "test-conversion", "message"} and, for a problem on a line, details.line.
//
// One process-wide lock serializes the translations, as the libAntimony provider's does; a call
// polls cancel_requested() before it takes the lock and every 20 ms while it waits for it, with the
// wait the antimony provider uses (src/providers/common/cancellable_lock.hpp). Each conversion
// logs a debug message through the calling instance's host; start() logs "test-conversion
// started" to the provider's logger.
//
// C++17 with nlohmann/json. No exception leaves a function of the provider.
#include "cancellable_lock.hpp"
#include "provider_fixture.h"

#include <praenora/sdk/model_conversion.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

constexpr const char* kService = PRAENORA_MODEL_CONVERSION_SERVICE;
constexpr const char* kDecay = "test-decay";
constexpr const char* kDecayText = "test-decay-text";
constexpr const char* kMain = "main";
constexpr const char* kTotal = "total";
constexpr std::uint32_t kMaxHold = 10000; // milliseconds, per hold line
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

// ---------------------------------------------------------------------------------------------
// Failures and the documents that report them.

// A failed call: its status, message and details (an object, to which "service" is added).
struct Failure {
    praenora_status code;
    std::string message;
    json details;
};

[[noreturn]] void fail(praenora_status code, const std::string& message,
                       json details = json::object()) {
    throw Failure{code, std::string(kService) + ": " + message, std::move(details)};
}

// Canonical JSON (compact, keys sorted), with any text that is not valid UTF-8 replaced by U+FFFD.
std::string dump(const json& document) {
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
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
        fail(status, std::string("the sink refused ") + what);
    }
}

// The error document of a failure, written when the sink takes it.
void write_error(praenora_sink* error_json, praenora_status code, const std::string& message,
                 json details = json::object()) noexcept {
    try {
        details["service"] = kService;
        const std::string text =
            dump(json{{"code", code}, {"message", message}, {"details", std::move(details)}});
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
                    std::string(kService) + ": out of memory");
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    } catch (const std::exception& error) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    std::string(kService) + ": " + error.what());
        return PRAENORA_STATUS_INTERNAL;
    } catch (...) {
        write_error(error_json, PRAENORA_STATUS_INTERNAL,
                    std::string(kService) + ": an unknown exception");
        return PRAENORA_STATUS_INTERNAL;
    }
}

// ---------------------------------------------------------------------------------------------
// The host API of the calling instance.

bool host_has(const praenora_host_api* host, std::size_t offset, std::size_t size) noexcept {
    return host != nullptr && host->struct_size >= offset + size;
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
// The provider's lock: one for the process, serializing the translations.

std::timed_mutex& translation_lock() {
    static std::timed_mutex lock;
    return lock;
}

// ---------------------------------------------------------------------------------------------
// The options.

struct Options {
    std::string from, to;
    std::optional<std::string> module;
};

// The conversions, sorted by from, then to.
const std::vector<std::pair<std::string, std::string>>& conversions() {
    static const std::vector<std::pair<std::string, std::string>> list = {{kDecay, kDecayText},
                                                                          {kDecayText, kDecay}};
    return list;
}

json conversions_json() {
    json list = json::array();
    for (const auto& [from, to] : conversions()) {
        list.push_back(json{{"from", from}, {"to", to}});
    }
    return list;
}

[[noreturn]] void invalid_option(const std::string& message, const json::json_pointer& pointer) {
    fail(PRAENORA_STATUS_INVALID_ARGUMENT, message, {{"pointer", pointer.to_string()}});
}

Options read_options(praenora_bytes bytes) {
    const std::string_view text = view(bytes);
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    const json::json_pointer root;
    if (document.is_discarded() || !document.is_object()) {
        invalid_option("the options are not a JSON object", root);
    }
    for (const auto& item : document.items()) {
        if (item.key() != "from" && item.key() != "to" && item.key() != "module") {
            invalid_option("the options have an unknown field " + (root / item.key()).to_string(),
                           root / item.key());
        }
    }
    for (const char* field : {"from", "to"}) {
        const json::json_pointer pointer = root / field;
        if (!document.contains(field)) {
            invalid_option("the options have no " + pointer.to_string(), pointer);
        }
        if (!document.at(field).is_string()) {
            invalid_option(pointer.to_string() + " is not a string", pointer);
        }
    }
    Options options;
    options.from = document.at("from").get<std::string>();
    options.to = document.at("to").get<std::string>();
    if (document.contains("module")) {
        const json& module = document.at("module");
        if (!module.is_string()) {
            invalid_option("/module is not a string", root / "module");
        }
        if (module.get_ref<const std::string&>().empty()) {
            invalid_option("/module is empty: leave it out to translate the main module",
                           root / "module");
        }
        options.module = module.get<std::string>();
    }
    const auto& all = conversions();
    if (std::none_of(all.begin(), all.end(),
                     [&](const auto& conversion) { return conversion.first == options.from; })) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "test-conversion does not read the format '" + options.from + "'",
             {{"pointer", "/from"}, {"format", options.from}, {"conversions", conversions_json()}});
    }
    if (std::find(all.begin(), all.end(), std::make_pair(options.from, options.to)) == all.end()) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "test-conversion does not convert " + options.from + " to '" + options.to + "'",
             {{"pointer", "/to"}, {"format", options.to}, {"conversions", conversions_json()}});
    }
    return options;
}

// ---------------------------------------------------------------------------------------------
// The source text.

// The length of the UTF-8 sequence that starts at `i`, or 0 when the bytes there are not one
// (overlong forms, surrogates and code points above U+10FFFF are not).
std::size_t utf8_length(std::string_view text, std::size_t i) noexcept {
    const auto byte = [&text](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80, high = 0xBF; // the allowed range of the second byte
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead == 0xE0) {
        length = 3;
        low = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        length = 3;
    } else if (lead == 0xED) {
        length = 3;
        high = 0x9F;
    } else if (lead == 0xF0) {
        length = 4;
        low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        length = 4;
    } else if (lead == 0xF4) {
        length = 4;
        high = 0x8F;
    } else {
        return 0;
    }
    if (i + length > text.size() || byte(i + 1) < low || byte(i + 1) > high) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) {
            return 0;
        }
    }
    return length;
}

// Fails unless the source is valid UTF-8 without a NUL byte; details.offset is where it is not.
void check_text(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\0') {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "the source holds a NUL byte at offset " + std::to_string(i),
                 {{"reason", "nul"}, {"offset", i}});
        }
        const std::size_t length = utf8_length(text, i);
        if (length == 0) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "the source is not valid UTF-8 at offset " + std::to_string(i),
                 {{"reason", "utf8"}, {"offset", i}});
        }
        i += length;
    }
}

std::string_view without_byte_order_mark(std::string_view text) {
    return text.substr(0, kByteOrderMark.size()) == kByteOrderMark
               ? text.substr(kByteOrderMark.size())
               : text;
}

// Space, tab, line feed, vertical tab, form feed and carriage return.
bool white_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}

// One line of a test-decay-text source: its number, from 1, and its words, without its comment.
struct Line {
    std::size_t number = 0;
    std::vector<std::string_view> words;
};

std::vector<Line> lines_of(std::string_view text) {
    std::vector<Line> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = line.substr(0, line.find('#'));
        Line parsed;
        parsed.number = lines.size() + 1;
        std::size_t i = 0;
        while (i < line.size()) {
            if (line[i] == ' ' || line[i] == '\t') {
                ++i;
                continue;
            }
            const std::size_t word = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
                ++i;
            }
            parsed.words.push_back(line.substr(word, i - word));
        }
        lines.push_back(std::move(parsed));
    }
    return lines;
}

// The rule "import": a line whose first word is import refuses the source.
void check_imports(const std::vector<Line>& lines) {
    for (const Line& line : lines) {
        if (!line.words.empty() && line.words.front() == "import") {
            fail(PRAENORA_STATUS_DATA_INVALID,
                 "line " + std::to_string(line.number) +
                     " of the test-decay-text source imports another source, which the service "
                     "refuses (rule import)",
                 {{"rule", "import"}, {"line", line.number}});
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The model, as both formats hold it.

struct State {
    std::optional<double> number; // the initial value, or
    std::string parameter;        // the parameter whose value it is
    std::size_t line = 0;         // where test-decay-text declares it; 0 for test-decay
};

struct Parameter {
    double value = 0.0;
    std::size_t line = 0;
};

struct Model {
    std::map<std::string, State> states; // by id, in the order of UTF-8 bytes
    std::map<std::string, Parameter> parameters;
};

// The engine cannot read the source: DATA_INVALID with details.engine_error and, for a problem on
// a line, details.line.
[[noreturn]] void engine_failure(const char* format, const std::string& problem,
                                 std::size_t line = 0) {
    json details = {{"engine_error", {{"type", "test-conversion"}, {"message", problem}}}};
    std::string where;
    if (line != 0) {
        details["line"] = line;
        where = " (line " + std::to_string(line) + ")";
    }
    fail(PRAENORA_STATUS_DATA_INVALID,
         std::string("the source is not a ") + format + " model" + where + ": " + problem,
         std::move(details));
}

bool is_identifier(std::string_view id) {
    const auto letter = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    return !id.empty() && letter(id[0]) && std::all_of(id.begin(), id.end(), [&letter](char ch) {
        return letter(ch) || (ch >= '0' && ch <= '9');
    });
}

// The rules of test-decay that concern the model as a whole.
void check_model(const Model& model, const char* format) {
    if (model.states.empty()) {
        engine_failure(format, "the model has no state");
    }
    std::set<std::string> used;
    for (const auto& [id, state] : model.states) {
        const std::string rate = "k_" + id;
        if (model.parameters.count(rate) == 0) {
            engine_failure(format, "parameter " + rate + " of state " + id + " is missing",
                           state.line);
        }
        used.insert(rate);
        if (!state.number.has_value()) {
            if (model.parameters.count(state.parameter) == 0) {
                engine_failure(format,
                               "the initial value of state " + id + " names no parameter (" +
                                   state.parameter + ")",
                               state.line);
            }
            used.insert(state.parameter);
        }
    }
    for (const auto& [id, parameter] : model.parameters) {
        if (used.count(id) == 0) {
            engine_failure(format,
                           "parameter " + id + " is neither k_<state> nor a state's initial value",
                           parameter.line);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Reading test-decay-text.

// A JSON number, or nothing.
std::optional<double> number_of(std::string_view word) {
    const json value = json::parse(word.begin(), word.end(), nullptr, false);
    if (value.is_discarded() || !value.is_number() || !std::isfinite(value.get<double>())) {
        return std::nullopt;
    }
    return value.get<double>();
}

// The milliseconds of a hold line: an integer from 0 to kMaxHold without leading zeros.
std::optional<std::uint32_t> milliseconds_of(std::string_view word) {
    if (word.empty() || word.size() > 5 || (word.size() > 1 && word[0] == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char ch : word) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    if (value > kMaxHold) {
        return std::nullopt;
    }
    return value;
}

[[noreturn]] void malformed_line(const Line& line, const std::string& problem) {
    engine_failure(kDecayText, problem, line.number);
}

struct TextModel {
    Model model;
    std::uint64_t hold_milliseconds = 0;
    std::vector<std::string> warnings;
};

TextModel read_text_model(const std::vector<Line>& lines) {
    TextModel result;
    Model& model = result.model;
    for (const Line& line : lines) {
        const std::vector<std::string_view>& words = line.words;
        if (words.empty()) {
            continue;
        }
        const std::string_view directive = words.front();
        if (directive == "state" || directive == "parameter") {
            const bool state = directive == "state";
            if (words.size() != 3) {
                malformed_line(line, state ? "a state line is: state <id> <number or parameter id>"
                                           : "a parameter line is: parameter <id> <number>");
            }
            const std::string id(words[1]);
            if (!is_identifier(id) || (state && id == kTotal)) {
                malformed_line(line, std::string(directive) + " id '" + id + "' is not allowed");
            }
            if (state ? model.states.count(id) != 0 : model.parameters.count(id) != 0) {
                malformed_line(line, std::string(directive) + " " + id + " is declared twice");
            }
            const std::optional<double> number = number_of(words[2]);
            if (state) {
                State declared;
                declared.line = line.number;
                if (number.has_value()) {
                    declared.number = number;
                } else if (is_identifier(words[2])) {
                    declared.parameter = std::string(words[2]);
                } else {
                    malformed_line(line, "the initial value of state " + id +
                                             " is neither a finite number nor a parameter id");
                }
                model.states.emplace(id, std::move(declared));
            } else {
                if (!number.has_value()) {
                    malformed_line(line,
                                   "the value of parameter " + id + " is not a finite number");
                }
                model.parameters.emplace(id, Parameter{*number, line.number});
            }
        } else if (directive == "hold") {
            const std::optional<std::uint32_t> milliseconds =
                words.size() == 2 ? milliseconds_of(words[1]) : std::nullopt;
            if (!milliseconds.has_value()) {
                malformed_line(line, "a hold line is: hold <milliseconds>, an integer from 0 to "
                                     "10000");
            }
            result.hold_milliseconds += *milliseconds;
            result.warnings.push_back("hold " + std::to_string(*milliseconds) +
                                      " ignored in output");
        } else {
            malformed_line(line, "'" + std::string(directive) +
                                     "' is not one of state, parameter and hold");
        }
    }
    check_model(model, kDecayText);
    return result;
}

// ---------------------------------------------------------------------------------------------
// Reading test-decay.

// The line of the byte `byte` (from 1) of `text`.
std::size_t line_at(std::string_view text, std::size_t byte) {
    const std::string_view before = text.substr(0, byte == 0 ? 0 : byte - 1);
    return 1 + static_cast<std::size_t>(std::count(before.begin(), before.end(), '\n'));
}

bool finite_number(const json& value) {
    return value.is_number() && std::isfinite(value.get<double>());
}

Model read_json_model(std::string_view text) {
    json document;
    try {
        document = json::parse(text.begin(), text.end());
    } catch (const json::parse_error& error) {
        engine_failure(kDecay, error.what(), line_at(text, error.byte));
    } catch (const json::exception& error) {
        // Any other failure to read the text, such as a number too large for a double
        // (out_of_range): malformed as well, with no byte offset to locate it.
        engine_failure(kDecay, error.what());
    }
    if (!document.is_object()) {
        engine_failure(kDecay, "the model is not a JSON object");
    }
    for (const auto& item : document.items()) {
        if (item.key() != "states" && item.key() != "parameters" && item.key() != "events") {
            engine_failure(kDecay, "unknown field /" + item.key());
        }
    }
    if (!document.contains("states") || !document.at("states").is_object() ||
        document.at("states").empty()) {
        engine_failure(kDecay, "\"states\" must be an object of at least one state");
    }
    if (!document.contains("parameters") || !document.at("parameters").is_object()) {
        engine_failure(kDecay, "\"parameters\" must be an object");
    }
    Model model;
    for (const auto& item : document.at("parameters").items()) {
        if (!is_identifier(item.key())) {
            engine_failure(kDecay, "parameter id '" + item.key() + "' is not allowed");
        }
        if (!finite_number(item.value())) {
            engine_failure(kDecay, "parameter " + item.key() + " is not a finite number");
        }
        model.parameters.emplace(item.key(), Parameter{item.value().get<double>(), 0});
    }
    for (const auto& item : document.at("states").items()) {
        if (!is_identifier(item.key()) || item.key() == kTotal) {
            engine_failure(kDecay, "state id '" + item.key() + "' is not allowed");
        }
        State state;
        if (item.value().is_string()) {
            state.parameter = item.value().get<std::string>();
        } else if (finite_number(item.value())) {
            state.number = item.value().get<double>();
        } else {
            engine_failure(kDecay, "the initial value of state " + item.key() +
                                       " is neither a finite number nor a parameter id");
        }
        model.states.emplace(item.key(), std::move(state));
    }
    bool events = false;
    if (document.contains("events")) {
        const json& list = document.at("events");
        if (!list.is_array()) {
            engine_failure(kDecay, "\"events\" must be an array");
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            const json& event = list.at(i);
            if (!event.is_object() || event.size() != 3 || !event.contains("time") ||
                !finite_number(event.at("time")) || !event.contains("value") ||
                !finite_number(event.at("value")) || !event.contains("state") ||
                !event.at("state").is_string() ||
                model.states.count(event.at("state").get<std::string>()) == 0) {
                engine_failure(kDecay, "event " + std::to_string(i) +
                                           " is not {\"time\", \"state\", \"value\"} with a state "
                                           "id and finite numbers");
            }
        }
        events = !list.empty();
    }
    check_model(model, kDecay);
    if (events) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the model has events, which test-decay-text cannot hold: test-conversion does not "
             "convert them",
             {{"pointer", "/events"}});
    }
    return model;
}

// ---------------------------------------------------------------------------------------------
// Writing.

// A number as canonical JSON writes it (a double: the shortest digits that read back as the same
// double, ".0" after an integral value).
std::string number_text(double value) { return json(value).dump(); }

std::string write_json_model(const Model& model) {
    json states = json::object();
    json parameters = json::object();
    for (const auto& [id, state] : model.states) {
        states[id] = state.number.has_value() ? json(*state.number) : json(state.parameter);
    }
    for (const auto& [id, parameter] : model.parameters) {
        parameters[id] = parameter.value;
    }
    return dump(json{{"states", std::move(states)}, {"parameters", std::move(parameters)}});
}

std::string write_text_model(const Model& model) {
    std::string text;
    for (const auto& [id, state] : model.states) {
        text += "state " + id + " " +
                (state.number.has_value() ? number_text(*state.number) : state.parameter) + "\n";
    }
    for (const auto& [id, parameter] : model.parameters) {
        text += "parameter " + id + " " + number_text(parameter.value) + "\n";
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Translating, under the lock.

struct Translation {
    std::string text;
    std::vector<std::string> warnings;
};

// Reads the source (without its byte order mark; `lines` are its lines for test-decay-text),
// checks the module, writes the translation and holds the lock as the source's hold lines say.
Translation translate(const Options& options, std::string_view text,
                      const std::vector<Line>& lines) {
    Model model;
    std::uint64_t hold_milliseconds = 0;
    Translation translation;
    if (options.from == kDecayText) {
        TextModel read = read_text_model(lines);
        model = std::move(read.model);
        hold_milliseconds = read.hold_milliseconds;
        translation.warnings = std::move(read.warnings);
    } else {
        model = read_json_model(text);
    }
    if (options.module.has_value() && *options.module != kMain) {
        fail(PRAENORA_STATUS_NOT_FOUND,
             "the source has no module '" + *options.module + "': its only module is main",
             {{"module", *options.module}});
    }
    translation.text = options.to == kDecayText ? write_text_model(model) : write_json_model(model);
    if (hold_milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_milliseconds));
    }
    return translation;
}

// ---------------------------------------------------------------------------------------------
// The service's functions.

const std::string& description() {
    static const std::string text =
        dump(json{{"service", kService},
                  {"version",
                   {{"major", PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR},
                    {"minor", PRAENORA_MODEL_CONVERSION_SERVICE_MINOR}}},
                  {"engine", json::parse(PRAENORA_PROVIDER_ENGINE)},
                  {"conversions", conversions_json()}});
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

praenora_status PRAENORA_CALL convert(void*, const praenora_host_api* host,
                                      praenora_bytes options_json, praenora_bytes source,
                                      praenora_sink* out_text, praenora_sink* report_json,
                                      praenora_sink* error_json) noexcept {
    if (!usable(error_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return guarded(error_json, [&] {
        // The arguments, then the size limits, before anything is parsed.
        if (!usable(out_text) || !usable(report_json) || !readable(options_json) ||
            !readable(source)) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "convert: out_text or report_json is NULL or has no write function, or the "
                 "options or the source is NULL with a size");
        }
        if (options_json.size > PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE) {
            fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED, "the options are larger than 1 MiB",
                 {{"size", options_json.size},
                  {"limit", PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE}});
        }
        if (source.size > PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE) {
            fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED, "the source is larger than 16 MiB",
                 {{"size", source.size}, {"limit", PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE}});
        }
        // The options, the source text and the rules of its format.
        const Options options = read_options(options_json);
        check_text(view(source));
        const std::string_view text = without_byte_order_mark(view(source));
        if (std::all_of(text.begin(), text.end(), white_space)) {
            fail(PRAENORA_STATUS_DATA_INVALID,
                 "the source is empty or holds only white space (rule empty)", {{"rule", "empty"}});
        }
        std::vector<Line> lines;
        if (options.from == kDecayText) {
            lines = lines_of(text);
            check_imports(lines);
        }
        // The translation, under the lock: cancel_requested() is polled before it is taken and
        // while the call waits for it.
        Translation translation;
        {
            const praenora::providers::CancellableLock lock(translation_lock(), host);
            if (!lock.held()) {
                fail(PRAENORA_STATUS_CANCELLED, "the call was cancelled");
            }
            translation = translate(options, text, lines);
        }
        log_to(host, PRAENORA_LOG_DEBUG,
               "test-conversion: converted " + options.from + " to " + options.to + ", " +
                   std::to_string(source.size) + " bytes to " +
                   std::to_string(translation.text.size()));
        write_to(out_text, translation.text, "the translated text");
        write_to(report_json,
                 dump(json{{"from", options.from},
                           {"to", options.to},
                           {"module", kMain},
                           {"warnings", translation.warnings}}),
                 "the report");
    });
}

const praenora_model_conversion_v0 conversion_table = {
    {PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR, PRAENORA_MODEL_CONVERSION_SERVICE_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_model_conversion_v0)), 0u},
    nullptr,
    &describe,
    &convert,
};

// ---------------------------------------------------------------------------------------------
// The provider.

praenora_status PRAENORA_CALL start(const praenora_provider_host* host, praenora_sink*) noexcept {
    provider_fixture_log(host, PRAENORA_LOG_INFO, "test-conversion started");
    return PRAENORA_STATUS_OK;
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) noexcept {
    if (major == PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR &&
        provider_fixture_name_is(name, name_size, PRAENORA_MODEL_CONVERSION_SERVICE)) {
        return &conversion_table;
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
