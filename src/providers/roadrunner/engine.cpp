#include "engine.hpp"

#include <rr/Integrator.h>
#include <rr/rrConfig.h>
#include <rr/rrExecutableModel.h>
#include <rr/rrLogger.h>
#include <rr/rrRoadRunner.h>
#include <rr/rrRoadRunnerOptions.h>
#include <rr/rrSBMLReader.h>
#include <rr/rrSelectionRecord.h>
#include <rr/rrVersionInfo.h>
#include <rr-libstruct/lsMatrix.h>

#include <sbml/InitialAssignment.h>
#include <sbml/Model.h>
#include <sbml/Reaction.h>
#include <sbml/Rule.h>
#include <sbml/SBMLDocument.h>
#include <sbml/SBMLError.h>
#include <sbml/SBMLReader.h>
#include <sbml/SpeciesReference.h>
#include <sbml/StoichiometryMath.h>
#include <sbml/extension/SBMLDocumentPlugin.h>
#include <sbml/math/ASTNode.h>
#include <sbml/xml/XMLNamespaces.h>

#include <Poco/DigestEngine.h>
#include <Poco/SHA2Engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <ostream>
#include <set>
#include <sstream>
#include <streambuf>
#include <utility>

static_assert(RR_VERSION_MAJOR == PRAENORA_ROADRUNNER_VERSION_MAJOR &&
                  RR_VERSION_MINOR == PRAENORA_ROADRUNNER_VERSION_MINOR &&
                  RR_VERSION_PATCH == PRAENORA_ROADRUNNER_VERSION_PATCH,
              "the libRoadRunner headers are not those of the version the manifest names");

namespace praenora::roadrunner {

namespace {

// The seed of the random number generator of each model, set again before every run: SBML leaves
// the order of simultaneous events of equal priority open, and libRoadRunner draws it.
constexpr std::int64_t kRandomSeed = 0;

// How long a call waits for the engine, or for another call's compilation, between two polls of
// cancel_requested().
constexpr std::chrono::milliseconds kPollInterval(20);

// The compiled models the cache keeps, least recently used first out.
constexpr std::size_t kCacheCapacity = 16;

// The first bytes of an entry of the shared compiled models that this provider made.
constexpr char kStateMagic[] = "praenora-roadrunner-state\n";
constexpr std::size_t kStateMagicSize = sizeof kStateMagic - 1;

// The provider's host, from start(): its store callbacks reach the shared compiled models.
const praenora_provider_host* g_provider_host = nullptr;

int reset_everything() { return static_cast<int>(rr::SelectionRecord::ALL); }

// Resets the model to its initial values, evaluating its initial assignments with the current
// initial values, without restoring the initial values that SBML gives.
int reset_to_initial_values() {
    return static_cast<int>(rr::SelectionRecord::ALL & ~rr::SelectionRecord::SBML_INITIALIZE);
}

// ---------------------------------------------------------------------------------------------
// The engine lock: libRoadRunner objects are made, copied and destroyed one at a time.

std::timed_mutex& engine_mutex() {
    static auto* const mutex = new std::timed_mutex(); // never destroyed: used until exit
    return *mutex;
}

class EngineLock {
public:
    // Waits for the engine, polling the host for cancellation.
    explicit EngineLock(const praenora_host_api* host) {
        while (!engine_mutex().try_lock_for(kPollInterval)) {
            check_cancelled(host);
        }
    }
    // Waits for the engine.
    EngineLock() { engine_mutex().lock(); }
    EngineLock(const EngineLock&) = delete;
    EngineLock& operator=(const EngineLock&) = delete;
    ~EngineLock() { engine_mutex().unlock(); }
};

// ---------------------------------------------------------------------------------------------
// Engine logging: libRoadRunner's process-wide logger writes formatted lines ("<Priority>: <text>")
// to a stream whose buffer passes each to the provider's logger.

class LogForwarder final : public std::streambuf {
public:
    explicit LogForwarder(const praenora_provider_host* host) : host_(host) {}

protected:
    // A failure (out of memory) drops the line being written but never fails the stream: a stream
    // that failed once would stay failed, and libRoadRunner's later records would be lost.
    int_type overflow(int_type ch) override {
        try {
            if (!traits_type::eq_int_type(ch, traits_type::eof())) {
                line_.push_back(traits_type::to_char_type(ch));
            }
        } catch (...) {
            line_.clear();
        }
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char* text, std::streamsize size) override {
        try {
            line_.append(text, static_cast<std::size_t>(size));
        } catch (...) {
            line_.clear();
        }
        return size;
    }

    // Poco's console channel ends each message with std::endl, which flushes.
    int sync() override {
        try {
            forward();
        } catch (...) {
            line_.clear();
        }
        return 0;
    }

private:
    void forward() {
        std::string text;
        text.reserve(line_.size());
        // Without the terminal colour sequences (ESC [ ... m) of Poco's colour console channel.
        for (std::size_t i = 0; i < line_.size(); ++i) {
            if (line_[i] == '\x1b' && i + 1 < line_.size() && line_[i + 1] == '[') {
                const std::size_t end = line_.find('m', i);
                if (end != std::string::npos) {
                    i = end;
                    continue;
                }
            }
            text.push_back(line_[i]);
        }
        line_.clear();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (text.empty() || host_ == nullptr ||
            host_->struct_size < offsetof(praenora_provider_host, log) + sizeof host_->log ||
            host_->log == nullptr) {
            return;
        }
        // libRoadRunner logs its own errors, such as a CVODE failure that the service reports as
        // a failed run, which is data: one level lower than libRoadRunner's. The engine's records
        // are context; the service's results and error documents are what count.
        std::int32_t level = PRAENORA_LOG_WARN;
        static const std::pair<const char*, std::int32_t> levels[] = {
            {"Fatal: ", PRAENORA_LOG_ERROR},  {"Critical: ", PRAENORA_LOG_ERROR},
            {"Error: ", PRAENORA_LOG_WARN},   {"Warning: ", PRAENORA_LOG_INFO},
            {"Notice: ", PRAENORA_LOG_INFO},  {"Information: ", PRAENORA_LOG_INFO},
            {"Debug: ", PRAENORA_LOG_DEBUG},  {"Trace: ", PRAENORA_LOG_TRACE}};
        for (const auto& [prefix, prefix_level] : levels) {
            const std::string_view start(prefix);
            if (text.compare(0, start.size(), start) == 0) {
                level = prefix_level;
                text.erase(0, start.size());
                break;
            }
        }
        text.insert(0, "libRoadRunner: ");
        host_->log(host_->context, level, text.data(), text.size());
    }

