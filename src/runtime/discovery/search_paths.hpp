// What module discovery (discovery/module_index.hpp) and provider discovery
// (provider/provider_index.hpp) share: search paths and where they come from, the installation's
// directories, the manifest files on a search path and where a manifest was found.
#ifndef PRAENORA_RUNTIME_DISCOVERY_SEARCH_PATHS_HPP
#define PRAENORA_RUNTIME_DISCOVERY_SEARCH_PATHS_HPP

#include <praenora/runtime/error.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// Where a search path comes from, in search order: the caller's paths (the C++ API, the tool's
// options, the Python package), the configuration's modules.search_paths (module paths only), an
// environment variable (PRAENORA_MODULE_PATH, PRAENORA_PROVIDER_PATH) and the installation's
// directory.
enum class SearchPathOrigin { caller, configuration, environment, installation };

// "caller", "configuration", "environment" or "installation": the names of the run record's
// execution.manifests (docs/specs/reproducibility.md#run-record).
std::string to_string(SearchPathOrigin origin);

// A search path and where it comes from.
struct SearchPath {
    // As module_search_paths() and provider_search_paths() give it: absolute, lexically normal,
    // without a trailing separator. caller_search_paths() keeps a plain path as given.
    std::filesystem::path path;
    SearchPathOrigin origin = SearchPathOrigin::caller;
    // The path's position among the non-empty entries of its origin, as given, from 0: an entry
    // that repeats an earlier path, and so is not searched again, still counts. nullopt for the
    // installation's directory. caller_search_paths() numbers every path it is given.
    std::optional<std::size_t> index;
};

// The paths of `search_paths`, in order.
std::vector<std::filesystem::path> paths_of(const std::vector<SearchPath>& search_paths);

// `paths` as the caller's search paths, each at its position, unchanged.
std::vector<SearchPath> caller_search_paths(const std::vector<std::filesystem::path>& paths);

// `path` made absolute (relative paths against the current directory), lexically normal and
// without a trailing separator.
std::filesystem::path normalized_search_path(const std::filesystem::path& path);

// Appends `path`, normalized, to `paths` with its origin and index, unless it is empty or `paths`
// already holds it.
void append_search_path(std::vector<SearchPath>& paths, const std::filesystem::path& path,
                        SearchPathOrigin origin, std::optional<std::size_t> index);

// Appends the entries of the environment variable `name`, a path list (':'-separated, ';' on
// Windows; empty entries are skipped), as append_search_path() does, with origin environment and
// each entry's position among the non-empty ones. Nothing when it is not set.
void append_environment_search_paths(std::vector<SearchPath>& paths, const char* name);

// The installation's directory <prefix>/lib/praenora, located from the binary that holds the
// runtime (platform/platform.hpp): <its directory>/praenora, and on Windows, where the runtime DLL
// sits in <prefix>/bin, <its directory>/../lib/praenora. The directory need not exist. nullopt
// when the runtime's binary cannot be located.
std::optional<std::filesystem::path> installation_praenora_dir();

// The manifest files of one search path, in scan order: the path's own files whose names end in
// `suffix` (and are longer than it), then those of each immediate subdirectory, each list sorted
// by UTF-8 file name. A missing path has none. A path that is not a directory, and a directory
// that cannot be read, add a warning to `diagnostics` (NOT_FOUND "<kind> search path ... is not a
// directory", IO_ERROR "cannot read <kind> directory ..."); what was listed until then is kept.
std::vector<std::filesystem::path> manifest_files(const std::filesystem::path& search_path,
                                                  std::string_view suffix, std::string_view kind,
                                                  std::vector<Diagnostic>& diagnostics);

// Where discovery found a manifest, without an absolute path: the origin and index of the search
// path that reached it first, and the manifest file relative to that search path, '/'-separated on
// every platform: "<file>" for a manifest in the search path itself, "<directory>/<file>" for one
// in an immediate subdirectory. The run record keeps it in execution.manifests
// (docs/specs/reproducibility.md#run-record).
struct ManifestLocation {
    SearchPathOrigin origin = SearchPathOrigin::caller;
    std::optional<std::size_t> index; // as SearchPath::index
    std::string file;
};

// The location of `file`, one of the manifest_files() of `search_path`.
ManifestLocation manifest_location(const SearchPath& search_path,
                                   const std::filesystem::path& file);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DISCOVERY_SEARCH_PATHS_HPP
