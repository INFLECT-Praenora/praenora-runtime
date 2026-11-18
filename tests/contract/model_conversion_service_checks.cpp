// The service checks of praenora.model-conversion@0 (model_conversion_service_checks.hpp). Every
// check uses only the table, the parameter set and the rules of
// docs/specs/model-conversion-service.md, so that any provider of the service can be checked with a
// parameter set of its own.
#include "model_conversion_service_checks.hpp"

#include "instance/services.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace praenora::test {

void PrintTo(const ModelConversionCase& conversion_case, std::ostream* out) {
    *out << conversion_case.name << " (" << conversion_case.provider_manifest.filename().u8string()
         << ")";
}

std::string model_conversion_case_name(const ::testing::TestParamInfo<ModelConversionCase>& info) {
    return info.param.name;
}

const praenora_model_conversion_v0*
load_model_conversion_table(const std::filesystem::path& provider_manifest) {
    const detail::ProviderManifest manifest = detail::load_provider_manifest(provider_manifest);
    const detail::LoadedProvider& provider =
        detail::load_provider(manifest,
                              {detail::ServiceVersion{PRAENORA_MODEL_CONVERSION_SERVICE,
                                                      PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR,
                                                      PRAENORA_MODEL_CONVERSION_SERVICE_MINOR}},
                              std::nullopt, nullptr);
    return static_cast<const praenora_model_conversion_v0*>(provider.service(
        PRAENORA_MODEL_CONVERSION_SERVICE, PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR));
}

void ModelConversionService::SetUp() {
    const ModelConversionCase& c = GetParam();
    try {
        table_ = load_model_conversion_table(c.provider_manifest);
    } catch (const Error& error) {
        FAIL() << "the provider of " << c.provider_manifest.u8string()
               << " does not load: " << error.what() << " " << error.details_json();
    }
    ASSERT_NE(table_, nullptr) << "the provider offers no praenora.model-conversion@0 table";
    ASSERT_FALSE(c.cases.empty()) << "a parameter set needs at least one translation";
}

namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char* kService = PRAENORA_MODEL_CONVERSION_SERVICE;
constexpr std::size_t kMaxSource = PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE;
constexpr std::size_t kMaxOptions = PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE;

// ---------------------------------------------------------------------------------------------
// Sinks and a host API for the checks.

// A sink that collects what is written to it, or refuses every write with `refusal`.
class TextSink {
public:
    explicit TextSink(praenora_status refusal = PRAENORA_STATUS_OK) : refusal_(refusal) {}
    TextSink(const TextSink&) = delete;
    TextSink& operator=(const TextSink&) = delete;

    praenora_sink* get() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        auto* self = static_cast<TextSink*>(context);
        if (self->refusal_ != PRAENORA_STATUS_OK) {
            return self->refusal_;
        }
        if (data == nullptr && size != 0) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        try {
            self->text_.append(data == nullptr ? "" : data, size);
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        return PRAENORA_STATUS_OK;
    }

    praenora_status refusal_;
    std::string text_;
    praenora_sink sink_{this, &TextSink::write};
};

// A host API that records log messages, counts the calls of cancel_requested() and lets a check
// decide what it reports: nonzero from its n-th call on. Thread-safe.
class CheckHost {
public:
    CheckHost() {
        api_.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
        api_.abi_major = PRAENORA_ABI_MAJOR;
        api_.abi_minor = PRAENORA_ABI_MINOR;
        api_.context = this;
        api_.log = &CheckHost::log;
        api_.log_enabled = &CheckHost::log_enabled;
        api_.cancel_requested = &CheckHost::cancel_requested;
        api_.get_service = &CheckHost::get_service;
    }
    CheckHost(const CheckHost&) = delete;
    CheckHost& operator=(const CheckHost&) = delete;

    const praenora_host_api* api() const noexcept { return &api_; }

    // From the `poll`-th call of cancel_requested() on (1: every call), it reports nonzero; 0:
    // never.
    void cancel_from_poll(int poll) {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancel_from_ = poll;
    }

    int polls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return polls_;
    }

    std::size_t logged() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return logs_.size();
    }

    // Reports a smaller struct_size, as a host of an older minor version or one that lacks the
    // later callbacks; the callbacks beyond it must not be called.
    void set_struct_size(std::size_t size) { api_.struct_size = static_cast<std::uint32_t>(size); }

private:
    static CheckHost* self(void* context) noexcept { return static_cast<CheckHost*>(context); }

    static void PRAENORA_CALL log(void* context, std::int32_t, const char* message,
                                  std::size_t size) noexcept {
        try {
            const std::lock_guard<std::mutex> lock(self(context)->mutex_);
            self(context)->logs_.emplace_back(message == nullptr ? "" : message,
                                              message == nullptr ? 0 : size);
        } catch (...) {
        }
    }

    static std::int32_t PRAENORA_CALL log_enabled(void*, std::int32_t) noexcept { return 1; }

    static std::int32_t PRAENORA_CALL cancel_requested(void* context) noexcept {
        CheckHost* host = self(context);
        const std::lock_guard<std::mutex> lock(host->mutex_);
        ++host->polls_;
        return host->cancel_from_ > 0 && host->polls_ >= host->cancel_from_ ? 1 : 0;
    }

    static const void* PRAENORA_CALL get_service(void*, const char*, std::size_t,
                                                 std::uint32_t) noexcept {
        return nullptr;
    }

    praenora_host_api api_{};
    mutable std::mutex mutex_;
    std::vector<std::string> logs_;
    int polls_ = 0;
    int cancel_from_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Calls.