    const praenora_provider_host* host_;
    std::string line_;
};

// Every key of libRoadRunner's process-wide configuration (rr::Config), with the value the provider
// pins: libRoadRunner 2.11.0's built-in defaults, except the random seed. libRoadRunner reads a
// configuration file on its first use (ROADRUNNER_CONFIG, roadrunner.conf in the home directory
// and beside the library); setting every key afterwards makes that file irrelevant.
void pin_configuration() {
    using rr::Config;
    using rr::Setting;
    const std::pair<Config::Keys, Setting> pinned[] = {
        {Config::LOADSBMLOPTIONS_CONSERVED_MOIETIES, Setting(false)},
        {Config::LOADSBMLOPTIONS_RECOMPILE, Setting(false)},
        {Config::LOADSBMLOPTIONS_READ_ONLY, Setting(false)},
        {Config::LOADSBMLOPTIONS_MUTABLE_INITIAL_CONDITIONS, Setting(true)},
        {Config::LOADSBMLOPTIONS_OPTIMIZE_GVN, Setting(false)},
        {Config::LOADSBMLOPTIONS_OPTIMIZE_CFG_SIMPLIFICATION, Setting(false)},
        {Config::LOADSBMLOPTIONS_OPTIMIZE_INSTRUCTION_COMBINING, Setting(false)},
        {Config::LOADSBMLOPTIONS_OPTIMIZE_DEAD_INST_ELIMINATION, Setting(false)},
        {Config::LOADSBMLOPTIONS_OPTIMIZE_DEAD_CODE_ELIMINATION, Setting(false)},
        {Config::LOADSBMLOPTIONS_OPTIMIZE_INSTRUCTION_SIMPLIFIER, Setting(false)},
        {Config::LOADSBMLOPTIONS_USE_MCJIT, Setting(false)},
        {Config::SIMULATEOPTIONS_STEPS, Setting(50)},
        {Config::SIMULATEOPTIONS_DURATION, Setting(5.0)},
        {Config::SIMULATEOPTIONS_ABSOLUTE, Setting(1.e-10)},
        {Config::SIMULATEOPTIONS_RELATIVE, Setting(1.e-5)},
        {Config::SIMULATEOPTIONS_STRUCTURED_RESULT, Setting(false)},
        {Config::SIMULATEOPTIONS_STIFF, Setting(true)},
        {Config::SIMULATEOPTIONS_MULTI_STEP, Setting(false)},
        {Config::SIMULATEOPTIONS_DETERMINISTIC_VARIABLE_STEP, Setting(false)},
        {Config::SIMULATEOPTIONS_STOCHASTIC_VARIABLE_STEP, Setting(true)},
        {Config::SIMULATEOPTIONS_INTEGRATOR, Setting(std::string("CVODE"))},
        {Config::SIMULATEOPTIONS_INITIAL_TIMESTEP, Setting(-1)},
        {Config::SIMULATEOPTIONS_MINIMUM_TIMESTEP, Setting(-1)},
        {Config::SIMULATEOPTIONS_MAXIMUM_TIMESTEP, Setting(-1)},
        {Config::SIMULATEOPTIONS_MAXIMUM_NUM_STEPS, Setting(-1)},
        {Config::ROADRUNNER_DISABLE_WARNINGS, Setting(false)},
        {Config::ROADRUNNER_DISABLE_PYTHON_DYNAMIC_PROPERTIES, Setting(false)},
        // AllChecksON & UnitsCheckOFF
        {Config::SBML_APPLICABLEVALIDATORS, Setting(0x7f & 0xef)},
        {Config::ROADRUNNER_JACOBIAN_STEP_SIZE, Setting(0.00001)},
        {Config::MODEL_RESET,
         Setting(static_cast<int>(rr::SelectionRecord::TIME | rr::SelectionRecord::RATE |
                                  rr::SelectionRecord::FLOATING))},
        {Config::CVODE_MIN_ABSOLUTE, Setting(1.e-12)},
        {Config::CVODE_MIN_RELATIVE, Setting(1.e-6)},
        {Config::SIMULATEOPTIONS_COPY_RESULT, Setting(true)},
        {Config::STEADYSTATE_PRESIMULATION, Setting(false)},
        {Config::STEADYSTATE_PRESIMULATION_MAX_STEPS, Setting(100)},
        {Config::STEADYSTATE_PRESIMULATION_TIME, Setting(100.0)},
        {Config::STEADYSTATE_APPROX, Setting(false)},
        {Config::STEADYSTATE_APPROX_TOL, Setting(1.e-6)},
        {Config::STEADYSTATE_APPROX_MAX_STEPS, Setting(10000)},
        {Config::STEADYSTATE_APPROX_TIME, Setting(10000.0)},
        {Config::STEADYSTATE_RELATIVE, Setting(1e-12)},
        {Config::STEADYSTATE_MAXIMUM_NUM_STEPS, Setting(100)},
        {Config::STEADYSTATE_MINIMUM_DAMPING, Setting(1e-20)},
        {Config::STEADYSTATE_BROYDEN, Setting(0)},
        {Config::STEADYSTATE_LINEARITY, Setting(3)},
        {Config::ROADRUNNER_JACOBIAN_MODE,
         Setting(static_cast<std::int32_t>(Config::ROADRUNNER_JACOBIAN_MODE_CONCENTRATIONS))},
        {Config::TEMP_DIR_PATH, Setting(std::string("."))},
        {Config::LOGGER_LOG_FILE_PATH, Setting(std::string())},
        {Config::RANDOM_SEED, Setting(static_cast<int>(kRandomSeed))},
        {Config::PYTHON_ENABLE_NAMED_MATRIX, Setting(true)},
        {Config::LLVM_SYMBOL_CACHE, Setting(true)},
        {Config::OPTIMIZE_REACTION_RATE_SELECTION, Setting(true)},
        {Config::LOADSBMLOPTIONS_PERMISSIVE, Setting(true)},
        {Config::MAX_OUTPUT_ROWS, Setting(100000)},
        {Config::ALLOW_EVENTS_IN_STEADY_STATE_CALCULATIONS, Setting(false)},
        {Config::MAX_EVENT_CASCADE, Setting(10000)},
        {Config::VALIDATION_IN_REGENERATION, Setting(true)},
        {Config::K_ROWS_PER_WRITE, Setting(1000)},
        {Config::LLVM_BACKEND, Setting(static_cast<std::int32_t>(Config::MCJIT))},
        {Config::LLJIT_OPTIMIZATION_LEVEL, Setting(static_cast<std::int32_t>(Config::NONE))},
        {Config::LLJIT_NUM_THREADS, Setting(1)},
        {Config::METABOLIC_CONTROL_ANALYSIS_FLUX_THRESHOLD, Setting(1.e-9)},
        {Config::METABOLIC_CONTROL_ANALYSIS_DIFFERENTIAL_STEP_SIZE, Setting(0.02)},
        {Config::METABOLIC_CONTROL_ANALYSIS_STEADY_STATE_THRESHOLD, Setting(1.e-2)},
    };
    static_assert(sizeof pinned / sizeof pinned[0] == Config::CONFIG_END,
                  "every key of rr::Config is pinned: review the new keys of this libRoadRunner");
    for (const auto& [key, value] : pinned) {
        Config::setValue(key, value);
    }
}

// ---------------------------------------------------------------------------------------------
// Model texts.

[[noreturn]] void refuse_text(const std::string& why, json details = json::object()) {
    details["reason"] = "model";
    fail(PRAENORA_STATUS_DATA_INVALID, "praenora.simulation: the model is not an SBML document " +
                                           std::string("the provider accepts: ") + why,
         std::move(details));
}

// The text libRoadRunner gets: the document from its first '<' on, after the checks of the
// feasibility rules for SBML text and libRoadRunner's own test of document text, so that
// libRoadRunner never reads the text as a path or a URI.
std::string checked_text(std::string_view text) {
    std::size_t start = 0;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        start = 3;
    }
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' ||
                                   text[start] == '\r' || text[start] == '\n')) {
        ++start;
    }
    if (start >= text.size() || text[start] != '<') {
        refuse_text("it does not start with '<' (the model is the document's text; a path or a "
                    "URI is never read)");
    }
    const std::string_view document = text.substr(start);
    if (document.find('\0') != std::string_view::npos) {
        refuse_text("it holds a NUL character");
    }
    // A document type declaration can declare entities, which are refused before any XML parser
    // sees the text (in any case of letters).
    std::string lower(document);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    });
    if (lower.find("<!doctype") != std::string::npos) {
        refuse_text("it has a document type declaration (<!DOCTYPE)");
    }
    // libRoadRunner takes a string as document text only when its own test says so; otherwise it
    // reads the string as a path or a URI. That test, not a copy of it, decides.
    std::string accepted(document);
    if (!rr::SBMLReader::is_sbml(accepted)) {
        refuse_text("libRoadRunner would not take it as document text (rr::SBMLReader::is_sbml), "
                    "and would read it as a path or a URI instead");
    }
    return accepted;
}

