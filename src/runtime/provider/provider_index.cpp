#include "provider/provider_index.hpp"

#include "compat/compat.hpp"
#include "core/paths.hpp"
#include "discovery/search_paths.hpp"
#include "provider/provider_abi.h"

#include <praenora/runtime/version.hpp>

#include <nlohmann/json.hpp>

#include <set>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kManifestSuffix = ".provider.json";

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

std::string platform_of(const ProviderManifest& manifest) {
    return manifest.os + "-" + manifest.arch;
}

// The services of `manifest` named `name`, as "<name>@<major>.<minor>", joined.
std::string offered(const ProviderManifest& manifest, const std::string& name) {
    std::string text;
    for (const auto& service : manifest.services) {
        if (service.name == name) {
            text += (text.empty() ? "" : ", ") + to_string(service);
        }
    }
    return text;
}

} // namespace

std::vector<SearchPath> provider_search_paths(const std::vector<std::string>& explicit_paths,
                                              bool use_environment) {
    std::vector<SearchPath> paths;
    std::size_t index = 0; // among the caller's non-empty paths
    for (const auto& text : explicit_paths) {
        if (!text.empty()) {
            append_search_path(paths, path_from_utf8(text), SearchPathOrigin::caller, index++);
        }
    }
    if (use_environment) {
        append_environment_search_paths(paths, "PRAENORA_PROVIDER_PATH");
    }
    if (const auto installed = installation_provider_dir()) {
        append_search_path(paths, *installed, SearchPathOrigin::installation, std::nullopt);
    }
    return paths;
}

std::optional<fs::path> installation_provider_dir() {
    const auto directory = installation_praenora_dir();
    if (!directory) {
        return std::nullopt;
    }
    return *directory / "providers";
}

std::string shadowed_reason(const fs::path& shadowed_by) {
    return "shadowed by " + quoted(shadowed_by) +
           ", which comes first on the provider search path; it is never used";
}

std::optional<std::string> provider_problem(const ProviderManifest& manifest) {
    const HostPlatform host = host_platform();
    if (manifest.os != host.os || manifest.arch != host.arch) {
        return "built for " + platform_of(manifest) + "; this host is " + host.os + "-" +
               host.arch;
    }
    const std::string runtime = version_info().runtime;
    if (manifest.runtime_version != parse_semver(runtime)) {
        return "built for runtime " + to_string(manifest.runtime_version) + "; this runtime is " +
               runtime;
    }
    if (manifest.abi_major != PRAENORA_PROVIDER_ABI_MAJOR) {
        return "built for provider interface " + std::to_string(manifest.abi_major) + "." +
               std::to_string(manifest.abi_minor) + "; this runtime implements " +
               std::to_string(PRAENORA_PROVIDER_ABI_MAJOR) + "." +
               std::to_string(PRAENORA_PROVIDER_ABI_MINOR) + " and accepts only the same major";
    }
    return std::nullopt;
}

ProviderIndex ProviderIndex::scan(const std::vector<fs::path>& search_paths) {
    return scan(caller_search_paths(search_paths));
}

ProviderIndex ProviderIndex::scan(const std::vector<SearchPath>& search_paths) {
    ProviderIndex index;
    index.search_paths_ = paths_of(search_paths);
    std::set<fs::path> seen;
    for (const auto& search_path : search_paths) {
        for (const auto& file :
             manifest_files(search_path.path, kManifestSuffix, "provider", index.diagnostics_)) {
            if (!seen.insert(normalized_search_path(file)).second) {
                continue;
            }
            try {
                ProviderManifest manifest = load_provider_manifest(file);
                manifest.location = manifest_location(search_path, file);
                index.add(std::move(manifest));
            } catch (const Error& error) {
                Diagnostic diagnostic;
                diagnostic.severity = Severity::warning;
                diagnostic.code = error.code();
                diagnostic.message = std::string(error.what()) + " (manifest skipped)";
                diagnostic.location = error.location();
                if (diagnostic.location.file.empty()) {
                    diagnostic.location.file = path_to_utf8(file);
                }
                index.diagnostics_.push_back(std::move(diagnostic));
            }
        }
    }
    return index;
}

void ProviderIndex::add(ProviderManifest manifest) {
    IndexedProvider entry;
    entry.problem = provider_problem(manifest);
    for (const auto& earlier : providers_) {
        const ProviderManifest& first = earlier.manifest;
        if (first.name == manifest.name && first.os == manifest.os &&
            first.arch == manifest.arch) {
            Diagnostic warning;
            warning.severity = Severity::warning;
            warning.code = ErrorCode::OK;
            warning.message = "provider " + manifest.name + " for " + platform_of(manifest) +
                              " in " + quoted(manifest.manifest_path) + " is shadowed by " +
                              quoted(first.manifest_path) +
                              ", which comes first on the provider search path; it is never used";
            warning.location.file = path_to_utf8(manifest.manifest_path);
            diagnostics_.push_back(std::move(warning));
            entry.shadowed_by = first.manifest_path;
            break;
        }
    }
    entry.manifest = std::move(manifest);
    providers_.push_back(std::move(entry));
}

const std::vector<Diagnostic>& ProviderIndex::diagnostics() const { return diagnostics_; }

const std::vector<IndexedProvider>& ProviderIndex::entries() const { return providers_; }

ServiceResolution ProviderIndex::resolve(const ServiceVersion& requirement) const {
    ServiceResolution resolution;
    for (const auto& entry : providers_) {
        const ProviderManifest& manifest = entry.manifest;
        const ServiceVersion* match = nullptr;
        bool named = false;
        for (const auto& service : manifest.services) {
            if (service.name == requirement.name) {
                named = true;
                if (service.major == requirement.major) {
                    match = &service;
                }
            }
        }
        if (!named) {
            continue;
        }
        std::string reason;
        if (entry.shadowed_by) {
            reason = shadowed_reason(*entry.shadowed_by);
        } else if (entry.problem) {
            reason = *entry.problem;
        } else if (match == nullptr) {
            reason = "offers " + offered(manifest, requirement.name) + " only";
        } else if (match->minor < requirement.minor) {
            reason = "offers " + to_string(*match) + "; " + requirement.name + "@" +
                     std::to_string(requirement.major) + "." + std::to_string(requirement.minor) +
                     " or a later minor is required";
        } else {
            resolution.provider = &manifest;
            resolution.offered_minor = match->minor;
            resolution.rejected.clear();
            return resolution;
        }
        resolution.rejected.push_back({&manifest, std::move(reason)});
    }
    return resolution;
}

std::string ProviderIndex::unresolved_details(const ServiceVersion& requirement,
                                              const ServiceResolution& resolution) const {
    nlohmann::json paths = nlohmann::json::array();
    for (const auto& path : search_paths_) {
        paths.push_back(path_to_utf8(path));
    }
    nlohmann::json providers = nlohmann::json::array();
    for (const auto& rejected : resolution.rejected) {
        providers.push_back({{"name", rejected.provider->name},
                             {"version", to_string(rejected.provider->version)},
                             {"manifest", path_to_utf8(rejected.provider->manifest_path)},
                             {"reason", rejected.reason}});
    }
    return nlohmann::json{{"service", requirement.name},
                          {"major", requirement.major},
                          {"minor", requirement.minor},
                          {"search_paths", std::move(paths)},
                          {"providers", std::move(providers)}}
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

const std::vector<fs::path>& ProviderIndex::search_paths() const { return search_paths_; }

} // namespace praenora::detail