// What a call of convert() returned and wrote.
struct Call {
    praenora_status status = -1;
    std::string text, report, error;
};

bool operator==(const Call& a, const Call& b) {
    return a.status == b.status && a.text == b.text && a.report == b.report && a.error == b.error;
}

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

// The options of a conversion; `module` is left out when empty.
std::string options_for(const std::string& from, const std::string& to,
                        const std::string& module = "") {
    json options = {{"from", from}, {"to", to}};
    if (!module.empty()) {
        options["module"] = module;
    }
    return options.dump();
}

Call convert(const praenora_model_conversion_v0* t, std::string_view options,
             std::string_view source, const praenora_host_api* host = nullptr) {
    TextSink text, report, error;
    Call call;
    call.status = t->convert(t->context, host, bytes(options), bytes(source), text.get(),
                             report.get(), error.get());
    call.text = text.text();
    call.report = report.text();
    call.error = error.text();
    return call;
}

Call convert_case(const praenora_model_conversion_v0* t, const ConversionExpectation& e,
                  const praenora_host_api* host = nullptr) {
    return convert(t, options_for(e.from, e.to, e.module), e.source, host);
}

std::string shown(const Call& call) {
    return std::string(praenora_status_name(call.status)) + " (" + std::to_string(call.status) +
           "), error '" + call.error + "', report '" + call.report + "', text of " +
           std::to_string(call.text.size()) + " bytes";
}

// ---------------------------------------------------------------------------------------------
// Documents and expectations.

json parse(const std::string& text) { return json::parse(text, nullptr, false); }

// Whether `text` is the canonical JSON of the document it holds: compact, keys sorted.
bool canonical(const std::string& text) {
    const json document = parse(text);
    return !document.is_discarded() && document.dump() == text;
}

// The details of a failed call's error document, or {} when it has none.
json details_of(const Call& call) {
    const json document = parse(call.error);
    if (!document.is_object() || !document.contains("details") ||
        !document.at("details").is_object()) {
        return json::object();
    }
    return document.at("details");
}

// Expects `call` to have failed with `code`: nothing written to out_text or report_json, and an
// error document of the pmp-error shape, canonical, with details.service and, when given,
// details.pointer `pointer`.
void expect_failure(const Call& call, praenora_status code,
                    const std::optional<std::string>& pointer = std::nullopt) {
    EXPECT_EQ(call.status, code) << shown(call);
    EXPECT_EQ(call.text, "") << "a failed call wrote text: " << shown(call);
    EXPECT_EQ(call.report, "") << "a failed call wrote a report: " << shown(call);
    EXPECT_TRUE(canonical(call.error)) << "error document '" << call.error << "'";
    const json document = parse(call.error);
    ASSERT_TRUE(document.is_object()) << "error document: '" << call.error << "'";
    EXPECT_EQ(document.value("code", -1), code) << call.error;
    EXPECT_TRUE(document.contains("message") && document.at("message").is_string() &&
                !document.at("message").get<std::string>().empty())
        << call.error;
    ASSERT_TRUE(document.contains("details") && document.at("details").is_object()) << call.error;
    EXPECT_EQ(document.at("details").value("service", ""), kService) << call.error;
    if (pointer.has_value()) {
        EXPECT_EQ(document.at("details").value("pointer", "<none>"), *pointer) << call.error;
    }
}

// The report a translation must write: {"from", "to", "module", "warnings"}, canonical.
std::string expected_report(const ConversionExpectation& e) {
    return json{
        {"from", e.from}, {"to", e.to}, {"module", e.expected_module}, {"warnings", e.warnings}}
        .dump();
}

// Expects `call` to be the translation `e`, exactly.
void expect_translation(const Call& call, const ConversionExpectation& e) {
    ASSERT_EQ(call.status, PRAENORA_STATUS_OK) << e.name << ": " << shown(call);
    EXPECT_EQ(call.text, e.output) << e.name;
    EXPECT_TRUE(canonical(call.report)) << e.name << ": report '" << call.report << "'";
    EXPECT_EQ(call.report, expected_report(e)) << e.name;
    EXPECT_EQ(call.error, "") << e.name << ": a successful call wrote an error document";
}

// The first format the parameter set's conversions pair with `from`; empty when none does.
std::string to_for(const ModelConversionCase& c, const std::string& from) {
    for (const auto& [source, target] : c.conversions) {
        if (source == from) {
            return target;
        }
    }
    return "";
}

// One conversion for each format the provider reads: the first one with that `from`.
std::vector<std::pair<std::string, std::string>> one_per_format(const ModelConversionCase& c) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& conversion : c.conversions) {
        if (std::none_of(result.begin(), result.end(),
                         [&](const auto& chosen) { return chosen.first == conversion.first; })) {
            result.push_back(conversion);
        }
    }
    return result;
}