// The names an expression uses.
void collect_names(const libsbml::ASTNode* node, std::set<std::string>& names) {
    if (node == nullptr) {
        return;
    }
    if (node->getType() == libsbml::AST_NAME && node->getName() != nullptr) {
        names.insert(node->getName());
    }
    for (unsigned int i = 0; i < node->getNumChildren(); ++i) {
        collect_names(node->getChild(i), names);
    }
}

// libSBML's view of the text: no error; in Level 3, no required package and no comp at all; a
// model that libRoadRunner can simulate; and the symbols whose values feed stoichiometries that
// libRoadRunner computes once, when it loads the model (Level 3 initial assignments of species
// references, Level 2 stoichiometryMath of constant values): those symbols cannot be parameters.
struct SbmlFacts {
    std::set<std::string> stoichiometry_inputs;
};

SbmlFacts check_document(const std::string& text) {
    const std::unique_ptr<libsbml::SBMLDocument> document(
        libsbml::readSBMLFromString(text.c_str()));
    if (document == nullptr) {
        refuse_text("libSBML cannot read it");
    }
    for (unsigned int i = 0; i < document->getNumErrors(); ++i) {
        const libsbml::SBMLError* error = document->getError(i);
        if (error != nullptr && (error->isError() || error->isFatal())) {
            refuse_text("libSBML reports an error: " + error->getMessage(),
                        {{"line", error->getLine()}, {"column", error->getColumn()}});
        }
    }
    // Packages exist in Level 3 only (libSBML lists namespaces of its own Level 2 annotations as
    // required, which are not packages).
    const libsbml::XMLNamespaces* namespaces =
        document->getLevel() >= 3 ? document->getNamespaces() : nullptr;
    const std::string core = document->getSBMLNamespaces() != nullptr
                                 ? document->getSBMLNamespaces()->getURI()
                                 : std::string();
    for (int i = 0; namespaces != nullptr && i < namespaces->getLength(); ++i) {
        const std::string uri = namespaces->getURI(i);
        if (uri == core) {
            continue;
        }
        if (uri.find("/comp/") != std::string::npos ||
            document->getPackageRequired(uri)) {
            refuse_text("it declares the SBML Level 3 package " + uri +
                            " (a required package, or comp in any form, is not accepted)",
                        {{"package", uri}});
        }
    }
    const libsbml::Model* model = document->getModel();
    if (model == nullptr) {
        refuse_text("it has no model");
    }
    // libRoadRunner refuses to simulate a flux-balance model: one whose document has the fbc
    // package with its required attribute set and whose reactions have no kinetic law math (the
    // test of RoadRunner::simulate, which would fail every run).
    if (const auto* fbc =
            dynamic_cast<const libsbml::SBMLDocumentPlugin*>(document->getPlugin("fbc"))) {
        bool kinetic = false;
        for (unsigned int r = 0; r < model->getNumReactions(); ++r) {
            const libsbml::Reaction* reaction = model->getReaction(r);
            if (reaction->isSetKineticLaw() && reaction->getKineticLaw()->isSetMath()) {
                kinetic = true;
                break;
            }
        }
        if (fbc->isSetRequired() && !kinetic) {
            refuse_text("libRoadRunner cannot simulate it: it is a flux-balance model (the fbc "
                        "package) without kinetic laws");
        }
    }
    std::set<std::string> species_references;
    std::set<std::string> pending;
    const auto stoichiometry = [&](const libsbml::SpeciesReference* reference) {
        if (reference->isSetId()) {
            species_references.insert(reference->getId());
        }
        if (reference->isSetStoichiometryMath()) {
            collect_names(reference->getStoichiometryMath()->getMath(), pending);
        }
    };
    for (unsigned int r = 0; r < model->getNumReactions(); ++r) {
        const libsbml::Reaction* reaction = model->getReaction(r);
        for (unsigned int i = 0; i < reaction->getNumReactants(); ++i) {
            stoichiometry(reaction->getReactant(i));
        }
        for (unsigned int i = 0; i < reaction->getNumProducts(); ++i) {
            stoichiometry(reaction->getProduct(i));
        }
    }
    // The symbols in stoichiometryMath (above) and in the initial assignments of species
    // references, and, transitively, in the initial assignments and assignment rules of those
    // symbols.
    for (unsigned int i = 0; i < model->getNumInitialAssignments(); ++i) {
        const libsbml::InitialAssignment* assignment = model->getInitialAssignment(i);
        if (species_references.count(assignment->getSymbol()) != 0) {
            collect_names(assignment->getMath(), pending);
        }
    }
    SbmlFacts facts;
    while (!pending.empty()) {
        const std::string symbol = *pending.begin();
        pending.erase(pending.begin());
        if (!facts.stoichiometry_inputs.insert(symbol).second) {
            continue;
        }
        if (const libsbml::InitialAssignment* assignment = model->getInitialAssignment(symbol)) {
            collect_names(assignment->getMath(), pending);
        }
        if (const libsbml::Rule* rule = model->getAssignmentRule(symbol)) {
            collect_names(rule->getMath(), pending);
        }
    }
    return facts;
}

