// The antimony provider: offers praenora.model-conversion@0.0 (include/praenora/sdk/
// model_conversion.h; normative: docs/specs/model-conversion-service.md) for Antimony and SBML,
// with libAntimony as its engine. This file holds the provider interface
// (src/runtime/provider/provider_abi.h), the service's table and the order of work of convert();
// source_checks.cpp holds the rules that read a source's text alone, sbml_checks.cpp those that
// need libSBML, engine.cpp what uses libAntimony, documents.cpp what reads and writes the
// service's documents. No exception leaves a function of the provider.
#include "documents.hpp"
#include "engine.hpp"
#include "provider_abi.h"
#include "source_checks.hpp"

#include "cancellable_lock.hpp"

#include <praenora/sdk/model_conversion.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>

namespace {

using praenora::antimony_provider::conversions;
using praenora::antimony_provider::dump;
using praenora::antimony_provider::fail;
using praenora::antimony_provider::Failure;
using praenora::antimony_provider::guarded;
using praenora::antimony_provider::json;
using praenora::antimony_provider::kAntimony;
using praenora::antimony_provider::kService;
using praenora::antimony_provider::Options;
using praenora::antimony_provider::readable;
using praenora::antimony_provider::refuse;
using praenora::antimony_provider::Translation;
using praenora::antimony_provider::usable;
using praenora::antimony_provider::view;
using praenora::antimony_provider::write_error;
using praenora::antimony_provider::write_to;

// What a rule's refusal says about the source.
std::string why(const std::string& rule) {
    if (rule == "empty") {
        return "the source is empty or holds only white space";
    }
    if (rule == "xml") {
        return "the antimony source is an XML document, which libAntimony would read as SBML; "
               "convert it from sbml";
    }
    if (rule == "import") {
        return "the antimony source imports a file, which libAntimony would read";
    }
    if (rule == "unterminated") {
        return "the antimony source does not close a block comment, a string or a "
               "triple-backtick block";
    }
    if (rule == "not_document") {
        return "the sbml source does not start with '<': it must be the document's text, not a "
               "path or a URI";
    }
    if (rule == "document_type") {
        return "the sbml source has a document type declaration (<!DOCTYPE), which can declare "
               "entities that name files";
    }
    return "the source breaks a rule of its format";
}

// The rules that read the source's text alone, in order: empty, then those of its format.
void check_rules(const Options& options, std::string_view source) {
    const std::string_view text = praenora::antimony_provider::without_byte_order_mark(source);
    if (std::all_of(text.begin(), text.end(), praenora::antimony_provider::is_white_space)) {
        refuse("empty", why("empty"));
    }
    const std::string rule = options.from == kAntimony
                                 ? praenora::antimony_provider::antimony_text_rule(source)
                                 : praenora::antimony_provider::sbml_text_rule(source);
    if (!rule.empty()) {
        refuse(rule, why(rule));
    }
}

// ---------------------------------------------------------------------------------------------
// The service's functions.

json conversions_json() {
    json list = json::array();
    for (const auto& [from, to] : conversions()) {
        list.push_back(json{{"from", from}, {"to", to}});
    }
    return list;
}

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
        // The options, the source's text and the rules that read it alone.
        const Options options = praenora::antimony_provider::read_options(options_json);
        praenora::antimony_provider::check_text(view(source));
        check_rules(options, view(source));
        // The translation, under the lock: cancel_requested() is polled before it is taken and
        // while the call waits for it; the rules that need libSBML's parser run under it too.
        Translation translation;
        {
            const praenora::providers::CancellableLock lock(
                praenora::antimony_provider::engine_lock(), host);
            if (!lock.held()) {
                fail(PRAENORA_STATUS_CANCELLED, "the call was cancelled");
            }
            translation = praenora::antimony_provider::translate(options, view(source));
        }
        praenora::antimony_provider::log_to(host, PRAENORA_LOG_DEBUG,
                                            "antimony: converted " + options.from + " to " +
                                                options.to + " (module " + translation.module +
                                                "), " + std::to_string(source.size) + " bytes to " +
                                                std::to_string(translation.text.size()));
        write_to(out_text, translation.text, "the translated text");
        write_to(report_json,
                 dump(json{{"from", options.from},
                           {"to", options.to},
                           {"module", translation.module},
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

const char kDescription[] =
    "{\"name\":\"" PRAENORA_PROVIDER_NAME "\",\"version\":\"" PRAENORA_PROVIDER_VERSION
    "\",\"services\":" PRAENORA_PROVIDER_SERVICES ",\"engine\":" PRAENORA_PROVIDER_ENGINE "}";

praenora_status PRAENORA_CALL provider_describe(praenora_sink* out_json) noexcept {
    if (!usable(out_json)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return out_json->write(out_json->context, kDescription, std::strlen(kDescription));
}

// Logs `message` at `level` to the provider's own logger.
void log_to_provider(const praenora_provider_host* host, std::int32_t level,
                     const std::string& message) noexcept {
    if (host == nullptr ||
        host->struct_size <
            offsetof(praenora_provider_host, log_enabled) + sizeof host->log_enabled ||
        host->log == nullptr || host->log_enabled == nullptr ||
        host->log_enabled(host->context, level) == 0) {
        return;
    }
    host->log(host->context, level, message.data(), message.size());
}

praenora_status PRAENORA_CALL start(const praenora_provider_host* host,
                                    praenora_sink* error_json) noexcept {
    try {
        const json engine = json::parse(PRAENORA_PROVIDER_ENGINE);
        json linked;
        {
            const std::lock_guard<std::timed_mutex> lock(
                praenora::antimony_provider::engine_lock());
            linked = praenora::antimony_provider::start_engine(engine);
        }
        log_to_provider(host, PRAENORA_LOG_INFO,
                        "antimony started: libAntimony " + linked.at("version").get<std::string>() +
                            ", libSBML " + linked.at("libsbml").get<std::string>() +
                            ", SBMLNetwork " + linked.at("sbmlnetwork").get<std::string>());
        return PRAENORA_STATUS_OK;
    } catch (const Failure& failure) {
        if (usable(error_json)) {
            write_error(error_json, failure.code, failure.message, failure.details);
        }
        return failure.code;
    } catch (const std::exception& error) {
        if (usable(error_json)) {
            write_error(error_json, PRAENORA_STATUS_INTERNAL,
                        std::string("antimony: libAntimony did not start: ") + error.what());
        }
    } catch (...) {
        if (usable(error_json)) {
            write_error(error_json, PRAENORA_STATUS_INTERNAL,
                        "antimony: libAntimony did not start");
        }
    }
    return PRAENORA_STATUS_INTERNAL;
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) noexcept {
    const std::size_t length = std::strlen(PRAENORA_MODEL_CONVERSION_SERVICE);
    if (major == PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR && name != nullptr &&
        name_size == length && std::memcmp(name, PRAENORA_MODEL_CONVERSION_SERVICE, length) == 0) {
        return &conversion_table;
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
