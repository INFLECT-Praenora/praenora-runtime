// What the roadrunner provider (src/providers/roadrunner) does beyond the rules every provider of
// praenora.simulation@0 follows (simulation_service_checks.hpp): its engine identity, the model
// texts it refuses, the ids of an SBML model, events, solver settings, the step limit, solver
// steps and cancellation within a run (docs/specs/simulation-service.md, the roadrunner provider's
// section). Built with PRAENORA_WITH_ROADRUNNER only.
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/simulation.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

std::string read_model(std::string_view name) {
    std::ifstream in(praenora::test::fixture_path("models/sbml/" + std::string(name)),
                     std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A sink that keeps what is written to it.
class TextSink {
public:
    TextSink() = default;
    TextSink(const TextSink&) = delete;
    TextSink& operator=(const TextSink&) = delete;

    praenora_sink* get() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        try {
            static_cast<TextSink*>(context)->text_.append(data == nullptr ? "" : data, size);
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        return PRAENORA_STATUS_OK;
    }

    std::string text_;
    praenora_sink sink_{this, &TextSink::write};
};

// A host API whose cancel_requested() reports nonzero from its n-th call on (0: never).
class CancellingHost {
public:
    explicit CancellingHost(int from_poll) : from_poll_(from_poll) {
        api_.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
        api_.abi_major = PRAENORA_ABI_MAJOR;
        api_.abi_minor = PRAENORA_ABI_MINOR;
        api_.context = this;
        api_.log = [](void*, std::int32_t, const char*, std::size_t) noexcept {};
        api_.log_enabled = [](void*, std::int32_t) noexcept -> std::int32_t { return 0; };
        api_.cancel_requested = &CancellingHost::cancel_requested;
        api_.get_service = [](void*, const char*, std::size_t,
                              std::uint32_t) noexcept -> const void* { return nullptr; };
    }
    CancellingHost(const CancellingHost&) = delete;
    CancellingHost& operator=(const CancellingHost&) = delete;

    const praenora_host_api* api() const noexcept { return &api_; }
    int polls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return polls_;
    }

private:
    static std::int32_t PRAENORA_CALL cancel_requested(void* context) noexcept {
        auto* self = static_cast<CancellingHost*>(context);
        const std::lock_guard<std::mutex> lock(self->mutex_);
        ++self->polls_;
        return self->from_poll_ > 0 && self->polls_ >= self->from_poll_ ? 1 : 0;
    }

    praenora_host_api api_{};
    mutable std::mutex mutex_;
    int polls_ = 0;
    int from_poll_;
};

// What a call returned and wrote.
struct Call {
    praenora_status status = -1;
    std::string output, error;
};

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

class RoadrunnerProvider : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            const praenora::detail::ProviderManifest manifest =
                praenora::detail::load_provider_manifest(
                    praenora::test::utf8_path(PRAENORA_TEST_ROADRUNNER_MANIFEST));
            const praenora::detail::LoadedProvider& provider = praenora::detail::load_provider(
                manifest,
                {praenora::detail::ServiceVersion{PRAENORA_SIMULATION_SERVICE,
                                                  PRAENORA_SIMULATION_SERVICE_MAJOR,
                                                  PRAENORA_SIMULATION_SERVICE_MINOR}},
                std::nullopt, nullptr);
            table_ = static_cast<const praenora_simulation_v0*>(provider.service(
                PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR));
        } catch (const praenora::Error& error) {
            FAIL() << "the roadrunner provider does not load: " << error.what() << " "
                   << error.details_json();
        }
        ASSERT_NE(table_, nullptr);
    }

    void TearDown() override {
        for (praenora_simulation_session* session : sessions_) {
            table_->close_session(session);
        }
    }

    const praenora_simulation_v0* table() const { return table_; }

    // Opens `model`; the session is closed when the test ends.
    Call open(std::string_view model, praenora_simulation_session** out = nullptr) {
        TextSink error;
        praenora_simulation_session* session = nullptr;
        Call call;
        call.status = table_->open_session(table_->context, nullptr, bytes(model),
                                           bytes(R"({"format":"sbml"})"), &session, error.get());
        call.error = error.text();
        if (session != nullptr) {
            sessions_.push_back(session);
        }
        if (out != nullptr) {
            *out = session;
        }
        return call;
    }

    // A session of `model`, which must open.
    praenora_simulation_session* session_of(std::string_view model) {
        praenora_simulation_session* session = nullptr;
        const Call call = open(model, &session);
        EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
        return session;
    }

    Call simulate(praenora_simulation_session* session, const json& request,
                  const praenora_host_api* host = nullptr) {
        TextSink result, error;
        const std::string text = request.dump();
        Call call;
        call.status = table_->simulate(session, host, bytes(text), result.get(), error.get());
        call.output = result.text();
        call.error = error.text();
        return call;
    }

    // The result document of a request that must succeed.
    json simulated(praenora_simulation_session* session, const json& request) {
        const Call call = simulate(session, request);
        EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
        return json::parse(call.output, nullptr, false);
    }

    json described(praenora_simulation_session* session) {
        TextSink out, error;
        EXPECT_EQ(table_->describe_model(session, nullptr, out.get(), error.get()),
                  PRAENORA_STATUS_OK)
            << error.text();
        return json::parse(out.text(), nullptr, false);
    }