bool has_conversion(const ModelConversionCase& c, const std::string& from, const std::string& to) {
    return std::find(c.conversions.begin(), c.conversions.end(), std::make_pair(from, to)) !=
           c.conversions.end();
}

// ---------------------------------------------------------------------------------------------
// The provider's lock, seen from outside.

// A conversion running on a thread of its own, joined when it goes.
class Background {
public:
    Background(const praenora_model_conversion_v0* t, std::string options, std::string source,
               const praenora_host_api* host = nullptr)
        : options_(std::move(options)), source_(std::move(source)), thread_([this, t, host] {
              call_ = convert(t, options_, source_, host);
              finished_ = Clock::now();
              done_.store(true);
          }) {}
    Background(const Background&) = delete;
    Background& operator=(const Background&) = delete;
    ~Background() { join(); }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    bool done() const { return done_.load(); }
    // After join().
    const Call& call() const { return call_; }
    Clock::time_point finished() const { return finished_; }

private:
    std::string options_, source_;
    Call call_;
    Clock::time_point finished_{};
    std::atomic<bool> done_{false};
    std::thread thread_; // last: it starts once everything else exists
};

// How many times a conversion of `e` that nothing makes wait polls cancel_requested().
int uncontended_polls(const praenora_model_conversion_v0* t, const ConversionExpectation& e) {
    CheckHost host;
    const Call call = convert_case(t, e, host.api());
    EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << shown(call);
    return host.polls();
}

// Converts `e` with a host that reports a cancellation from its poll `polls + 1` on: a call that
// finds the provider's lock free polls only `polls` times and converts; one that has to wait for
// the lock polls again while it waits, and is cancelled.
Call probe(const praenora_model_conversion_v0* t, const ConversionExpectation& e, int polls) {
    CheckHost host;
    host.cancel_from_poll(polls + 1);
    return convert_case(t, e, host.api());
}

// Probes the lock until a probe has to wait for it, as long as `held` runs, and returns that
// probe's call (cancelled while waiting); nothing when `held` finished first.
std::optional<Call> probe_until_waiting(const praenora_model_conversion_v0* t,
                                        const ConversionExpectation& e, int polls,
                                        const Background& held) {
    while (!held.done()) {
        Call call = probe(t, e, polls);
        if (call.status != PRAENORA_STATUS_OK) {
            return call;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

} // namespace

ConversionResult convert_source(const praenora_model_conversion_v0* table,
                                std::string_view options_json, std::string_view source) {
    const Call call = convert(table, options_json, source);
    return ConversionResult{call.status, call.text, call.report, call.error};
}

// ---------------------------------------------------------------------------------------------
// The table and describe.

TEST_P(ModelConversionService, TableHeaderAndFunctions) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    EXPECT_EQ(t->header.abi_major, PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR);
    EXPECT_EQ(t->header.reserved, 0u);
    EXPECT_GE(t->header.struct_size, PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE);
    EXPECT_TRUE(t->describe != nullptr && t->convert != nullptr)
        << "every function of the table is required";
    // The minor the table reports is the one the manifest offers.
    std::ifstream manifest_file(c.provider_manifest, std::ios::binary);
    const json manifest = json::parse(manifest_file);
    bool offered = false;
    for (const json& service : manifest.at("services")) {
        if (service.at("name") == kService &&
            service.at("major") == PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR) {
            offered = true;
            EXPECT_EQ(service.at("minor"), t->header.abi_minor) << manifest.dump();
        }
    }
    EXPECT_TRUE(offered) << "the manifest offers no praenora.model-conversion@0: "
                         << manifest.dump();
}

TEST_P(ModelConversionService, DescribeMatchesManifest) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    TextSink out;
    ASSERT_EQ(t->describe(t->context, out.get()), PRAENORA_STATUS_OK);
    EXPECT_TRUE(canonical(out.text())) << out.text();
    const json described = parse(out.text());
    ASSERT_TRUE(described.is_object()) << out.text();
    EXPECT_EQ(described.value("service", ""), kService) << out.text();
    EXPECT_EQ(
        described.value("version", json()),
        json({{"major", PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR}, {"minor", t->header.abi_minor}}))
        << out.text();

    // The engine is the one of the provider's manifest, which the run fingerprint records, and
    // the one the parameter set names.
    std::ifstream manifest_file(c.provider_manifest, std::ios::binary);
    const json manifest = json::parse(manifest_file);
    EXPECT_EQ(described.value("engine", json()), manifest.at("engine")) << out.text();
    EXPECT_EQ(described.value("engine", json()), json::parse(c.engine_json)) << out.text();

    // The conversions: not empty, each {"from", "to"}, sorted by from, then to, without
    // duplicates, and the parameter set's.
    ASSERT_TRUE(described.contains("conversions") && described.at("conversions").is_array())
        << out.text();
    std::vector<std::pair<std::string, std::string>> conversions;
    for (const json& conversion : described.at("conversions")) {
        ASSERT_TRUE(conversion.is_object() && conversion.size() == 2 &&
                    conversion.contains("from") && conversion.at("from").is_string() &&
                    conversion.contains("to") && conversion.at("to").is_string())
            << conversion.dump();
        conversions.emplace_back(conversion.at("from").get<std::string>(),
                                 conversion.at("to").get<std::string>());
    }
    EXPECT_FALSE(conversions.empty());
    EXPECT_TRUE(std::is_sorted(conversions.begin(), conversions.end())) << out.text();
    EXPECT_TRUE(std::adjacent_find(conversions.begin(), conversions.end()) == conversions.end())
        << out.text();
    EXPECT_EQ(conversions, c.conversions) << out.text();

    // Free of side effects: the same bytes every time, from any thread.
    TextSink again;
    ASSERT_EQ(t->describe(t->context, again.get()), PRAENORA_STATUS_OK);
    EXPECT_EQ(again.text(), out.text());
    std::string from_thread;
    std::thread([&] {
        TextSink other;
        if (t->describe(t->context, other.get()) == PRAENORA_STATUS_OK) {
            from_thread = other.text();
        }
    }).join();
    EXPECT_EQ(from_thread, out.text());
}

