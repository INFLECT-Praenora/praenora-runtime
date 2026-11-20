// The roadrunner provider's engine code (src/providers/roadrunner/engine.cpp, compiled into this
// test with the provider's definitions) and the shared compiled models: a model loaded from its
// saved state, without compiling, is the model compiled from its text, field by field (its ids,
// the engine's index of each parameter, how each quantity is read, whether it has events); and an
// entry that does not load says why, from its envelope's checks (not an entry of the provider,
// another engine's, cut short, corrupt) or from libRoadRunner's loadStateS(). Built and run only
// with the roadrunner provider (PRAENORA_WITH_ROADRUNNER).
#include "engine.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using praenora::roadrunner::CompiledModel;
using praenora::roadrunner::json;
using praenora::roadrunner::RunSettings;
using praenora::roadrunner::RunValues;
using praenora::roadrunner::Simulator;

namespace {

std::string read_text(const std::string& relative) {
    std::ifstream in(praenora::test::fixture_path(relative), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Whether a store callback of the host below was called.
std::atomic<bool> g_store_called{false};

std::int32_t PRAENORA_CALL store_load(void*, const char*, std::size_t, praenora_sink*) {
    g_store_called = true;
    return 0;
}

void PRAENORA_CALL store_save(void*, const char*, std::size_t, const char*, std::size_t) {
    g_store_called = true;
}

// Starts the engine once in this process, with a provider host that logs nothing and is as short
// as a runtime's before the store's callbacks were appended: its struct_size ends before
// load_compiled, whose fields hold callbacks that the provider must therefore never call.
void start_engine_once() {
    static const bool started = [] {
        static praenora_provider_host host{};
        host.struct_size =
            static_cast<std::uint32_t>(offsetof(praenora_provider_host, load_compiled));
        host.load_compiled = &store_load;
        host.save_compiled = &store_save;
        praenora::roadrunner::start_engine(&host);
        return true;
    }();
    static_cast<void>(started);
}

// The state an entry holds, with libRoadRunner's number of the saved object in its process (an int
// after its magic number and data version, saveStateS() of libRoadRunner 2.11) set to 0: what does
// not change from one process, or one object, to another.
std::string state_without_instance_number(const std::string& entry) {
    std::string why;
    const std::optional<std::string_view> state =
        praenora::roadrunner::envelope_payload(entry, why);
    EXPECT_TRUE(state.has_value()) << why;
    std::string bytes(state.value_or(std::string_view()));
    if (bytes.size() >= 12) {
        bytes.replace(8, 4, 4, '\0');
    }
    return bytes;
}

void expect_same_model(const CompiledModel& loaded, const CompiledModel& compiled) {
    EXPECT_EQ(loaded.ids().states, compiled.ids().states);
    EXPECT_EQ(loaded.ids().parameters, compiled.ids().parameters);
    EXPECT_EQ(loaded.ids().quantities, compiled.ids().quantities);
    EXPECT_EQ(loaded.parameter_indices(), compiled.parameter_indices());
    ASSERT_EQ(loaded.quantities().size(), compiled.quantities().size());
    for (std::size_t i = 0; i < compiled.quantities().size(); ++i) {
        SCOPED_TRACE(compiled.quantities()[i].selection);
        EXPECT_EQ(loaded.quantities()[i].kind, compiled.quantities()[i].kind);
        EXPECT_EQ(loaded.quantities()[i].index, compiled.quantities()[i].index);
        EXPECT_EQ(loaded.quantities()[i].selection, compiled.quantities()[i].selection);
    }
    EXPECT_EQ(loaded.events(), compiled.events());
    EXPECT_EQ(loaded.description(), compiled.description());
}

} // namespace

// Models with states and parameters, assignment rules, initial assignments and stoichiometries
// (whose inputs are no parameters), and events.
TEST(RoadrunnerEngine, AModelLoadedFromItsSavedStateIsTheModelCompiledFromItsText) {
    start_engine_once();
    for (const char* name :
         {"decay.xml", "rules.xml", "stoichiometry.xml", "stoichiometry-math-l2.xml",
          "decay-with-event.xml", "two-compartments-with-event.xml", "delayed-event.xml"}) {
        SCOPED_TRACE(name);
        const std::string text = read_text(std::string("models/sbml/") + name);
        const std::shared_ptr<const CompiledModel> compiled =
            praenora::roadrunner::compile_model(text, nullptr);
        ASSERT_NE(compiled, nullptr);
        const std::string entry = compiled->saved_state(nullptr);
        std::string why;
        const std::shared_ptr<const CompiledModel> loaded =
            praenora::roadrunner::load_compiled_model(text, entry, nullptr, why);
        ASSERT_NE(loaded, nullptr) << why;
        expect_same_model(*loaded, *compiled);
        // The loaded model saves the same state again, byte for byte, but for libRoadRunner's
        // number of the object in its process, which loadStateS() reads and ignores.
        const std::string again = loaded->saved_state(nullptr);
        EXPECT_EQ(state_without_instance_number(again), state_without_instance_number(entry));
    }
}

// Simulated after a load from its saved state, a model gives the doubles of the model compiled
// from its text, bit for bit: in a first run and in a second one of the same simulator (events,
// delayed ones and those at the start included, whose queue a run may leave behind).
TEST(RoadrunnerEngine, AModelLoadedFromItsSavedStateSimulatesAsTheCompiledOne) {
    start_engine_once();
    const json request = {
        {"time_domain", {{"start", 0}, {"end", 10}}},
        {"output", {{"save_at", "explicit_times"}, {"times", {0, 0.5, 1, 2, 3, 5, 8, 10}}}},
        {"solver",
         {{"method", "cvode"},
          {"stiffness", "nonstiff"},
          {"relative_tolerance", 1e-10},
          {"absolute_tolerance", 1e-12}}}};
    for (const char* name :
         {"decay.xml", "rules.xml", "stoichiometry.xml", "stoichiometry-math-l2.xml",
          "decay-with-event.xml", "two-compartments-with-event.xml", "delayed-event.xml",
          "delayed-event-at-start.xml", "simultaneous-events.xml"}) {
        SCOPED_TRACE(name);
        const std::string text = read_text(std::string("models/sbml/") + name);
        const auto compiled = praenora::roadrunner::compile_model(text, nullptr);
        std::string why;
        const auto loaded = praenora::roadrunner::load_compiled_model(
            text, compiled->saved_state(nullptr), nullptr, why);
        ASSERT_NE(loaded, nullptr) << why;
        const RunSettings settings =
            praenora::roadrunner::read_settings(request, compiled->ids(), false);
        Simulator from_compile(compiled, nullptr);
        Simulator from_load(loaded, nullptr);
        for (int run = 0; run < 2; ++run) {
            SCOPED_TRACE(run);
            std::vector<double> expected(settings.times.size() * settings.columns.size());
            std::vector<double> actual(expected.size());
            ASSERT_TRUE(
                from_compile.run(settings, RunValues{}, expected.data(), nullptr).is_null());
            ASSERT_TRUE(from_load.run(settings, RunValues{}, actual.data(), nullptr).is_null());
            EXPECT_EQ(std::memcmp(actual.data(), expected.data(), expected.size() * sizeof(double)),
                      0);
        }
    }
}

// A provider host too short for the store's callbacks offers no store: a model compiled through
// the cache is compiled, and neither callback is called.
TEST(RoadrunnerEngine, AHostTooShortForTheStoreOffersNone) {
    start_engine_once();
    std::string text = read_text("models/sbml/decay.xml");
    static std::atomic<unsigned> next{0};
    text.insert(text.find('\n') + 1, "<!-- short host " + std::to_string(next++) + " -->\n");
    ASSERT_NE(praenora::roadrunner::compiled_model(text, nullptr), nullptr);
    EXPECT_FALSE(g_store_called.load());
}

TEST(RoadrunnerEngine, AnEnvelopeHoldsItsStateWhole) {
    for (const std::string& state : {std::string(), std::string("a state\0with a NUL", 18)}) {
        const std::string entry = praenora::roadrunner::state_envelope(state);
        std::string why;
        const std::optional<std::string_view> payload =
            praenora::roadrunner::envelope_payload(entry, why);
        ASSERT_TRUE(payload.has_value()) << why;
        EXPECT_EQ(*payload, state);
    }
}

// What does not load: garbage, an entry cut short, a state whose bytes changed under a valid
// envelope, the envelope of another engine's identity, and a valid envelope around bytes that are
// not a saved state (loadStateS() throws).
TEST(RoadrunnerEngine, AnEntryThatDoesNotLoadSaysWhy) {
    start_engine_once();
    const std::string text = read_text("models/sbml/decay.xml");
    const std::string entry =
        praenora::roadrunner::compile_model(text, nullptr)->saved_state(nullptr);
    std::string corrupt = entry;
    corrupt.back() = static_cast<char>(corrupt.back() ^ 0x5A);
    std::string other_engine = entry;
    other_engine[std::string("praenora-roadrunner-state\n").size() + 8] ^= 0x01;
    const struct {
        const char* name;
        std::string entry;
        const char* why;
    } cases[] = {
        {"garbage", "garbage", "not a saved state of this provider"},
        {"cut short", entry.substr(0, entry.size() - 1), "bytes, not the"},
        {"corrupt", corrupt, "SHA-256"},
        {"another engine", other_engine, "another engine"},
        {"not a state", praenora::roadrunner::state_envelope("not a state"),
         "libRoadRunner could not load it"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        std::string why;
        EXPECT_EQ(praenora::roadrunner::load_compiled_model(text, c.entry, nullptr, why), nullptr);
        EXPECT_NE(why.find(c.why), std::string::npos) << why;
    }
}