private:
    const praenora_simulation_v0* table_ = nullptr;
    std::vector<praenora_simulation_session*> sessions_;
};

// A request from 0 to `end` at the given times, with tight CVODE settings.
json request(double end, std::vector<double> times, std::vector<std::string> quantities = {}) {
    return {{"time_domain", {{"start", 0.0}, {"end", end}}},
            {"solver",
             {{"method", "cvode"},
              {"stiffness", "nonstiff"},
              {"relative_tolerance", 1e-12},
              {"absolute_tolerance", 1e-12}}},
            {"output",
             {{"save_at", "explicit_times"},
              {"times", std::move(times)},
              {"quantities", std::move(quantities)},
              {"include_states", true}}}};
}

// The value of `column` in the last row of a result.
double last_value(const json& result, const std::string& column) {
    const json& columns = result.at("columns");
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns.at(i) == column) {
            return result.at("rows").back().at(i).get<double>();
        }
    }
    ADD_FAILURE() << "no column " << column << " in " << result;
    return std::nan("");
}

bool near(double actual, double expected, double relative) {
    return std::fabs(actual - expected) <= relative * std::fabs(expected);
}

// Expects a model text refused with DATA_INVALID and details.reason "model" by the provider's own
// checks, before libRoadRunner saw it (no details.engine_error).
void expect_refused_text(const Call& call, const std::string& what) {
    EXPECT_EQ(call.status, PRAENORA_STATUS_DATA_INVALID) << what << ": " << call.error;
    const json error = json::parse(call.error, nullptr, false);
    ASSERT_TRUE(error.is_object()) << what << ": " << call.error;
    EXPECT_EQ(error.at("details").value("reason", ""), "model") << what << ": " << call.error;
    EXPECT_EQ(error.at("details").value("service", ""), PRAENORA_SIMULATION_SERVICE);
    EXPECT_FALSE(error.at("details").contains("engine_error"))
        << what << ": libRoadRunner saw the text: " << call.error;
}

} // namespace

TEST_F(RoadrunnerProvider, DescribesTheEngineItPins) {
    TextSink out;
    ASSERT_EQ(table()->describe(table()->context, out.get()), PRAENORA_STATUS_OK);
    const json described = json::parse(out.text());
    const json& engine = described.at("engine");
    EXPECT_EQ(engine.at("name"), "libroadrunner");
    EXPECT_EQ(engine.at("jit"), "mcjit");
    EXPECT_EQ(engine.at("llvm_optimization"), "none");
    for (const char* version : {"version", "llvm", "sundials", "libsbml"}) {
        EXPECT_TRUE(engine.contains(version) && engine.at(version).is_string()) << engine;
    }
    EXPECT_EQ(described.at("model_formats"), json::array({"sbml"}));
    EXPECT_EQ(described.at("methods"), json::array({"cvode"}));
}

