// The antimony provider's documents and failures (documents.hpp).
#include "documents.hpp"

#include <algorithm>

namespace praenora::antimony_provider {

[[noreturn]] void fail(praenora_status code, const std::string& message, json details) {
    throw Failure{code, std::string(kService) + ": " + message, std::move(details)};
}

[[noreturn]] void refuse(const std::string& rule, const std::string& why, json details) {
    details["rule"] = rule;
    fail(PRAENORA_STATUS_DATA_INVALID, why + " (rule " + rule + ")", std::move(details));
}

[[noreturn]] void engine_failure(const std::string& type, const std::string& what,
                                 const std::string& message, std::size_t line, std::size_t column) {
    json details = {{"engine_error", {{"type", type}, {"message", message}}}};
    if (line != 0) {
        details["line"] = line;
    }
    if (column != 0) {
        details["column"] = column;
    }
    fail(PRAENORA_STATUS_DATA_INVALID, what + ": " + message, std::move(details));
}

std::string dump(const json& document) {
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

void write_to(praenora_sink* sink, const std::string& text, const char* what) {
    const praenora_status status = sink->write(sink->context, text.data(), text.size());
    if (status != PRAENORA_STATUS_OK) {
        fail(status, std::string("the sink refused ") + what);
    }
}

void write_error(praenora_sink* error_json, praenora_status code, const std::string& message,
                 json details) noexcept {
    try {
        details["service"] = kService;
        const std::string text =
            dump(json{{"code", code}, {"message", message}, {"details", std::move(details)}});
        static_cast<void>(error_json->write(error_json->context, text.data(), text.size()));
    } catch (...) {
        // Out of memory: the status alone reports the failure.
    }
}

void log_to(const praenora_host_api* host, std::int32_t level, const std::string& message) {
    if (host == nullptr ||
        host->struct_size < offsetof(praenora_host_api, log_enabled) + sizeof host->log_enabled ||
        host->log == nullptr || host->log_enabled == nullptr ||
        host->log_enabled(host->context, level) == 0) {
        return;
    }
    host->log(host->context, level, message.data(), message.size());
}

const std::vector<std::pair<std::string, std::string>>& conversions() {
    static const std::vector<std::pair<std::string, std::string>> list = {{kAntimony, kSbml},
                                                                          {kSbml, kAntimony}};
    return list;
}

namespace {

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

} // namespace

Options read_options(praenora_bytes bytes) {
    const std::string_view text = view(bytes);
    const json document = json::parse(text.begin(), text.end(), nullptr, false);
    const json::json_pointer root;
    if (document.is_discarded() || !document.is_object()) {
        invalid_option("the options are not a JSON object", root);
    }
    // nlohmann/json keeps an object's fields in the order of their names (UTF-8 bytes): the first
    // unknown field reported is the first by name.
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
             "the antimony provider does not read the format '" + options.from + "'",
             {{"pointer", "/from"}, {"format", options.from}, {"conversions", conversions_json()}});
    }
    if (std::find(all.begin(), all.end(), std::make_pair(options.from, options.to)) == all.end()) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the antimony provider does not convert " + options.from + " to '" + options.to + "'",
             {{"pointer", "/to"}, {"format", options.to}, {"conversions", conversions_json()}});
    }
    return options;
}

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

} // namespace praenora::antimony_provider
