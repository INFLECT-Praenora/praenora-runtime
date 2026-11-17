// What module discovery (discovery/module_index.hpp) and provider discovery
// (provider/provider_index.hpp) share: search paths, the installation's directories and the
// manifest files on a search path.
#ifndef PRAENORA_RUNTIME_DISCOVERY_SEARCH_PATHS_HPP
#define PRAENORA_RUNTIME_DISCOVERY_SEARCH_PATHS_HPP

#include <praenora/runtime/error.hpp>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace praenora::detail {

// `path` made absolute (relative paths against the current directory), lexically normal and
// without a trailing separator.
std::filesystem::path normalized_search_path(const std::filesystem::path& path);

// Appends `path`, normalized, to `paths`, unless it is empty or `paths` already holds it.
void append_search_path(std::vector<std::filesystem::path>& paths,
                        const std::filesystem::path& path);

// Appends the entries of the environment variable `name`, a path list (':'-separated, ';' on
// Windows; empty entries are skipped), as append_search_path() does. Nothing when it is not set.
void append_environment_search_paths(std::vector<std::filesystem::path>& paths, const char* name);

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

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DISCOVERY_SEARCH_PATHS_HPP