TEST_F(RoadrunnerProvider, RefusesTextsThatAreNotSbmlDocuments) {
    const std::string model = read_model("decay.xml");
    const std::string declaration = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    const std::string body = model.substr(model.find("<sbml"));
    // Paths and URIs are never read: only document text starting with '<' is a model.
    const std::vector<std::string> refused = {
        praenora::test::fixture_path("models/sbml/decay.xml").u8string(),
        "decay.xml",
        "file:///etc/hosts",
        "http://localhost:9/model.xml",
        "<not an sbml document/>",
        "<sbml",
        // Document type declarations, which can expand entities, are refused before any parser
        // sees the text.
        declaration + "<!DOCTYPE sbml [<!ENTITY x \"x\">]>" + body,
        "<!DOCTYPE sbml>" + body,
        declaration + "\n<!doctype sbml>\n" + body,
        // Valid SBML that libRoadRunner's own test (rr::SBMLReader::is_sbml) would not take as
        // document text, so that it would open the text as a URI or, with a '/' before the first
        // ':', as a file path: a trailing comment or processing instruction that holds "?xml?"
        // and a '<' after it.
        body + "<!--?xml?><-->",
        body + "<!-- note? xml ? > <  -->",
        body + "<?pi ?xml?><!---->",
        "<!--/-->" + body + "<!--?xml?><-->",
    };
    for (const std::string& text : refused) {
        expect_refused_text(open(text), text.substr(0, 60));
    }
    // A byte order mark and white space may come first.
    EXPECT_EQ(open("\xEF\xBB\xBF \n\t" + model).status, PRAENORA_STATUS_OK);
}

TEST_F(RoadrunnerProvider, RefusesDocumentsThatRequirePackages) {
    const std::string model = read_model("decay.xml");
    const std::string core = R"(<sbml xmlns="http://www.sbml.org/sbml/level3/version2/core" )"
                             R"(level="3" version="2">)";
    ASSERT_NE(model.find(core), std::string::npos);
    const auto with_root = [&](const std::string& root) {
        std::string text = model;
        return text.replace(text.find(core), core.size(), root);
    };
    // The comp package (which can name other documents by URI or path), and a package that
    // libSBML does not know (spatial), both required.
    const std::string comp =
        R"(<sbml xmlns="http://www.sbml.org/sbml/level3/version2/core" level="3" version="2" )"
        R"(xmlns:comp="http://www.sbml.org/sbml/level3/version1/comp/version1" )"
        R"(comp:required="true">)";
    expect_refused_text(open(with_root(comp)), "comp");
    // comp declared but not required: refused too.
    const std::string comp_optional =
        R"(<sbml xmlns="http://www.sbml.org/sbml/level3/version2/core" level="3" version="2" )"
        R"(xmlns:comp="http://www.sbml.org/sbml/level3/version1/comp/version1" )"
        R"(comp:required="false">)";
    expect_refused_text(open(with_root(comp_optional)), "comp, not required");
    const std::string unknown =
        R"(<sbml xmlns="http://www.sbml.org/sbml/level3/version2/core" level="3" version="2" )"
        R"(xmlns:spatial="http://www.sbml.org/sbml/level3/version1/spatial/version1" )"
        R"(spatial:required="true">)";
    expect_refused_text(open(with_root(unknown)), "spatial");
    // A package that the document does not require is ignored, as SBML allows.
    const std::string optional =
        R"(<sbml xmlns="http://www.sbml.org/sbml/level3/version2/core" level="3" version="2" )"
        R"(xmlns:layout="http://www.sbml.org/sbml/level3/version1/layout/version1" )"
        R"(layout:required="false">)";
    const Call accepted = open(with_root(optional));
    EXPECT_EQ(accepted.status, PRAENORA_STATUS_OK) << accepted.error;
    // A document that is not valid SBML.
    std::string invalid = model;
    invalid.replace(invalid.find("<listOfSpecies>"), 0, "<bogus/>");
    const Call refused = open(invalid);
    EXPECT_EQ(refused.status, PRAENORA_STATUS_DATA_INVALID) << refused.error;
}