// ---------------------------------------------------------------------------------------------
// Translations.

TEST_P(ModelConversionService, CasesConvertExactly) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    for (const auto& conversion : c.conversions) {
        EXPECT_TRUE(std::any_of(c.cases.begin(), c.cases.end(),
                                [&conversion](const ConversionExpectation& e) {
                                    return e.from == conversion.first && e.to == conversion.second;
                                }))
            << "the parameter set has no translation from " << conversion.first << " to "
            << conversion.second;
    }
    for (const ConversionExpectation& e : c.cases) {
        SCOPED_TRACE(e.name);
        CheckHost host;
        const Call call = convert_case(t, e, host.api());
        expect_translation(call, e);
        // It polled for cancellation before the translation, and logged the conversion.
        EXPECT_GE(host.polls(), 1);
        EXPECT_GE(host.logged(), 1u);
    }
}

TEST_P(ModelConversionService, RepeatedCallsAreIdentical) {
    // The same input always gets the same output, or the same error.
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    for (const ConversionExpectation& e : c.cases) {
        const Call first = convert_case(t, e);
        for (int round = 0; round < 3; ++round) {
            EXPECT_TRUE(convert_case(t, e) == first) << e.name << ": " << shown(first);
        }
    }
    for (const ConversionRefusal& r : c.refusals) {
        const Call first = convert(t, options_for(r.from, r.to), r.source);
        for (int round = 0; round < 3; ++round) {
            EXPECT_TRUE(convert(t, options_for(r.from, r.to), r.source) == first)
                << r.name << ": " << shown(first);
        }
    }
    if (!c.failing_source.empty()) {
        const std::string options = options_for(c.failing_from, to_for(c, c.failing_from));
        const Call first = convert(t, options, c.failing_source);
        for (int round = 0; round < 3; ++round) {
            EXPECT_TRUE(convert(t, options, c.failing_source) == first) << shown(first);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Sources the provider refuses.

TEST_P(ModelConversionService, RefusalsNameTheirRule) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    for (const ConversionRefusal& r : c.refusals) {
        SCOPED_TRACE(r.name);
        const Call call = convert(t, options_for(r.from, r.to), r.source);
        expect_failure(call, r.status, r.pointer);
        const json details = details_of(call);
        if (r.rule.empty()) {
            EXPECT_FALSE(details.contains("rule")) << call.error;
        } else {
            EXPECT_EQ(details.value("rule", ""), r.rule) << call.error;
            // A refused source never reaches the translation: a refusal is not an engine
            // failure.
            EXPECT_FALSE(details.contains("engine_error")) << call.error;
        }
    }
}

TEST_P(ModelConversionService, EngineFailureIsDataInvalid) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    ASSERT_FALSE(c.failing_source.empty()) << "a parameter set names a source the engine refuses";
    const std::string to = to_for(c, c.failing_from);
    ASSERT_FALSE(to.empty()) << "no conversion reads " << c.failing_from;
    const Call call = convert(t, options_for(c.failing_from, to), c.failing_source);
    expect_failure(call, PRAENORA_STATUS_DATA_INVALID);
    const json details = details_of(call);
    EXPECT_FALSE(details.contains("rule")) << call.error;
    ASSERT_TRUE(details.contains("engine_error") && details.at("engine_error").is_object())
        << call.error;
    const json& engine_error = details.at("engine_error");
    for (const char* member : {"type", "message"}) {
        EXPECT_TRUE(engine_error.contains(member) && engine_error.at(member).is_string() &&
                    !engine_error.at(member).get<std::string>().empty())
            << member << ": " << call.error;
    }
    for (const char* location : {"line", "column"}) {
        if (details.contains(location)) {
            EXPECT_TRUE(details.at(location).is_number_unsigned() &&
                        details.at(location).get<std::uint64_t>() >= 1)
                << location << ": " << call.error;
        }
    }
    if (c.failing_line > 0) {
        EXPECT_EQ(details.value("line", json()), json(c.failing_line)) << call.error;
    }
}

TEST_P(ModelConversionService, EmptySourceIsRefused) {
    // Empty, or only white space after an optional byte order mark, in every format the provider
    // reads: never an empty model.
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const std::vector<std::string> empty_sources = {"",
                                                    " ",
                                                    "    ",
                                                    "\n\n\n",
                                                    "\r\n",
                                                    "\r\n\r\n",
                                                    " \t\r\n\v\f",
                                                    "\xEF\xBB\xBF",
                                                    "\xEF\xBB\xBF \t\r\n",
                                                    std::string(4096, ' ')};
    for (const auto& [from, to] : one_per_format(c)) {
        for (const std::string& source : empty_sources) {
            SCOPED_TRACE(from + ", a source of " + std::to_string(source.size()) + " bytes");
            const Call call = convert(t, options_for(from, to), source);
            expect_failure(call, PRAENORA_STATUS_DATA_INVALID);
            EXPECT_EQ(details_of(call).value("rule", ""), "empty") << call.error;
        }
    }
}

TEST_P(ModelConversionService, OptionsErrorsHavePointers) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const std::vector<std::pair<std::string, std::string>> refused = {
        // Not a JSON object.
        {"", ""},
        {"not json", ""},
        {"{", ""},
        {"[]", ""},
        {"null", ""},
        {"\"from\"", ""},
        {"{\"from\":\"\xff\"}", ""},
        {json{{"from", e.from}, {"to", e.to}}.dump() + " {}", ""},
        // Unknown fields, missing and wrong-typed fields, an empty module.
        {json{{"from", e.from}, {"to", e.to}, {"bogus", 1}}.dump(), "/bogus"},
        {json{{"from", e.from}, {"to", e.to}, {"From", e.from}}.dump(), "/From"},
        {json{{"to", e.to}}.dump(), "/from"},
        {json{{"from", e.from}}.dump(), "/to"},
        {"{}", "/from"},
        {json{{"from", 7}, {"to", e.to}}.dump(), "/from"},
        {json{{"from", nullptr}, {"to", e.to}}.dump(), "/from"},
        {json{{"from", e.from}, {"to", nullptr}}.dump(), "/to"},
        {json{{"from", e.from}, {"to", json::array({e.to})}}.dump(), "/to"},
        {json{{"from", e.from}, {"to", e.to}, {"module", 7}}.dump(), "/module"},
        {json{{"from", e.from}, {"to", e.to}, {"module", nullptr}}.dump(), "/module"},
        {json{{"from", e.from}, {"to", e.to}, {"module", json::object()}}.dump(), "/module"},
        {json{{"from", e.from}, {"to", e.to}, {"module", ""}}.dump(), "/module"},
        // The document's own problems come before formats it names that the provider does not
        // know.
        {json{{"from", "no-such-format"}, {"to", e.to}, {"module", ""}}.dump(), "/module"},
    };
    for (const auto& [options, pointer] : refused) {
        SCOPED_TRACE("options '" + options + "'");
        expect_failure(convert(t, options, e.source), PRAENORA_STATUS_INVALID_ARGUMENT, pointer);
    }
    // Options are read before the source: an invalid source with invalid options fails for the
    // options.
    expect_failure(convert(t, "[]", std::string("\0\xff", 2)), PRAENORA_STATUS_INVALID_ARGUMENT,
                   "");
    // Bytes whose data is NULL but whose size is not 0.
    TextSink text, report, error;
    EXPECT_EQ(t->convert(t->context, nullptr, praenora_bytes{nullptr, 3}, bytes(e.source),
                         text.get(), report.get(), error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(text.text(), "");
    EXPECT_EQ(report.text(), "");
}

TEST_P(ModelConversionService, UnsupportedFormatsAndPairs) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    expect_failure(convert(t, options_for("no-such-format", e.to), e.source),
                   PRAENORA_STATUS_UNSUPPORTED, "/from");
    expect_failure(convert(t, options_for(e.from, "no-such-format"), e.source),
                   PRAENORA_STATUS_UNSUPPORTED, "/to");
    // Format names are compared exactly.
    std::string upper = e.from;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char ch) {
        return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    });
    if (upper != e.from) {
        expect_failure(convert(t, options_for(upper, e.to), e.source), PRAENORA_STATUS_UNSUPPORTED,
                       "/from");
    }
    // A pair describe() does not list, from a format the provider reads: a format to itself.
    for (const auto& [from, to] : one_per_format(c)) {
        if (!has_conversion(c, from, from)) {
            expect_failure(convert(t, options_for(from, from), e.source),
                           PRAENORA_STATUS_UNSUPPORTED, "/to");
        }
    }
    // The options come before the source: an unsupported format with a source that is not UTF-8.
    expect_failure(convert(t, options_for("no-such-format", e.to), std::string("\xff\xfe", 2)),
                   PRAENORA_STATUS_UNSUPPORTED, "/from");
}