std::set<std::string> as_set(const std::list<std::string>& ids) {
    return std::set<std::string>(ids.begin(), ids.end());
}

// ---------------------------------------------------------------------------------------------
// The shared compiled models of the provider host (docs/specs/process-isolation.md): an entry is
// an envelope around a state that saveStateS('b') wrote, which loadStateS() reads back.

bool store_offered() noexcept {
    const praenora_provider_host* host = g_provider_host;
    return host != nullptr &&
           host->struct_size >= offsetof(praenora_provider_host, save_compiled) +
                                    sizeof(praenora_provider_host::save_compiled) &&
           host->load_compiled != nullptr && host->save_compiled != nullptr;
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

void put_u64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

std::uint64_t get_u64(std::string_view in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[i])) << (8 * i);
    }
    return value;
}

// The SHA-256 of `bytes`, its 32 bytes.
std::string sha256_bytes(std::string_view bytes) {
    Poco::SHA2Engine digest(Poco::SHA2Engine::SHA_256);
    digest.update(bytes.data(), bytes.size());
    const Poco::DigestEngine::Digest& value = digest.digest();
    return std::string(value.begin(), value.end());
}

// The saved state of `runner`, in its envelope. The caller holds the engine lock.
std::string envelope_of(rr::RoadRunner& runner) {
    const std::unique_ptr<std::stringstream> state(runner.saveStateS('b'));
    return state_envelope(state->str());
}

// A libRoadRunner object loaded from `entry`, an envelope this build made, without compiling; or
// nullptr, with why in `why`, when it does not load: the envelope's checks fail, or loadStateS()
// throws. The caller holds the engine lock.
std::unique_ptr<rr::RoadRunner> load_runner(std::string_view entry, std::string& why) {
    const std::optional<std::string_view> payload = envelope_payload(entry, why);
    if (!payload) {
        return nullptr;
    }
    try {
        auto runner = std::make_unique<rr::RoadRunner>();
        auto state = std::make_unique<std::stringstream>(
            std::string(*payload), std::ios::in | std::ios::out | std::ios::binary);
        runner->loadStateS(state.release()); // takes the stream over, also when it throws
        if (runner->getModel() == nullptr) {
            why = "libRoadRunner loaded no model from it";
            return nullptr;
        }
        return runner;
    } catch (const std::exception& error) {
        why = std::string("libRoadRunner could not load it: ") + error.what();
    } catch (...) {
        why = "libRoadRunner could not load it";
    }
    return nullptr;
}

// A libRoadRunner object compiled from the text (checked_text()). The caller holds the engine
// lock.
std::unique_ptr<rr::RoadRunner> compile_runner(const std::string& text) {
    rr::LoadSBMLOptions options;
    // MCJit, no LLVM optimization passes, no conserved-moiety analysis (so that the state vector
    // holds every floating species that no rule defines), settable initial values.
    options.modelGeneratorOpt = rr::LoadSBMLOptions::MUTABLE_INITIAL_CONDITIONS |
                                rr::LoadSBMLOptions::LLVM_SYMBOL_CACHE |
                                rr::LoadSBMLOptions::MCJIT | rr::LoadSBMLOptions::NONE;
    options.loadFlags = 0;
    try {
        return std::make_unique<rr::RoadRunner>(text, &options);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             std::string("praenora.simulation: libRoadRunner cannot compile the model: ") +
                 error.what(),
             {{"reason", "model"}, {"engine_error", engine_error(error)}});
    }
}

// What describe_model reports of a compiled libRoadRunner object, and how to read each quantity:
// the same for an object compiled from the text and one loaded from its saved state.
std::shared_ptr<const CompiledModel> describe_compiled(RunnerPtr runner, const SbmlFacts& facts) {
    rr::ExecutableModel* model = runner->getModel();
    if (model == nullptr) {
        fail(PRAENORA_STATUS_DATA_INVALID, "praenora.simulation: libRoadRunner made no model",
             {{"reason", "model"}});
    }

    ModelIds ids;
    std::vector<Quantity> quantities;
    const int state_count = model->getStateVector(nullptr);
    for (int i = 0; i < state_count; ++i) {
        ids.states.push_back(model->getStateVectorId(static_cast<std::size_t>(i)));
        quantities.push_back({QuantityKind::state, i, ids.states.back()});
    }
    const std::set<std::string> states(ids.states.begin(), ids.states.end());

    std::list<std::string> listed;
    model->getAssignmentRuleIds(listed);
    const std::set<std::string> assignment_rules = as_set(listed);
    listed.clear();
    model->getRateRuleIds(listed);
    const std::set<std::string> rate_rules = as_set(listed);
    listed.clear();
    model->getInitialAssignmentIds(listed);
    const std::set<std::string> initial_assignments = as_set(listed);

    std::vector<int> parameter_indices;
    std::vector<std::pair<int, std::string>> rule_parameters;
    for (int g = 0; g < model->getNumGlobalParameters(); ++g) {
        const std::string id = model->getGlobalParameterId(static_cast<std::size_t>(g));
        if (assignment_rules.count(id) != 0) {
            rule_parameters.emplace_back(g, id);
        } else if (rate_rules.count(id) == 0 && initial_assignments.count(id) == 0 &&
                   facts.stoichiometry_inputs.count(id) == 0) {
            ids.parameters.push_back(id);
            parameter_indices.push_back(g);
        }
    }
    const int species = model->getNumFloatingSpecies();
    for (int s = 0; s < species; ++s) {
        const std::string id = model->getFloatingSpeciesId(static_cast<std::size_t>(s));
        if (states.count(id) == 0) {
            quantities.push_back({QuantityKind::species_amount, s, id});
        }
    }
    for (int s = 0; s < species; ++s) {
        const std::string id = "[" + model->getFloatingSpeciesId(static_cast<std::size_t>(s)) + "]";
        quantities.push_back({QuantityKind::species_concentration, s, id});
    }
    for (int r = 0; r < model->getNumReactions(); ++r) {
        quantities.push_back({QuantityKind::reaction_rate, r,
                              model->getReactionId(static_cast<std::size_t>(r))});
    }
    for (const auto& [g, id] : rule_parameters) {
        quantities.push_back({QuantityKind::parameter, g, id});
    }
    for (const Quantity& quantity : quantities) {
        ids.quantities.push_back(quantity.selection);
    }
    const bool events = model->getNumEvents() > 0;
    return std::make_shared<const CompiledModel>(std::move(runner), std::move(ids),
                                                 std::move(parameter_indices),
                                                 std::move(quantities), events);
}

