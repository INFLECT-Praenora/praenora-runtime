// The antimony provider's use of libAntimony (engine.cpp): the configuration set once at start,
// the check of the linked libraries' versions, and the translations, which run only under the
// provider's lock (docs/specs/model-conversion-service.md, the antimony provider's section).
// libAntimony's and libSBML's functions stay in engine.cpp and sbml_checks.cpp.
#ifndef PRAENORA_PROVIDERS_ANTIMONY_ENGINE_HPP
#define PRAENORA_PROVIDERS_ANTIMONY_ENGINE_HPP

#include "documents.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::antimony_provider {

// The provider's process-wide lock: libAntimony keeps its models, errors, warnings and settings in
// process-global state, and libSBML's writer a process-global setting. Every call into either is
// made under it.
std::timed_mutex& engine_lock();

// Sets libAntimony's process-wide configuration (function definitions kept, no timestamp in SBML,
// no directory to search for imports, nothing loaded) and checks that the libraries the provider
// links report the versions of `engine`, the manifest's engine object; a difference fails
// (INTERNAL, naming it). Returns what they report: {"version", "libsbml", "sbmlnetwork"}. Called
// once, from start(), under the lock.
json start_engine(const json& engine);

// A translation: the text, the module translated and libAntimony's warnings, one per non-empty
// line.
struct Translation {
    std::string text;
    std::string module;
    std::vector<std::string> warnings;
};

// Translates `source` (a source that passed the rules that read its text alone) as `options` say:
// for sbml, the rules that need libSBML's parser first; then libAntimony loads it, finds the
// module and writes it. SBML that holds a layout (SBMLNetwork's, seeded from the wall clock) is
// refused: UNSUPPORTED, details.reason "layout". libAntimony's state is cleared before and after.
// Under the lock only.
Translation translate(const Options& options, std::string_view source);

} // namespace praenora::antimony_provider

#endif // PRAENORA_PROVIDERS_ANTIMONY_ENGINE_HPP