TEST_P(ModelConversionService, UnknownModuleIsNotFound) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const Call call = convert(t, options_for(e.from, e.to, "no_such_module"), e.source);
    expect_failure(call, PRAENORA_STATUS_NOT_FOUND);
    EXPECT_EQ(details_of(call).value("module", ""), "no_such_module") << call.error;

    if (!c.multi_module_source.empty()) {
        const std::string to = to_for(c, c.multi_module_from);
        ASSERT_FALSE(to.empty()) << "no conversion reads " << c.multi_module_from;
        // Without "module", the main module is translated.
        const Call main = convert(t, options_for(c.multi_module_from, to), c.multi_module_source);
        ASSERT_EQ(main.status, PRAENORA_STATUS_OK) << shown(main);
        const json report = parse(main.report);
        ASSERT_TRUE(report.is_object()) << main.report;
        EXPECT_FALSE(report.value("module", "").empty()) << main.report;
        // A name the source does not have, though it has several modules.
        const Call unknown = convert(t, options_for(c.multi_module_from, to, "no_such_module"),
                                     c.multi_module_source);
        expect_failure(unknown, PRAENORA_STATUS_NOT_FOUND);
        EXPECT_EQ(details_of(unknown).value("module", ""), "no_such_module") << unknown.error;
    }
}