TEST_F(RoadrunnerProvider, DescribesStatesParametersAndQuantities) {
    praenora_simulation_session* session = session_of(read_model("rules.xml"));
    ASSERT_NE(session, nullptr);
    const json model = described(session);
    // States: the variables of rate rules, then the floating species that no rule defines.
    EXPECT_EQ(model.at("states"), json::array({"z", "S"})) << model;
    // Parameters: global parameters that no rule and no initial assignment defines.
    EXPECT_EQ(model.at("parameters"), json::array({"k", "base"})) << model;
    // Quantities: the states, the other floating species (amounts), the concentrations of the
    // floating species, the reaction rates and the parameters of assignment rules.
    EXPECT_EQ(model.at("quantities"),
              json::array({"z", "S", "P", "[S]", "[P]", "r", "v"}))
        << model;
    EXPECT_EQ(model.at("events"), false);

    // The quantities' values, and k2's initial assignment evaluated with the value of k.
    for (const double k : {0.3, 0.6}) {
        json r = request(2.0, {0.0, 1.0, 2.0}, {"P", "[S]", "[P]", "r", "v"});
        r["parameter_values"] = {{"k", k}};
        const json result = simulated(session, r);
        ASSERT_TRUE(result.value("ok", false)) << result;
        const double s = 2.0 * std::exp(-k * 2.0);
        EXPECT_TRUE(near(last_value(result, "S"), s, 1e-9)) << result;
        EXPECT_TRUE(near(last_value(result, "z"), k * 2.0, 1e-9)) << result;
        EXPECT_TRUE(near(last_value(result, "P"), s, 1e-9)) << result;
        EXPECT_TRUE(near(last_value(result, "[S]"), s / 2.0, 1e-9)) << result;
        EXPECT_TRUE(near(last_value(result, "[P]"), s / 2.0, 1e-9)) << result;
        EXPECT_TRUE(near(last_value(result, "r"), k * s, 1e-9)) << result;
        EXPECT_TRUE(near(last_value(result, "v"), 2.0 * k * s, 1e-9)) << result;
    }
    // What is not a parameter cannot be set.
    for (const char* derived : {"k2", "v", "z"}) {
        json r = request(2.0, {2.0});
        r["parameter_values"] = {{derived, 1.0}};
        const Call call = simulate(session, r);
        EXPECT_EQ(call.status, PRAENORA_STATUS_INVALID_ARGUMENT) << derived << ": " << call.error;
    }
}

TEST_F(RoadrunnerProvider, ParametersOfStoichiometriesAreNotParameters) {
    // libRoadRunner keeps a stoichiometry that an initial assignment sets at its first value, so
    // the parameters that assignment uses, directly or through another, are not settable.
    praenora_simulation_session* session = session_of(read_model("stoichiometry.xml"));
    ASSERT_NE(session, nullptr);
    const json model = described(session);
    EXPECT_EQ(model.at("parameters"), json::array({"k"})) << model;
    // P gains two for each S that turns into it: P = 2 * (10 - S).
    const json result = simulated(session, request(2.0, {2.0}));
    ASSERT_TRUE(result.value("ok", false)) << result;
    const double s = 10.0 * std::exp(-1.0);
    EXPECT_TRUE(near(last_value(result, "S"), s, 1e-9)) << result;
    EXPECT_TRUE(near(last_value(result, "P"), 2.0 * (10.0 - s), 1e-9)) << result;
}

TEST_F(RoadrunnerProvider, AcceptsSbmlLevel2) {
    praenora_simulation_session* session = session_of(read_model("decay-l2v4.xml"));
    ASSERT_NE(session, nullptr);
    const json model = described(session);
    EXPECT_EQ(model.at("states"), json::array({"x", "y"})) << model;
    EXPECT_EQ(model.at("parameters"), json::array({"k_x", "k_y"})) << model;
    // The parameter takes effect; the result agrees with the exact solution.
    json r = request(2.0, {1.0, 2.0}, {"[x]"});
    r["parameter_values"] = {{"k_x", 0.8}};
    const json result = simulated(session, r);
    ASSERT_TRUE(result.value("ok", false)) << result;
    EXPECT_TRUE(near(last_value(result, "x"), 10.0 * std::exp(-1.6), 1e-9)) << result;
    EXPECT_TRUE(near(last_value(result, "[x]"), 5.0 * std::exp(-1.6), 1e-9)) << result;
    EXPECT_TRUE(near(last_value(result, "y"), 4.0 * std::exp(-3.0), 1e-9)) << result;
}

