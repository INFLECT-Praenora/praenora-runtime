// Module discovery: the module search paths, the index of the manifests found on them, and the
// resolution of a module name and version to one manifest.
#ifndef PRAENORA_RUNTIME_DISCOVERY_MODULE_INDEX_HPP
#define PRAENORA_RUNTIME_DISCOVERY_MODULE_INDEX_HPP

#include "core/semver.hpp"
#include "discovery/manifest.hpp"
#include "discovery/search_paths.hpp"

#include <praenora/runtime/error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// The module search paths, in the order they are searched:
//   1. `explicit_paths` (API or CLI, UTF-8; relative ones against the current directory);
//   2. `config_paths` (the configuration's modules.search_paths, already resolved);
//   3. PRAENORA_MODULE_PATH, when `use_environment` (':'-separated, ';' on Windows; empty
//      entries are skipped, relative ones are relative to the current directory);
//   4. installation_module_dir().
// Every path is made absolute and lexically normal, without a trailing separator; a path that
// repeats an earlier one is dropped. Each keeps its origin (caller, configuration, environment,
// installation) and its index among its origin's non-empty entries (SearchPath).
std::vector<SearchPath> module_search_paths(const std::vector<std::string>& explicit_paths,
                                            const std::vector<std::filesystem::path>& config_paths,
                                            bool use_environment);

// The installation's default module directory, <prefix>/lib/praenora/modules, located from the
// binary that holds the runtime (platform/platform.hpp): <its directory>/praenora/modules, and on
// Windows, where the runtime DLL sits in <prefix>/bin, <its directory>/../lib/praenora/modules.
// The directory need not exist. nullopt when the runtime's binary cannot be located.
std::optional<std::filesystem::path> installation_module_dir();

// The valid manifests on a list of search paths.
class ModuleIndex {
public:
    // Scans each path, then its immediate subdirectories, for files named *.pmm.json: first the
    // path's own files, then each subdirectory's, everything in sorted (UTF-8 byte) order.
    // Missing paths are skipped. Every manifest is loaded with load_manifest():
    // - an invalid manifest becomes a warning diagnostic carrying its error code and location,
    //   and is skipped;
    // - a manifest whose name, version and platform repeat an earlier one is indexed but can
    //   never be resolved; it gets a warning naming both paths;
    // - manifests for other platforms are indexed too and shadow nothing.
    // A manifest file reached twice (overlapping search paths) is indexed once. Each indexed
    // manifest's `location` says where it was found: the search path that reached it first.
    static ModuleIndex scan(const std::vector<SearchPath>& search_paths);
    // The same over `search_paths` taken as the caller's paths (caller_search_paths()).
    static ModuleIndex scan(const std::vector<std::filesystem::path>& search_paths);

    // Warnings found while scanning, in the order found. Warnings that report no failure (a
    // shadowed manifest) carry ErrorCode::OK; severity says how much they weigh.
    const std::vector<Diagnostic>& diagnostics() const;

    // The first manifest, in search order, of `name` at `version` (build metadata ignored) for
    // the host platform (compat/compat.hpp), or nullptr. Then `why_not`, when given, receives an
    // error diagnostic: PLATFORM_MISMATCH when manifests of that name and version exist only for
    // other platforms (naming them), otherwise MODULE_NOT_FOUND, which lists the searched paths
    // and any other versions of the module that were found. The pointer stays valid as long as
    // this index exists and is not modified.
    const ModuleManifest* resolve(std::string_view name, const SemVer& version,
                                  Diagnostic* why_not = nullptr) const;

    // Every indexed manifest, in scan order.
    std::vector<const ModuleManifest*> entries() const;

    // The paths given to scan().
    const std::vector<std::filesystem::path>& search_paths() const;

private:
    void add(ModuleManifest manifest);

    std::vector<std::filesystem::path> search_paths_;
    std::vector<ModuleManifest> manifests_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DISCOVERY_MODULE_INDEX_HPP