TEST_P(ModelConversionService, SourceMustBeUtf8WithoutNul) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const std::string options = options_for(e.from, e.to);
    // A lone continuation byte, an overlong form, a lead byte without its continuation, a
    // surrogate, a code point above U+10FFFF, a byte that never occurs, a truncated sequence.
    const std::vector<std::string> not_utf8 = {
        "\x80", "\xc0\xaf", "\xc3", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff", "\xe2\x82"};
    for (const std::string& bad : not_utf8) {
        for (const std::string& source : {bad + e.source, e.source + bad}) {
            SCOPED_TRACE("a source with " + std::to_string(bad.size()) +
                         " bytes that are not "
                         "UTF-8");
            const Call call = convert(t, options, source);
            expect_failure(call, PRAENORA_STATUS_INVALID_ARGUMENT);
            EXPECT_EQ(details_of(call).value("reason", ""), "utf8") << call.error;
        }
    }
    const std::string nul(1, '\0');
    for (const std::string& source :
         {nul + e.source, e.source + nul,
          e.source.substr(0, e.source.size() / 2) + nul + e.source.substr(e.source.size() / 2),
          nul}) {
        SCOPED_TRACE("a source with a NUL byte");
        const Call call = convert(t, options, source);
        expect_failure(call, PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(details_of(call).value("reason", ""), "nul") << call.error;
    }
    // Bytes whose data is NULL but whose size is not 0.
    TextSink text, report, error;
    EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), praenora_bytes{nullptr, 3},
                         text.get(), report.get(), error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(text.text(), "");
    EXPECT_EQ(report.text(), "");
}