TEST_F(RoadrunnerProvider, AcceptsSbmlLevel3Version1) {
    praenora_simulation_session* session = session_of(read_model("decay-l3v1.xml"));
    ASSERT_NE(session, nullptr);
    const json model = described(session);
    EXPECT_EQ(model.at("states"), json::array({"x", "y"})) << model;
    EXPECT_EQ(model.at("parameters"), json::array({"k_x", "k_y"})) << model;
    // The parameter takes effect; the result agrees with the exact solution.
    json r = request(2.0, {1.0, 2.0}, {"[x]"});
    r["parameter_values"] = {{"k_x", 0.8}};
    const json result = simulated(session, r);
    ASSERT_TRUE(result.value("ok", false)) << result;
    EXPECT_TRUE(near(last_value(result, "x"), 10.0 * std::exp(-1.6), 1e-9)) << result;
    EXPECT_TRUE(near(last_value(result, "[x]"), 5.0 * std::exp(-1.6), 1e-9)) << result;
    EXPECT_TRUE(near(last_value(result, "y"), 4.0 * std::exp(-3.0), 1e-9)) << result;
}

TEST_F(RoadrunnerProvider, AcceptsSbmlLevel1) {
    // Level 1 identifies elements by name; libRoadRunner reads them as ids.
    praenora_simulation_session* session = session_of(read_model("decay-l1v2.xml"));
    ASSERT_NE(session, nullptr);
    const json model = described(session);
    EXPECT_EQ(model.at("states"), json::array({"x"})) << model;
    EXPECT_EQ(model.at("parameters"), json::array({"k_x"})) << model;
    EXPECT_TRUE(near(last_value(simulated(session, request(2.0, {1.0, 2.0})), "x"),
                     10.0 * std::exp(-1.0), 1e-9));
    json r = request(2.0, {2.0});
    r["parameter_values"] = {{"k_x", 0.8}};
    EXPECT_TRUE(near(last_value(simulated(session, r), "x"), 10.0 * std::exp(-1.6), 1e-9));
}

TEST_F(RoadrunnerProvider, ParametersOfStoichiometryMathAreNotParameters) {
    // SBML Level 2: libRoadRunner computes a stoichiometryMath of constant parameters once, at
    // load, so those parameters are not settable.
    praenora_simulation_session* session = session_of(read_model("stoichiometry-math-l2.xml"));
    ASSERT_NE(session, nullptr);
    const json model = described(session);
    EXPECT_EQ(model.at("parameters"), json::array({"k"})) << model;
    const json result = simulated(session, request(2.0, {2.0}));
    ASSERT_TRUE(result.value("ok", false)) << result;
    const double s = 10.0 * std::exp(-1.0);
    EXPECT_TRUE(near(last_value(result, "S"), s, 1e-9)) << result;
    EXPECT_TRUE(near(last_value(result, "P"), 2.0 * (10.0 - s), 1e-9)) << result;
}

TEST_F(RoadrunnerProvider, RefusesModelsThatCannotBeSimulated) {
    // A flux-balance model (fbc, not required) without kinetic laws: libRoadRunner can analyze it
    // but not simulate it, so the session is refused rather than every run failing.
    const Call call = open(read_model("flux-balance.xml"));
    EXPECT_EQ(call.status, PRAENORA_STATUS_DATA_INVALID) << call.error;
    const json error = json::parse(call.error, nullptr, false);
    ASSERT_TRUE(error.is_object()) << call.error;
    EXPECT_EQ(error.at("details").value("reason", ""), "model") << call.error;
}

