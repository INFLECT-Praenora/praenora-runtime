// The antimony provider's use of libAntimony (engine.hpp). Every function here runs under
// engine_lock(). libAntimony hands out text it allocated and keeps track of: it is copied at once,
// never freed here, and freeAll() frees it at the end of each translation.
#include "engine.hpp"

#include "sbml_checks.hpp"
#include "source_checks.hpp"

#include <antimony_api.h>
#include <sbml/common/libsbml-version.h>
#include <sbmlnetwork/libsbmlnetwork_common.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace praenora::antimony_provider {

std::timed_mutex& engine_lock() {
    static std::timed_mutex lock;
    return lock;
}

namespace {

std::string copied(const char* text) { return text == nullptr ? std::string() : std::string(text); }

std::string last_error() { return without_trailing_white_space(copied(getLastError())); }

// libAntimony's text when a function failed: out of memory, or another failure. The failure has no
// details.line: libAntimony's own count of lines, which its message names, skips the line ends
// inside block comments and triple-backtick blocks and counts a run of continued lines as one, so
// it does not locate the problem in the source.
[[noreturn]] void engine_failed(const std::string& what) {
    const std::string message = last_error();
    if (message == "Out of memory error.") {
        fail(PRAENORA_STATUS_RESOURCE_EXHAUSTED, "libAntimony ran out of memory");
    }
    engine_failure("libantimony", what, message.empty() ? "an unknown failure" : message);
}

// Clears libAntimony's models and directories when it is made and when it goes, and then frees
// what libAntimony handed out.
class Cleared {
public:
    Cleared() {
        clearPreviousLoads();
        clearDirectories();
    }
    Cleared(const Cleared&) = delete;
    Cleared& operator=(const Cleared&) = delete;
    ~Cleared() {
        try {
            freeAll();
            clearPreviousLoads();
        } catch (...) {
            // Nothing else can be done here; the next translation clears again.
        }
    }
};

// libAntimony's warnings: one entry for each non-empty line, in order.
std::vector<std::string> warnings() {
    const std::string text = copied(getWarnings());
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = without_trailing_white_space(text.substr(start, end - start));
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        start = end + 1;
    }
    return lines;
}

// The names of the modules of what libAntimony loaded.
std::vector<std::string> module_names() {
    std::vector<std::string> names;
    const unsigned long count = getNumModules();
    for (unsigned long i = 0; i < count; ++i) {
        names.push_back(copied(getNthModuleName(i)));
    }
    return names;
}

// What the libraries the provider links report: {"version", "libsbml", "sbmlnetwork"}.
json linked_versions() {
    std::string antimony = copied(getVersionStr());
    if (!antimony.empty() && antimony.front() == 'v') {
        antimony.erase(0, 1);
    }
    return {{"version", antimony},
            {"libsbml", copied(libsbml::getLibSBMLDottedVersion())},
            {"sbmlnetwork", getLibraryVersion()}};
}

} // namespace

json start_engine(const json& engine) {
    setRemoveFunctionDefinitions(false);
    setWriteSBMLTimestamp(false);
    clearDirectories();
    clearPreviousLoads();
    const json linked = linked_versions();
    freeAll();
    for (const auto& [field, library] : std::vector<std::pair<const char*, const char*>>{
             {"version", "libAntimony"}, {"libsbml", "libSBML"}, {"sbmlnetwork", "SBMLNetwork"}}) {
        if (engine.value(field, json()) != linked.at(field)) {
            fail(PRAENORA_STATUS_INTERNAL,
                 std::string("the antimony provider links ") + library + " " +
                     linked.at(field).get<std::string>() + ", but its engine object names " +
                     engine.value(field, json()).dump(),
                 {{"engine", engine}, {"linked", linked}});
        }
    }
    return linked;
}

Translation translate(const Options& options, std::string_view source) {
    const Cleared cleared;
    long loaded = -1;
    if (options.from == kSbml) {
        const SbmlDocumentText text = sbml_engine_text(source);
        check_sbml_document(text);
        loaded = loadSBMLString(text.document.c_str());
    } else {
        const std::string text = antimony_engine_text(source);
        loaded = loadAntimonyString(text.c_str());
    }
    if (loaded < 0) {
        engine_failed("libAntimony cannot read the " + options.from + " source");
    }
    std::string module;
    if (options.module.has_value()) {
        module = *options.module;
    } else {
        const char* main = getMainModuleName();
        if (main == nullptr) {
            engine_failed("libAntimony found no main module in the source");
        }
        module = main;
    }
    const std::vector<std::string> modules = module_names();
    if (std::find(modules.begin(), modules.end(), module) == modules.end()) {
        fail(PRAENORA_STATUS_NOT_FOUND, "the source has no module '" + module + "'",
             {{"module", module}, {"modules", modules}});
    }
    const char* text =
        options.to == kSbml ? getSBMLString(module.c_str()) : getAntimonyString(module.c_str());
    if (text == nullptr) {
        engine_failed("libAntimony cannot write module " + module + " as " + options.to);
    }
    Translation translation;
    translation.text = text;
    // A layout in the SBML is SBMLNetwork's automatic layout, whose positions are random, seeded
    // from the wall clock: the text would change from one second to the next. Refused. (Loading the
    // source has already run that layout, and reseeded the C library's rand().)
    if (options.to == kSbml && declares_layout(translation.text)) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the antimony source asks for a layout, which SBMLNetwork computes from random "
             "positions seeded from the wall clock, so its SBML would not be reproducible: the "
             "antimony provider does not convert it",
             {{"reason", "layout"}});
    }
    translation.module = module;
    translation.warnings = warnings();
    return translation;
}

} // namespace praenora::antimony_provider
