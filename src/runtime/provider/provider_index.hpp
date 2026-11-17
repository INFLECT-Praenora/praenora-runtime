// Provider discovery: the provider search paths, the index of the provider manifests found on them,
// whether each provider is usable, and the resolution of a required service to one provider
// (docs/specs/runtime-services.md).
#ifndef PRAENORA_RUNTIME_PROVIDER_PROVIDER_INDEX_HPP
#define PRAENORA_RUNTIME_PROVIDER_PROVIDER_INDEX_HPP

#include "discovery/search_paths.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// The provider search paths, in the order they are searched:
//   1. `explicit_paths` (the caller's provider paths, UTF-8; relative ones against the current
//      directory), in order;
//   2. PRAENORA_PROVIDER_PATH, when `use_environment` (':'-separated, ';' on Windows; empty
//      entries are skipped, relative ones are relative to the current directory);
//   3. installation_provider_dir().
// Every path is made absolute and lexically normal, without a trailing separator; a path that
// repeats an earlier one is dropped. Each keeps its origin (caller, environment, installation) and
// its index among its origin's non-empty entries (SearchPath).
std::vector<SearchPath> provider_search_paths(const std::vector<std::string>& explicit_paths,
                                              bool use_environment);

// The installation's provider directory, <prefix>/lib/praenora/providers: the sibling of the
// module directory (installation_module_dir()), located the same way. nullopt when the runtime's
// binary cannot be located.
std::optional<std::filesystem::path> installation_provider_dir();

// Why this runtime cannot use the provider, or nullopt when it can. A provider is usable when its
// manifest is for the host platform, its runtime_version equals this runtime's version (SemVer
// equality: build metadata ignored) and its abi.major equals PRAENORA_PROVIDER_ABI_MAJOR (any
// minor). The rules are checked in that order; the first one broken is the reason.
std::optional<std::string> provider_problem(const ProviderManifest& manifest);

// One manifest of the index.
struct IndexedProvider {
    ProviderManifest manifest;
    std::optional<std::string> problem; // provider_problem(manifest)
    // When an earlier manifest of the same name for the same platform came first on the search
    // path, that manifest's path: this one is never used.
    std::optional<std::filesystem::path> shadowed_by;
};

// Why a shadowed manifest is not used: "shadowed by '<shadowed_by>', which comes first on the
// provider search path; it is never used". The provider listings and resolve()'s reasons give it.
std::string shadowed_reason(const std::filesystem::path& shadowed_by);

// What resolve() found for a requirement.
struct ServiceResolution {
    // A provider that came close but cannot satisfy the requirement, and why.
    struct Rejected {
        const ProviderManifest* provider = nullptr;
        std::string reason;
    };

    const ProviderManifest* provider = nullptr; // the provider that satisfies it, or nullptr
    std::uint32_t offered_minor = 0;            // the minor it offers, when resolved
    // When unresolved: every indexed provider, in search order, that offers a service of the
    // required name but is unusable, shadowed, offers other majors only or a lower minor.
    std::vector<Rejected> rejected;
};

// The valid provider manifests on a list of search paths.
class ProviderIndex {
public:
    // Scans each path, then its immediate subdirectories, for files named *.provider.json: first
    // the path's own files, then each subdirectory's, everything in sorted (UTF-8 byte) order.
    // Missing paths are skipped silently; an existing path that is not a directory gets a warning
    // (NOT_FOUND). Every manifest is loaded with load_provider_manifest():
    // - an invalid manifest becomes a warning diagnostic carrying its error code and location,
    //   and is skipped;
    // - a manifest whose name and platform repeat an earlier one is indexed but shadowed: it is
    //   never used, and gets a warning (code OK) naming both paths;
    // - manifests for other platforms are indexed too (unusable) and shadow nothing.
    // A manifest file reached twice (overlapping search paths) is indexed once. Each indexed
    // manifest's `location` says where it was found: the search path that reached it first.
    static ProviderIndex scan(const std::vector<SearchPath>& search_paths);
    // The same over `search_paths` taken as the caller's paths (caller_search_paths()).
    static ProviderIndex scan(const std::vector<std::filesystem::path>& search_paths);

    // Warnings found while scanning, in the order found. A shadowed manifest's warning carries
    // ErrorCode::OK.
    const std::vector<Diagnostic>& diagnostics() const;

    // Every indexed manifest, in scan order, shadowed ones included.
    const std::vector<IndexedProvider>& entries() const;

    // The provider of `requirement`: the first indexed provider, in search order, that is usable,
    // not shadowed, and offers requirement.name at requirement.major with a minor of at least
    // requirement.minor. The pointers stay valid as long as this index exists.
    ServiceResolution resolve(const ServiceVersion& requirement) const;

    // The details of a SERVICE_UNAVAILABLE error for a requirement that did not resolve, as a JSON
    // object text: {"service", "major", "minor", "search_paths": [...], "providers": [{"name",
    // "version", "manifest", "reason"}]}, the providers being resolution.rejected.
    std::string unresolved_details(const ServiceVersion& requirement,
                                   const ServiceResolution& resolution) const;

    // The paths given to scan().
    const std::vector<std::filesystem::path>& search_paths() const;

private:
    void add(ProviderManifest manifest);

    std::vector<std::filesystem::path> search_paths_;
    std::vector<IndexedProvider> providers_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_PROVIDER_PROVIDER_INDEX_HPP