TEST_F(RoadrunnerProvider, AppliesTheModelsEvents) {
    praenora_simulation_session* session = session_of(read_model("decay-with-event.xml"));
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(described(session).at("events"), true);
    // At time 1, x is set back to 10.
    const json result = simulated(session, request(1.5, {0.5, 1.5}));
    ASSERT_TRUE(result.value("ok", false)) << result;
    EXPECT_TRUE(near(last_value(result, "x"), 10.0 * std::exp(-0.25), 1e-8)) << result;
    EXPECT_TRUE(near(last_value(result, "y"), 4.0 * std::exp(-2.25), 1e-8)) << result;
}

TEST_F(RoadrunnerProvider, DelayedEventsDoNotCarryOverToLaterRuns) {
    const std::string model = read_model("delayed-event.xml");
    praenora_simulation_session* session = session_of(model);
    praenora_simulation_session* fresh = session_of(model);
    ASSERT_TRUE(session != nullptr && fresh != nullptr);
    // A run that ends at 2 leaves the event's assignment (due at 2.4) pending.
    ASSERT_EQ(simulate(session, request(2.0, {2.0})).status, PRAENORA_STATUS_OK);
    // A later run applies it once, as a run in a new session does.
    const Call later = simulate(session, request(3.0, {1.0, 3.0}));
    ASSERT_EQ(later.status, PRAENORA_STATUS_OK) << later.error;
    EXPECT_EQ(later.output, simulate(fresh, request(3.0, {1.0, 3.0})).output);
    const json result = json::parse(later.output);
    const double expected = (10.0 * std::exp(-1.2) + 5.0) * std::exp(-0.3);
    EXPECT_TRUE(near(last_value(result, "x"), expected, 1e-8)) << result;
}

TEST_F(RoadrunnerProvider, DelayedEventsTriggeredAtTheStartAreAppliedOnce) {
    const std::string model = read_model("delayed-event-at-start.xml");
    praenora_simulation_session* session = session_of(model);
    ASSERT_NE(session, nullptr);
    // The event fires at the start; its assignment, half a time unit later, adds 5 to x once.
    const json r = request(1.0, {0.25, 1.0});
    const double expected = (10.0 * std::exp(-0.25) + 5.0) * std::exp(-0.25);
    for (int run = 0; run < 3; ++run) {
        const json result = simulated(session, r);
        ASSERT_TRUE(result.value("ok", false)) << result;
        EXPECT_TRUE(near(last_value(result, "x"), expected, 1e-8)) << "run " << run << ": "
                                                                    << result;
    }
    // A plan's runs, and a later start time, apply it once too.
    TextSink info, error;
    praenora_simulation_plan* plan = nullptr;
    const std::string plan_json = request(1.0, {1.0}).dump();
    ASSERT_EQ(table()->prepare(session, nullptr, bytes(plan_json), &plan, info.get(),
                               error.get()),
              PRAENORA_STATUS_OK)
        << error.text();
    for (int run = 0; run < 2; ++run) {
        double out[2] = {0.0, 0.0};
        std::int32_t outcome = -1;
        TextSink run_error;
        ASSERT_EQ(table()->run(plan, nullptr, nullptr, 0, out, 2, &outcome, nullptr,
                               run_error.get()),
                  PRAENORA_STATUS_OK)
            << run_error.text();
        EXPECT_TRUE(near(out[1], expected, 1e-8)) << "plan run " << run << ": " << out[1];
    }
    table()->close_plan(plan);
    json later = request(3.0, {2.0, 3.0});
    later["time_domain"]["start"] = 2.0;
    const json moved = simulated(session, later);
    ASSERT_TRUE(moved.value("ok", false)) << moved;
    EXPECT_TRUE(near(last_value(moved, "x"), (10.0 * std::exp(-0.25) + 5.0) * std::exp(-0.25),
                     1e-8))
        << moved;
}