// What decides a compiled model besides its text: the engine (the provider manifest's engine
// object) and the load options (none but the format). Part of the cache key, and of every entry
// of the shared compiled models.
std::string engine_identity() {
    return std::string(PRAENORA_PROVIDER_ENGINE) + "\n{\"format\":\"" + kFormat + "\"}\n";
}

// The compiled model of the text (checked_text()) whose cache key is `key`: loaded from the shared
// compiled models when the provider host offers them and they hold it, otherwise compiled now and
// then kept there, when there are any. An entry that does not load is a warning, and the model is
// compiled. Under the engine lock: the text's checks, the store's load and loadStateS() or the
// compile, saveStateS() and the store's save.
std::shared_ptr<const CompiledModel> obtain(const std::string& key, const std::string& text,
                                            const praenora_host_api* host) {
    RunnerPtr runner; // declared before the lock: destroying it takes the lock
    SbmlFacts facts;
    bool loaded = false;
    {
        const EngineLock lock(host);
        facts = check_document(text);
        const bool store = store_offered();
        if (store) {
            std::string entry;
            praenora_sink sink{&entry, &append_to};
            if (g_provider_host->load_compiled(g_provider_host->context, key.data(), key.size(),
                                               &sink) == 1) {
                std::string why;
                if (std::unique_ptr<rr::RoadRunner> from_store = load_runner(entry, why)) {
                    runner.reset(from_store.release());
                    loaded = true;
                } else {
                    log_to(host, PRAENORA_LOG_WARN,
                           "roadrunner: the compiled model in the shared store does not load (" +
                               why + "); compiling it");
                }
            }
        }
        if (!loaded) {
            runner.reset(compile_runner(text).release());
            if (store) {
                // A state that cannot be saved is not kept, and the compile stands: a warning.
                std::string entry;
                std::string why;
                try {
                    entry = envelope_of(*runner);
                } catch (const std::exception& error) {
                    why = std::string("libRoadRunner could not save its state: ") + error.what();
                } catch (...) {
                    why = "libRoadRunner could not save its state";
                }
                if (why.empty()) {
                    g_provider_host->save_compiled(g_provider_host->context, key.data(), key.size(),
                                                   entry.data(), entry.size());
                } else {
                    log_to(host, PRAENORA_LOG_WARN,
                           "roadrunner: the compiled model is not kept in the shared store: " +
                               why);
                }
            }
        }
    }
    log_to(host, PRAENORA_LOG_DEBUG,
           loaded ? "roadrunner: loaded the compiled model from the shared store"
                  : "roadrunner: compiled the model");
    return describe_compiled(std::move(runner), facts);
}

// ---------------------------------------------------------------------------------------------
// The cache of compiled models.

class ModelCache {
public:
    std::shared_ptr<const CompiledModel> get(const std::string& key, const std::string& text,
                                             const praenora_host_api* host) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            const auto found = entries_.find(key);
            if (found == entries_.end()) {
                entries_[key].compiling = true; // this call compiles it
                break;
            }
            if (found->second.model) {
                found->second.used = ++clock_;
                std::shared_ptr<const CompiledModel> model = found->second.model;
                lock.unlock();
                log_to(host, PRAENORA_LOG_DEBUG, "roadrunner: the model was compiled before");
                return model;
            }
            // Another call compiles it: wait, polling for cancellation without the lock.
            changed_.wait_for(lock, kPollInterval);
            lock.unlock();
            check_cancelled(host);
            lock.lock();
        }
        lock.unlock();

        std::shared_ptr<const CompiledModel> model;
        try {
            model = obtain(key, text, host);
        } catch (...) {
            lock.lock();
            entries_.erase(key);
            changed_.notify_all();
            throw;
        }

        std::vector<std::shared_ptr<const CompiledModel>> evicted;
        lock.lock();
        Entry& entry = entries_[key];
        entry.model = model;
        entry.compiling = false;
        entry.used = ++clock_;
        for (;;) {
            std::size_t compiled = 0;
            auto oldest = entries_.end();
            for (auto i = entries_.begin(); i != entries_.end(); ++i) {
                if (i->second.model) {
                    ++compiled;
                    if (oldest == entries_.end() || i->second.used < oldest->second.used) {
                        oldest = i;
                    }
                }
            }
            if (compiled <= kCacheCapacity) {
                break;
            }
            evicted.push_back(std::move(oldest->second.model));
            entries_.erase(oldest);
        }
        changed_.notify_all();
        lock.unlock();
        // The evicted models go here, outside the lock (destroying one takes the engine lock); a
        // session, plan or kernel that uses one keeps it alive.
        evicted.clear();
        return model;
    }

private:
    struct Entry {
        std::shared_ptr<const CompiledModel> model;
        bool compiling = false;
        std::uint64_t used = 0;
    };

    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Entry> entries_;
    std::uint64_t clock_ = 0;
};

ModelCache& model_cache() {
    static auto* const cache = new ModelCache(); // never destroyed: used until exit
    return *cache;
}

// ---------------------------------------------------------------------------------------------
// Runs.

// Thrown by the integrator listener when the host asks the call to stop.
struct RunCancelled {};

// Polls the host after each step of the integrator (each output interval of a run, or each step
// of a run of solver steps).
class CancellationListener final : public rr::IntegratorListener {
public:
    explicit CancellationListener(const praenora_host_api* host) : host_(host) {}

    unsigned int onTimeStep(rr::Integrator*, rr::ExecutableModel*, double) override {
        poll();
        return 0;
    }
    unsigned int onEvent(rr::Integrator*, rr::ExecutableModel*, double) override {
        poll();
        return 0;
    }

private:
    void poll() const {
        if (cancel_requested(host_)) {
            throw RunCancelled{};
        }
    }

    const praenora_host_api* host_;
};