TEST_P(ModelConversionService, SizeLimitsBeforeParsing) {
    // The limits are checked before anything is parsed: the oversized inputs below are invalid in
    // every way (a NUL byte, bytes that are not UTF-8, not JSON), so a provider that read them
    // first would report something else.
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const std::string options = options_for(e.from, e.to, e.module);
    std::string source(kMaxSource + 1, '\xff');
    source[1] = '\0';
    std::string junk_options(kMaxOptions + 1, '\xff');
    junk_options[1] = '\0';

    const auto expect_limit = [](const Call& call, std::size_t size, std::size_t limit) {
        expect_failure(call, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
        const json details = details_of(call);
        EXPECT_EQ(details.value("size", json()), json(size)) << call.error;
        EXPECT_EQ(details.value("limit", json()), json(limit)) << call.error;
    };
    {
        SCOPED_TRACE("a source over the limit");
        expect_limit(convert(t, options, source), kMaxSource + 1, kMaxSource);
    }
    {
        SCOPED_TRACE("a source over the limit, with options that are not JSON");
        expect_limit(convert(t, "x", source), kMaxSource + 1, kMaxSource);
    }
    {
        SCOPED_TRACE("options over the limit");
        expect_limit(convert(t, junk_options, e.source), kMaxOptions + 1, kMaxOptions);
    }
    {
        SCOPED_TRACE("options and a source over their limits: the options first");
        expect_limit(convert(t, junk_options, source), kMaxOptions + 1, kMaxOptions);
    }
    // At the limits, nothing is refused for its size: options padded with white space to the
    // limit convert, and a source of the limit's size is read (only white space: rule "empty").
    std::string padded = options;
    padded.append(kMaxOptions - padded.size(), ' ');
    expect_translation(convert(t, padded, e.source), e);
    const Call blank = convert(t, options, std::string(kMaxSource, ' '));
    expect_failure(blank, PRAENORA_STATUS_DATA_INVALID);
    EXPECT_EQ(details_of(blank).value("rule", ""), "empty") << blank.error;
}

// ---------------------------------------------------------------------------------------------
// Sinks and hosts.

TEST_P(ModelConversionService, NullAndRefusingSinks) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const std::string options = options_for(e.from, e.to, e.module);
    const praenora_status refusal = PRAENORA_STATUS_IO_ERROR; // what the refusing sinks return
    praenora_sink no_write{nullptr, nullptr};

    // An error document of the pmp-error shape with code `code`.
    const auto expect_error_document = [](const std::string& error, praenora_status code) {
        const json document = parse(error);
        ASSERT_TRUE(document.is_object()) << "error document '" << error << "'";
        EXPECT_EQ(document.value("code", -1), code) << error;
        EXPECT_EQ(document.value("details", json::object()).value("service", ""), kService)
            << error;
    };

    // describe's sink.
    EXPECT_EQ(t->describe(t->context, nullptr), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(t->describe(t->context, &no_write), PRAENORA_STATUS_INVALID_ARGUMENT);
    {
        TextSink refusing(refusal);
        EXPECT_EQ(t->describe(t->context, refusing.get()), refusal);
    }

    for (praenora_sink* missing : {static_cast<praenora_sink*>(nullptr), &no_write}) {
        SCOPED_TRACE(missing == nullptr ? "NULL sink" : "sink without write");
        {
            // out_text: INVALID_ARGUMENT, with an error document.
            TextSink report, error;
            EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), bytes(e.source), missing,
                                 report.get(), error.get()),
                      PRAENORA_STATUS_INVALID_ARGUMENT);
            EXPECT_EQ(report.text(), "");
            expect_error_document(error.text(), PRAENORA_STATUS_INVALID_ARGUMENT);
        }
        {
            // report_json: INVALID_ARGUMENT, with an error document, and no text.
            TextSink text, error;
            EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), bytes(e.source), text.get(),
                                 missing, error.get()),
                      PRAENORA_STATUS_INVALID_ARGUMENT);
            EXPECT_EQ(text.text(), "");
            expect_error_document(error.text(), PRAENORA_STATUS_INVALID_ARGUMENT);
        }
        {
            // error_json: INVALID_ARGUMENT, and nothing is written.
            TextSink text, report;
            EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), bytes(e.source), text.get(),
                                 report.get(), missing),
                      PRAENORA_STATUS_INVALID_ARGUMENT);
            EXPECT_EQ(text.text(), "");
            EXPECT_EQ(report.text(), "");
        }
    }

    {
        // out_text refuses: the call ends with its status, after an error document; no report.
        TextSink refusing(refusal), report, error;
        EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), bytes(e.source), refusing.get(),
                             report.get(), error.get()),
                  refusal);
        EXPECT_EQ(report.text(), "");
        expect_error_document(error.text(), refusal);
    }
    {
        // report_json refuses: the text was written first; the call ends with the sink's status,
        // after an error document.
        TextSink text, refusing(refusal), error;
        EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), bytes(e.source), text.get(),
                             refusing.get(), error.get()),
                  refusal);
        EXPECT_EQ(text.text(), e.output);
        expect_error_document(error.text(), refusal);
    }
    {
        // error_json refuses: a successful call never writes it; a failed call keeps its status.
        TextSink text, report, refusing(refusal);
        EXPECT_EQ(t->convert(t->context, nullptr, bytes(options), bytes(e.source), text.get(),
                             report.get(), refusing.get()),
                  PRAENORA_STATUS_OK);
        EXPECT_EQ(text.text(), e.output);
        EXPECT_EQ(report.text(), expected_report(e));
        TextSink other_text, other_report, other_refusing(refusal);
        EXPECT_EQ(t->convert(t->context, nullptr, bytes("[]"), bytes(e.source), other_text.get(),
                             other_report.get(), other_refusing.get()),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(other_text.text(), "");
        EXPECT_EQ(other_report.text(), "");
    }
}