TEST_F(RoadrunnerProvider, SimultaneousEventsAreOrderedTheSameWayInEveryRun) {
    // Five events of the same priority set x at time 1; libRoadRunner draws their order from the
    // model's random number generator, which every run seeds the same way.
    const std::string model = read_model("simultaneous-events.xml");
    praenora_simulation_session* session = session_of(model);
    ASSERT_NE(session, nullptr);
    const json r = request(2.0, {1.5, 2.0});
    const std::string expected = simulate(session_of(model), r).output;
    const json result = json::parse(expected);
    ASSERT_TRUE(result.value("ok", false)) << result;
    const double kept = result.at("rows").at(0).at(1).get<double>() / std::exp(-0.25);
    EXPECT_TRUE(std::fabs(kept - std::round(kept)) < 1e-6 && kept >= 1.0 && kept <= 5.0)
        << "x after the events: " << kept;
    for (int run = 0; run < 5; ++run) {
        EXPECT_EQ(simulate(session, r).output, expected) << "run " << run;
    }
}

TEST_F(RoadrunnerProvider, SolverStepsRunsLeaveNoTrace) {
    // A run of solver steps, also one that ends at the row limit, changes nothing for the next
    // run of the session.
    const std::string model = read_model("decay-with-event.xml");
    const json later = request(2.0, {0.5, 1.5, 2.0});
    const std::string expected = simulate(session_of(model), later).output;
    praenora_simulation_session* session = session_of(model);
    ASSERT_NE(session, nullptr);
    json steps = request(2.0, {});
    steps["output"] = {{"save_at", "solver_steps"}};
    ASSERT_TRUE(simulated(session, steps).value("ok", false));
    EXPECT_EQ(simulate(session, later).output, expected);
    // Steps no longer than 2e-6 make more rows than a run may have (2^22 values in 9 columns:
    // time, the two states and the six quantities).
    steps["output"]["quantities"] = {"x", "y", "[x]", "[y]", "rx", "ry"};
    steps["solver"]["max_step"] = 2e-6;
    steps["solver"]["max_steps"] = 100000000;
    const Call exhausted = simulate(session, steps);
    EXPECT_EQ(exhausted.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED) << exhausted.error;
    EXPECT_EQ(simulate(session, later).output, expected);
}

TEST_F(RoadrunnerProvider, SolverSettingsLeaveNoTraceInLaterRuns) {
    const std::string model = read_model("two-compartments.xml");
    praenora_simulation_session* session = session_of(model);
    ASSERT_NE(session, nullptr);
    const json first = request(2.0, {0.5, 1.0, 2.0}, {"[A1]"});
    const std::string expected = simulate(session, first).output;
    json other = first;
    other["solver"] = {{"method", "cvode"},
                       {"stiffness", "stiff"},
                       {"relative_tolerance", 1e-6},
                       {"absolute_tolerance", 1e-8},
                       {"max_steps", 500},
                       {"max_step", 0.01},
                       {"min_step", 1e-9}};
    ASSERT_EQ(simulate(session, other).status, PRAENORA_STATUS_OK);
    EXPECT_EQ(simulate(session, first).output, expected);
    EXPECT_EQ(simulate(session_of(model), first).output, expected);
}