// Removes the integrator's listener when the run ends, however it ends.
class ListenerScope {
public:
    ListenerScope(rr::Integrator& integrator, const praenora_host_api* host)
        : integrator_(integrator) {
        if (host_has(host, offsetof(praenora_host_api, cancel_requested),
                     sizeof host->cancel_requested) &&
            host->cancel_requested != nullptr) {
            integrator_.setListener(std::make_shared<CancellationListener>(host));
        }
    }
    ListenerScope(const ListenerScope&) = delete;
    ListenerScope& operator=(const ListenerScope&) = delete;
    ~ListenerScope() { integrator_.setListener(rr::IntegratorListenerPtr()); }

private:
    rr::Integrator& integrator_;
};

json numerical_failure(const std::string& reason, const std::string& message,
                       json details = json::object()) {
    details["reason"] = reason;
    details["service"] = PRAENORA_SIMULATION_SERVICE;
    return {{"code", PRAENORA_STATUS_FAILED},
            {"message", "praenora.simulation: " + message},
            {"details", std::move(details)}};
}

// The failure document of the first value of `out` that is not finite, or null.
json first_nonfinite(const RunSettings& settings, const double* out, std::size_t rows) {
    const std::size_t columns = settings.columns.size();
    for (std::size_t i = 0; i < rows * columns; ++i) {
        if (!std::isfinite(out[i])) {
            const std::string& column = settings.columns[i % columns];
            const double time = out[(i / columns) * columns];
            return numerical_failure("nonfinite",
                                     column + " is not finite at time " + json(time).dump(),
                                     {{"column", column}, {"time", time}});
        }
    }
    return nullptr;
}

// Empties libRoadRunner's queue of pending event assignments, which its reset keeps: they are
// applied at time +infinity (the model's values do not matter, a reset follows), with the trigger
// states there as the previous ones, so that no trigger is new (an assignment that turns another
// trigger on may queue more: a few rounds). False when the queue is still not empty, or
// libRoadRunner failed.
bool drain_events(rr::ExecutableModel& model) noexcept {
    try {
        const int events = model.getNumEvents();
        for (int round = 0; round < 3 && model.getPendingEventSize() > 0; ++round) {
            const double end = std::numeric_limits<double>::infinity();
            model.setTime(end);
            std::vector<unsigned char> status(static_cast<std::size_t>(std::max(events, 1)), 0);
            model.getEventTriggers(static_cast<std::size_t>(events), nullptr, status.data());
            model.applyEvents(end, status.data(), nullptr, nullptr);
        }
        return model.getPendingEventSize() == 0;
    } catch (...) {
        return false;
    }
}

// Resets the model to its original state, then sets the run's parameter values as initial values
// (evaluating the initial assignments with them) and its initial conditions as state values.
void initialize(rr::ExecutableModel& model, const CompiledModel& compiled,
                const RunValues& values) {
    model.reset(reset_everything());
    model.setRandomSeed(kRandomSeed);
    if (!values.parameters.empty()) {
        std::vector<int> indices;
        std::vector<double> numbers;
        for (const auto& [index, value] : values.parameters) {
            indices.push_back(compiled.parameter_indices()[index]);
            numbers.push_back(value);
        }
        model.setGlobalParameterInitValues(indices.size(), indices.data(), numbers.data());
    }
    model.reset(reset_to_initial_values());
    if (!values.states.empty()) {
        std::vector<double> state(static_cast<std::size_t>(model.getStateVector(nullptr)));
        model.getStateVector(state.data());
        for (const auto& [index, value] : values.states) {
            state[index] = value;
        }
        model.setStateVector(state.data());
    }
}