TEST_P(ModelConversionService, NullHostAndShortHost) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    // A NULL host: no logging and no cancellation; the same translations.
    for (const ConversionExpectation& e : c.cases) {
        expect_translation(convert_case(t, e, nullptr), e);
    }
    // A host whose struct_size ends before a callback: the callbacks beyond it are not called.
    for (const std::size_t size :
         {offsetof(praenora_host_api, log), offsetof(praenora_host_api, cancel_requested)}) {
        SCOPED_TRACE("struct_size " + std::to_string(size));
        CheckHost host;
        host.cancel_from_poll(1); // a call that polls anyway is cancelled
        host.set_struct_size(size);
        for (const ConversionExpectation& e : c.cases) {
            expect_translation(convert_case(t, e, host.api()), e);
        }
        EXPECT_EQ(host.polls(), 0);
        if (size <= offsetof(praenora_host_api, log)) {
            EXPECT_EQ(host.logged(), 0u);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Cancellation, serialization and threads.

TEST_P(ModelConversionService, CancelledBeforeWork) {
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    CheckHost cancelled;
    cancelled.cancel_from_poll(1);
    for (const ConversionExpectation& e : c.cases) {
        SCOPED_TRACE(e.name);
        expect_failure(convert_case(t, e, cancelled.api()), PRAENORA_STATUS_CANCELLED);
    }
    EXPECT_GE(cancelled.polls(), static_cast<int>(c.cases.size()));
    // Nothing is left behind: the next calls convert.
    for (const ConversionExpectation& e : c.cases) {
        expect_translation(convert_case(t, e), e);
    }
}

TEST_P(ModelConversionService, CancelledWhileWaitingForTheLock) {
    const ModelConversionCase& c = GetParam();
    if (c.hold_source.empty()) {
        GTEST_SKIP() << "the parameter set has no conversion that holds the provider's lock";
    }
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const int polls = uncontended_polls(t, e);
    const std::string hold_options = options_for(c.hold_from, to_for(c, c.hold_from));
    Background held(t, hold_options, c.hold_source);
    // A call that has to wait for the lock while the held conversion has it polls while it waits,
    // and returns CANCELLED once cancel_requested() reports nonzero.
    const std::optional<Call> waited = probe_until_waiting(t, e, polls, held);
    held.join();
    ASSERT_TRUE(waited.has_value())
        << "no call had to wait for the provider's lock while a held conversion ran";
    expect_failure(*waited, PRAENORA_STATUS_CANCELLED);
    // The held conversion is not disturbed, and the lock is free again.
    EXPECT_EQ(held.call().status, PRAENORA_STATUS_OK) << shown(held.call());
    expect_translation(probe(t, e, polls), e);
}

TEST_P(ModelConversionService, ConversionsAreSerialized) {
    const ModelConversionCase& c = GetParam();
    if (c.hold_source.empty()) {
        GTEST_SKIP() << "the parameter set has no conversion that holds the provider's lock";
    }
    const praenora_model_conversion_v0* t = table();
    const ConversionExpectation& e = c.cases.front();
    const int polls = uncontended_polls(t, e);
    const std::string hold_options = options_for(c.hold_from, to_for(c, c.hold_from));
    // The held conversion alone: how long it takes, and how often it polls when nothing makes it
    // wait. It is timed again after the pair below, and the shorter time is the one compared: a
    // hold that is a computation (not a sleep) takes longer when the machine is busy, and the bound
    // then fails only if both times were taken under more load than the pair, not whenever one was.
    const auto held_alone = [&](CheckHost& host, Clock::duration& duration) {
        const Clock::time_point start = Clock::now();
        const Call call = convert(t, hold_options, c.hold_source, host.api());
        duration = Clock::now() - start;
        return call;
    };
    CheckHost alone_host;
    Clock::duration alone_duration{};
    const Call alone = held_alone(alone_host, alone_duration);
    ASSERT_EQ(alone.status, PRAENORA_STATUS_OK) << shown(alone);
    const int alone_polls = alone_host.polls();

    // A second held conversion, started while the first has the lock, waits for it (it polls more
    // than alone) and holds the lock itself only once the first has released it: it finishes at
    // least about one held conversion after the first (half of one, here). Without serialization
    // both would hold at once and finish close together. Both translate as alone.
    CheckHost second_host;
    std::optional<Call> waited;
    Clock::duration gap{};
    {
        Background first(t, hold_options, c.hold_source);
        waited = probe_until_waiting(t, e, polls, first);
        ASSERT_TRUE(waited.has_value())
            << "no call had to wait for the provider's lock while a held conversion ran";
        Background second(t, hold_options, c.hold_source, second_host.api());
        first.join();
        second.join();
        EXPECT_TRUE(first.call() == alone) << shown(first.call());
        EXPECT_TRUE(second.call() == alone) << shown(second.call());
        gap = second.finished() - first.finished();
    }
    CheckHost after_host;
    Clock::duration after_duration{};
    EXPECT_TRUE(held_alone(after_host, after_duration) == alone);
    const Clock::duration shortest = std::min(alone_duration, after_duration);
    EXPECT_GE(gap, shortest / 2)
        << "the second call finished "
        << std::chrono::duration_cast<std::chrono::milliseconds>(gap).count()
        << " ms after the first, though a held conversion alone takes "
        << std::chrono::duration_cast<std::chrono::milliseconds>(shortest).count()
        << " ms: the second did not wait until the first's translation was done";
    EXPECT_GT(second_host.polls(), alone_polls) << "the second call did not wait for the lock";
    expect_failure(*waited, PRAENORA_STATUS_CANCELLED);
}

TEST_P(ModelConversionService, ConcurrentCallsMatchSequential) {
    // Eight threads at once, each converting every case (and refusing every refusal) several
    // times: the same bytes as one thread alone. Run under ThreadSanitizer in the *-tsan builds.
    const ModelConversionCase& c = GetParam();
    const praenora_model_conversion_v0* t = table();
    constexpr std::size_t kThreads = 8;
    constexpr int kRounds = 5;
    struct Work {
        std::string options, source;
        Call expected;
    };
    std::vector<Work> work;
    for (const ConversionExpectation& e : c.cases) {
        work.push_back({options_for(e.from, e.to, e.module), e.source, {}});
    }
    for (const ConversionRefusal& r : c.refusals) {
        work.push_back({options_for(r.from, r.to), r.source, {}});
    }
    for (Work& item : work) {
        item.expected = convert(t, item.options, item.source);
    }
    for (std::size_t i = 0; i < c.cases.size(); ++i) {
        expect_translation(work[i].expected, c.cases[i]);
    }

    std::vector<std::string> problems(kThreads);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            CheckHost host;
            for (int round = 0; round < kRounds; ++round) {
                for (std::size_t k = 0; k < work.size(); ++k) {
                    const Work& item = work[(k + i) % work.size()]; // each thread its own order
                    const Call call = convert(t, item.options, item.source, host.api());
                    if (!(call == item.expected)) {
                        problems[i] = "round " + std::to_string(round) + ": " + shown(call) +
                                      " instead of " + shown(item.expected);
                        return;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 0; i < kThreads; ++i) {
        EXPECT_EQ(problems[i], "") << "thread " << i;
    }
}

} // namespace praenora::test