TEST_F(RoadrunnerProvider, ReportsTheSolverSettingsItUsed) {
    praenora_simulation_session* session = session_of(read_model("decay.xml"));
    ASSERT_NE(session, nullptr);
    json plain = request(2.0, {1.0, 2.0});
    plain.erase("solver");
    // CVODE's defaults, under the request's names.
    EXPECT_EQ(simulated(session, plain).at("solver"),
              json({{"method", "cvode"},
                    {"stiffness", "stiff"},
                    {"relative_tolerance", 1e-6},
                    {"absolute_tolerance", 1e-12},
                    {"max_steps", 20000}}));
    json all = plain;
    all["solver"] = {{"method", "cvode"},     {"stiffness", "nonstiff"},
                     {"relative_tolerance", 1e-9}, {"absolute_tolerance", 1e-10},
                     {"max_steps", 1000},     {"max_step", 0.5},
                     {"min_step", 0.0}};
    EXPECT_EQ(simulated(session, all).at("solver"), all.at("solver"));

    // Values CVODE cannot honor, and settings it does not map.
    const auto refused = [&](const json& solver, praenora_status status, const std::string& at) {
        json r = plain;
        r["solver"] = solver;
        const Call call = simulate(session, r);
        EXPECT_EQ(call.status, status) << solver << ": " << call.error;
        EXPECT_EQ(json::parse(call.error).at("details").value("pointer", ""), at) << call.error;
    };
    refused({{"method", "cvode"}, {"stiffness", "auto"}}, PRAENORA_STATUS_UNSUPPORTED,
            "/solver/stiffness");
    refused({{"method", "cvode"}, {"seed", 7}}, PRAENORA_STATUS_UNSUPPORTED, "/solver/seed");
    refused({{"method", "cvode"}, {"max_steps", 0}}, PRAENORA_STATUS_INVALID_ARGUMENT,
            "/solver/max_steps");
    refused({{"method", "cvode"}, {"max_steps", 1.5}}, PRAENORA_STATUS_INVALID_ARGUMENT,
            "/solver/max_steps");
    refused({{"method", "cvode"}, {"relative_tolerance", -1}}, PRAENORA_STATUS_INVALID_ARGUMENT,
            "/solver/relative_tolerance");
    refused({{"method", "cvode"}, {"max_step", 0}}, PRAENORA_STATUS_INVALID_ARGUMENT,
            "/solver/max_step");
    refused({{"method", "cvode"}, {"min_step", -1}}, PRAENORA_STATUS_INVALID_ARGUMENT,
            "/solver/min_step");
}

TEST_F(RoadrunnerProvider, ReportsTheStepLimit) {
    praenora_simulation_session* session = session_of(read_model("decay.xml"));
    ASSERT_NE(session, nullptr);
    json r = request(2.0, {2.0});
    r["solver"]["max_steps"] = 1;
    const json result = simulated(session, r);
    ASSERT_EQ(result.value("ok", true), false) << result;
    EXPECT_EQ(result.at("error").at("details").value("reason", ""), "step_limit") << result;
    EXPECT_TRUE(result.at("error").at("details").contains("engine_error")) << result;
}

TEST_F(RoadrunnerProvider, SimulatesTheSolverSteps) {
    praenora_simulation_session* session = session_of(read_model("decay.xml"));
    ASSERT_NE(session, nullptr);
    json r = request(2.0, {});
    r["output"] = {{"save_at", "solver_steps"}, {"quantities", {"[x]"}}};
    const json result = simulated(session, r);
    ASSERT_TRUE(result.value("ok", false)) << result;
    EXPECT_EQ(result.at("columns"), json::array({"time", "x", "y", "[x]"}));
    const json& rows = result.at("rows");
    ASSERT_GT(rows.size(), 2u) << result;
    EXPECT_EQ(rows.front().at(0).get<double>(), 0.0);
    EXPECT_EQ(rows.back().at(0).get<double>(), 2.0);
    double previous = -1.0;
    for (const json& row : rows) {
        const double t = row.at(0).get<double>();
        EXPECT_GT(t, previous) << row;
        previous = t;
        EXPECT_TRUE(near(row.at(1).get<double>(), 10.0 * std::exp(-0.5 * t), 1e-9)) << row;
        EXPECT_TRUE(near(row.at(3).get<double>(), 5.0 * std::exp(-0.5 * t), 1e-9)) << row;
    }
}

TEST_F(RoadrunnerProvider, CancelsWithinARun) {
    praenora_simulation_session* session = session_of(read_model("decay.xml"));
    ASSERT_NE(session, nullptr);
    std::vector<double> times;
    for (int i = 1; i <= 100; ++i) {
        times.push_back(0.02 * i);
    }
    const json r = request(2.0, times);
    const std::string expected = simulate(session, r).output;
    // The first poll comes before the run; the next ones during it, between output times.
    const CancellingHost host(3);
    const Call cancelled = simulate(session, r, host.api());
    EXPECT_EQ(cancelled.status, PRAENORA_STATUS_CANCELLED) << cancelled.error;
    EXPECT_EQ(host.polls(), 3);
    // The session is not left in the middle of a run.
    EXPECT_EQ(simulate(session, r).output, expected);
}