void configure_solver(rr::Integrator& integrator, const RunSettings& settings,
                      std::size_t state_count) {
    const SolverSettings& solver = settings.solver;
    if (static_cast<bool>(integrator.getValue("stiff")) != solver.stiff) {
        integrator.setValue("stiff", rr::Setting(solver.stiff)); // makes CVODE's memory again
    }
    integrator.setValue("relative_tolerance", rr::Setting(solver.relative_tolerance));
    // One absolute tolerance per state, as given: libRoadRunner scales a single value by the
    // states' initial values, a vector it uses as it is.
    integrator.setValue("absolute_tolerance",
                        rr::Setting(std::vector<double>(state_count, solver.absolute_tolerance)));
    integrator.setValue("maximum_num_steps", rr::Setting(static_cast<int>(solver.max_steps)));
    integrator.setValue("maximum_time_step", rr::Setting(solver.max_step.value_or(0.0)));
    integrator.setValue("minimum_time_step", rr::Setting(solver.min_step.value_or(0.0)));
    integrator.setValue("initial_time_step", rr::Setting(0.0));
    integrator.setValue("multiple_steps", rr::Setting(false));
    integrator.setValue("variable_step_size", rr::Setting(settings.solver_steps));
    // A run of solver steps stops at one row more than it may have.
    const std::uint64_t rows = std::min<std::uint64_t>(
        settings.max_rows + 1, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    integrator.setValue("max_output_rows", rr::Setting(static_cast<int>(rows)));
}

// Runs the time course of `options`; returns the failure document of a numerical failure, or
// null. Sets `fresh_copy_needed` when the run ended with an exception.
json integrate(rr::RoadRunner& runner, rr::SimulateOptions& options,
               const praenora_host_api* host, const ls::DoubleMatrix*& matrix,
               bool& fresh_copy_needed) {
    rr::Integrator* integrator = runner.getIntegrator();
    try {
        const ListenerScope listener(*integrator, host);
        matrix = runner.simulate(&options);
        return nullptr;
    } catch (const RunCancelled&) {
        fresh_copy_needed = true;
        fail(PRAENORA_STATUS_CANCELLED, "praenora.simulation: the call was cancelled");
    } catch (const std::bad_alloc&) {
        fresh_copy_needed = true;
        throw;
    } catch (const rr::IntegratorException& error) {
        fresh_copy_needed = true;
        // CVODE's CV_TOO_MUCH_WORK: it took max_steps steps without reaching the next output time
        // (libRoadRunner reports it only in the text).
        const std::string what = error.what();
        const bool step_limit = what.find("CV_TOO_MUCH_WORK") != std::string::npos;
        return numerical_failure(step_limit ? "step_limit" : "integrator",
                                 step_limit ? "the solver reached max_steps: " + what
                                            : "the solver failed: " + what,
                                 {{"engine_error", engine_error(error)}});
    } catch (const std::exception& error) {
        fresh_copy_needed = true;
        return numerical_failure("integrator",
                                 std::string("libRoadRunner failed during the run: ") +
                                     error.what(),
                                 {{"engine_error", engine_error(error)}});
    } catch (...) {
        fresh_copy_needed = true;
        throw;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------

void RunnerDeleter::operator()(rr::RoadRunner* runner) const noexcept {
    if (runner != nullptr) {
        const EngineLock lock;
        delete runner;
    }
}

void start_engine(const praenora_provider_host* host) {
    // Its store callbacks, if any, for the shared compiled models.
    g_provider_host = host;
    // Logging first, so that what libRoadRunner logs from now on goes to the provider's logger.
    // The forwarder and its stream are used by libRoadRunner's logger until the process ends.
    static auto* const forwarder = new LogForwarder(host);
    static auto* const stream = new std::ostream(forwarder);
    rr::Logger::setLevel(rr::Logger::LOG_ERROR);
    rr::Logger::setFormattingPattern("%p: %t");
    rr::Logger::setConsoleStream(stream);
    rr::Logger::setLevel(rr::Logger::LOG_ERROR);
    pin_configuration();
    // Initializes LLVM's native target and registers libRoadRunner's solvers, once.
    const EngineLock lock;
    rr::RoadRunner probe;
    static_cast<void>(probe);
}

CompiledModel::CompiledModel(RunnerPtr runner, ModelIds ids, std::vector<int> parameter_indices,
                             std::vector<Quantity> quantities, bool events)
    : runner_(std::move(runner)), ids_(std::move(ids)),
      parameter_indices_(std::move(parameter_indices)), quantities_(std::move(quantities)),
      events_(events) {
    description_ = json{{"events", events_},
                        {"states", ids_.states},
                        {"parameters", ids_.parameters},
                        {"quantities", ids_.quantities}}
                       .dump();
}

RunnerPtr CompiledModel::copy(const praenora_host_api* host) const {
    const EngineLock lock(host);
    return RunnerPtr(new rr::RoadRunner(*runner_));
}

std::string CompiledModel::saved_state(const praenora_host_api* host) const {
    const EngineLock lock(host);
    return envelope_of(*runner_);
}

std::string state_envelope(std::string_view payload) {
    const std::string identity = engine_identity();
    std::string entry(kStateMagic, kStateMagicSize);
    put_u64(entry, identity.size());
    entry += identity;
    put_u64(entry, payload.size());
    entry += sha256_bytes(payload);
    entry.append(payload.data(), payload.size());
    return entry;
}

std::optional<std::string_view> envelope_payload(std::string_view entry, std::string& why) {
    if (entry.size() < kStateMagicSize + 8 ||
        entry.substr(0, kStateMagicSize) != std::string_view(kStateMagic, kStateMagicSize)) {
        why = "it is not a saved state of this provider";
        return std::nullopt;
    }
    entry.remove_prefix(kStateMagicSize);
    const std::string identity = engine_identity();
    const std::uint64_t identity_size = get_u64(entry);
    entry.remove_prefix(8);
    if (identity_size != identity.size() || entry.size() < identity_size + 8 + 32 ||
        entry.substr(0, identity.size()) != identity) {
        why = "it was saved by another engine";
        return std::nullopt;
    }
    entry.remove_prefix(identity.size());
    const std::uint64_t size = get_u64(entry);
    entry.remove_prefix(8);
    const std::string_view digest = entry.substr(0, 32);
    entry.remove_prefix(32);
    if (size != entry.size()) {
        why = "its state has " + std::to_string(entry.size()) + " bytes, not the " +
              std::to_string(size) + " it says";
        return std::nullopt;
    }
    if (digest != sha256_bytes(entry)) {
        why = "its state does not have the SHA-256 it says";
        return std::nullopt;
    }
    return entry;
}

std::shared_ptr<const CompiledModel> compile_model(std::string_view text,
                                                   const praenora_host_api* host) {
    const std::string document = checked_text(text);
    RunnerPtr runner;
    SbmlFacts facts;
    {
        const EngineLock lock(host);
        facts = check_document(document);
        runner.reset(compile_runner(document).release());
    }
    return describe_compiled(std::move(runner), facts);
}

std::shared_ptr<const CompiledModel> load_compiled_model(std::string_view text,
                                                         std::string_view entry,
                                                         const praenora_host_api* host,
                                                         std::string& why) {
    const std::string document = checked_text(text);
    RunnerPtr runner;
    SbmlFacts facts;
    {
        const EngineLock lock(host);
        facts = check_document(document);
        std::unique_ptr<rr::RoadRunner> loaded = load_runner(entry, why);
        if (!loaded) {
            return nullptr;
        }
        runner.reset(loaded.release());
    }
    return describe_compiled(std::move(runner), facts);
}

std::shared_ptr<const CompiledModel> compiled_model(std::string_view text,
                                                    const praenora_host_api* host) {
    const std::string document = checked_text(text);
    // The key: the model bytes, the load options (none but the format) and the engine identity.
    // It is the key of the shared compiled models too: nothing in it belongs to the process.
    Poco::SHA2Engine digest(Poco::SHA2Engine::SHA_256);
    const std::string identity = engine_identity();
    digest.update(identity.data(), identity.size());
    digest.update(text.data(), text.size());
    const std::string key = Poco::DigestEngine::digestToHex(digest.digest());
    return model_cache().get(key, document, host);
}

// ---------------------------------------------------------------------------------------------

Simulator::Simulator(std::shared_ptr<const CompiledModel> model, const praenora_host_api* host)
    : model_(std::move(model)), runner_(model_->copy(host)) {}

void Simulator::prepare_run(const RunSettings& settings, const RunValues& values,
                            const praenora_host_api* host) {
    if (fresh_copy_needed_) {
        RunnerPtr fresh = model_->copy(host);
        runner_ = std::move(fresh);
        selections_.clear();
        fresh_copy_needed_ = false;
    }
    // libRoadRunner's reset keeps the event assignments still pending (delayed events a run left,
    // or those its copy constructor queued): no run may inherit them.
    if (!drain_events(*runner_->getModel())) {
        RunnerPtr fresh = model_->copy(host);
        runner_ = std::move(fresh);
        selections_.clear();
        if (!drain_events(*runner_->getModel())) {
            fresh_copy_needed_ = true;
            fail(PRAENORA_STATUS_INTERNAL,
                 "praenora.simulation: libRoadRunner keeps event assignments pending that the "
                 "provider cannot clear");
        }
    }
    try {
        rr::RoadRunner& runner = *runner_;
        rr::ExecutableModel& model = *runner.getModel();
        initialize(model, *model_, values);
        std::vector<std::string> selections = {"time"};
        if (settings.include_states) {
            selections.insert(selections.end(), model_->ids().states.begin(),
                              model_->ids().states.end());
        }
        for (const std::size_t index : settings.quantities) {
            selections.push_back(model_->quantities()[index].selection);
        }
        if (selections != selections_) {
            selections_.clear();
            runner.setSelections(selections);
            selections_ = std::move(selections);
        }
        rr::Integrator* integrator = runner.getIntegrator();
        if (integrator == nullptr || integrator->getName() != "cvode") {
            runner.setIntegrator("cvode");
            integrator = runner.getIntegrator();
        }
        configure_solver(*integrator, settings, model_->ids().states.size());
        // The event triggers are evaluated at the start against their initial values, as
        // libRoadRunner does when it loads a model and the run starts at 0: events that fire at
        // the start are applied, or queued with their delays from the start. (libRoadRunner's
        // simulate evaluates the triggers at the start against their values there, so it never
        // fires an event at the start by itself.)
        integrator->setIntegrationStartTime(settings.start);
        model.setIntegrationStartTime(settings.start);
        integrator->restart(settings.start);
    } catch (...) {
        fresh_copy_needed_ = true;
        throw;
    }
}

json Simulator::run(const RunSettings& settings, const RunValues& values, double* out,
                    const praenora_host_api* host) {
    // libRoadRunner integrates from its first time, the start: a first output time after the
    // start is preceded by the start, whose row is dropped; a single output time at the start is
    // followed by the end, whose row is dropped.
    std::vector<double> times;
    std::size_t skipped = 0;
    if (settings.times.front() > settings.start) {
        times.push_back(settings.start);
        skipped = 1;
    }
    times.insert(times.end(), settings.times.begin(), settings.times.end());
    if (times.size() == 1) {
        times.push_back(settings.end);
    }
    prepare_run(settings, values, host);

    rr::SimulateOptions options;
    options.setTimes(times);
    const ls::DoubleMatrix* matrix = nullptr;
    const json failure = integrate(*runner_, options, host, matrix, fresh_copy_needed_);
    if (!failure.is_null()) {
        return failure;
    }
    const std::size_t rows = settings.times.size();
    const std::size_t columns = settings.columns.size();
    if (matrix == nullptr || matrix->numRows() != times.size() || matrix->numCols() != columns) {
        fresh_copy_needed_ = true;
        fail(PRAENORA_STATUS_INTERNAL,
             "praenora.simulation: libRoadRunner returned a result of an unexpected shape");
    }
    for (std::size_t row = 0; row < rows; ++row) {
        double* cells = out + row * columns;
        cells[0] = settings.times[row]; // the output times exactly
        for (std::size_t column = 1; column < columns; ++column) {
            cells[column] = (*matrix)(static_cast<unsigned int>(row + skipped),
                                      static_cast<unsigned int>(column));
        }
    }
    return first_nonfinite(settings, out, rows);
}

json Simulator::run_steps(const RunSettings& settings, const RunValues& values,
                          std::vector<double>& out, std::size_t& rows,
                          const praenora_host_api* host) {
    prepare_run(settings, values, host);
    rr::SimulateOptions options;
    options.setStartTime(settings.start);
    options.setDuration(settings.end - settings.start);
    const ls::DoubleMatrix* matrix = nullptr;
    const json failure = integrate(*runner_, options, host, matrix, fresh_copy_needed_);
    // libRoadRunner's variable-step mode keeps an event or switch found at the end of a step for
    // the next call, which a restart does not clear: the next run takes a fresh copy, whatever
    // this one did.
    fresh_copy_needed_ = true;
    if (!failure.is_null()) {
        return failure;
    }
    const std::size_t columns = settings.columns.size();
    if (matrix == nullptr || matrix->numCols() != columns || matrix->numRows() < 2) {
        fail(PRAENORA_STATUS_INTERNAL,
             "praenora.simulation: libRoadRunner returned a result of an unexpected shape");
    }
    rows = matrix->numRows();
    if (rows > settings.max_rows) {
        fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED,
             "praenora.simulation: the run has more solver steps than its output may have rows",
             {{"rows", settings.max_rows},
              {"columns", columns},
              {"limit", PRAENORA_SIMULATION_MAX_RUN_VALUES}});
    }
    out.assign(rows * columns, 0.0);
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            out[row * columns + column] = (*matrix)(static_cast<unsigned int>(row),
                                                    static_cast<unsigned int>(column));
        }
    }
    // libRoadRunner ends at start + (end - start), which may differ from end in the last bit.
    out[0] = settings.start;
    out[(rows - 1) * columns] = settings.end;
    return first_nonfinite(settings, out.data(), rows);
}

// ---------------------------------------------------------------------------------------------

KernelModel::KernelModel(std::shared_ptr<const CompiledModel> model, std::vector<int> parameters,
                         std::vector<Quantity> quantities, const praenora_host_api* host)
    : model_(std::move(model)), runner_(model_->copy(host)), engine_model_(runner_->getModel()),
      parameters_(std::move(parameters)), quantities_(std::move(quantities)),
      state_count_(model_->ids().states.size()), initial_(state_count_, 0.0) {
    initialize(*engine_model_, *model_, RunValues{});
    if (state_count_ > 0) {
        engine_model_->getStateVector(initial_.data());
    }
}

void KernelModel::rhs(double t, const double* y, double* dydt) {
    if (state_count_ > 0) {
        engine_model_->getStateVectorRate(t, y, dydt);
    }
}

void KernelModel::set_parameters(const double* values) {
    if (!parameters_.empty()) {
        engine_model_->setGlobalParameterInitValues(parameters_.size(), parameters_.data(),
                                                    values);
    }
    // The values derived from the parameters (initial assignments, compartment sizes) follow.
    engine_model_->reset(reset_to_initial_values());
    if (state_count_ > 0) {
        engine_model_->getStateVector(initial_.data());
    }
}

void KernelModel::initial_state(double* y) const {
    std::copy(initial_.begin(), initial_.end(), y);
}

void KernelModel::quantities(double t, const double* y, double* out) {
    engine_model_->setTime(t);
    if (state_count_ > 0) {
        engine_model_->setStateVector(y);
    }
    for (std::size_t i = 0; i < quantities_.size(); ++i) {
        const Quantity& quantity = quantities_[i];
        const int index = quantity.index;
        switch (quantity.kind) {
        case QuantityKind::state:
            out[i] = y[index];
            break;
        case QuantityKind::species_amount:
            engine_model_->getFloatingSpeciesAmounts(1, &index, &out[i]);
            break;
        case QuantityKind::species_concentration:
            engine_model_->getFloatingSpeciesConcentrations(1, &index, &out[i]);
            break;
        case QuantityKind::reaction_rate:
            engine_model_->getReactionRates(1, &index, &out[i]);
            break;
        case QuantityKind::parameter:
            engine_model_->getGlobalParameterValues(1, &index, &out[i]);
            break;
        }
    }
}

} // namespace praenora::roadrunner
