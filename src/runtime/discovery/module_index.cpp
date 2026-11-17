#include "discovery/module_index.hpp"

#include "compat/compat.hpp"
#include "core/paths.hpp"
#include "discovery/search_paths.hpp"

#include <algorithm>
#include <set>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kManifestSuffix = ".pmm.json";

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

void warn(std::vector<Diagnostic>& diagnostics, ErrorCode code, std::string message,
          const fs::path& file) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::warning;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    diagnostic.location.file = path_to_utf8(file);
    diagnostics.push_back(std::move(diagnostic));
}

std::string identity(std::string_view name, const SemVer& version) {
    return std::string(name) + "@" + to_string(version);
}

std::string platform_of(const ModuleManifest& manifest) {
    return manifest.os + "-" + manifest.arch;
}

} // namespace

std::vector<fs::path> module_search_paths(const std::vector<std::string>& explicit_paths,
                                          const std::vector<fs::path>& config_paths,
                                          bool use_environment) {
    std::vector<fs::path> paths;
    for (const auto& text : explicit_paths) {
        append_search_path(paths, path_from_utf8(text));
    }
    for (const auto& path : config_paths) {
        append_search_path(paths, path);
    }
    if (use_environment) {
        append_environment_search_paths(paths, "PRAENORA_MODULE_PATH");
    }
    if (const auto installed = installation_module_dir()) {
        append_search_path(paths, *installed);
    }
    return paths;
}

std::optional<fs::path> installation_module_dir() {
    const auto directory = installation_praenora_dir();
    if (!directory) {
        return std::nullopt;
    }
    return *directory / "modules";
}

ModuleIndex ModuleIndex::scan(const std::vector<fs::path>& search_paths) {
    ModuleIndex index;
    index.search_paths_ = search_paths;
    std::set<fs::path> seen;
    for (const auto& search_path : search_paths) {
        for (const auto& file :
             manifest_files(search_path, kManifestSuffix, "module", index.diagnostics_)) {
            if (!seen.insert(normalized_search_path(file)).second) {
                continue;
            }
            try {
                index.add(load_manifest(file));
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

void ModuleIndex::add(ModuleManifest manifest) {
    for (const auto& earlier : manifests_) {
        if (earlier.name == manifest.name && earlier.version == manifest.version &&
            earlier.os == manifest.os && earlier.arch == manifest.arch) {
            const std::string module = identity(manifest.name, manifest.version);
            warn(diagnostics_, ErrorCode::OK,
                 "module " + module + " for " + platform_of(manifest) + " in " +
                     quoted(manifest.manifest_path) + " is shadowed by " +
                     quoted(earlier.manifest_path) +
                     ", which comes first on the module search path; it is never used",
                 manifest.manifest_path);
            diagnostics_.back().module = module;
            break;
        }
    }
    manifests_.push_back(std::move(manifest));
}

const std::vector<Diagnostic>& ModuleIndex::diagnostics() const { return diagnostics_; }

const ModuleManifest* ModuleIndex::resolve(std::string_view name, const SemVer& version,
                                           Diagnostic* why_not) const {
    const HostPlatform host = host_platform();
    std::vector<const ModuleManifest*> other_platforms, other_versions;
    for (const auto& manifest : manifests_) {
        if (manifest.name != name) {
            continue;
        }
        if (manifest.version != version) {
            other_versions.push_back(&manifest);
        } else if (manifest.os == host.os && manifest.arch == host.arch) {
            return &manifest;
        } else {
            other_platforms.push_back(&manifest);
        }
    }
    if (why_not == nullptr) {
        return nullptr;
    }

    const std::string module = identity(name, version);
    Diagnostic diagnostic;
    diagnostic.severity = Severity::error;
    diagnostic.module = module;
    if (!other_platforms.empty()) {
        std::string found;
        for (const auto* manifest : other_platforms) {
            found += (found.empty() ? "" : ", ") + platform_of(*manifest) + " in " +
                     quoted(manifest->manifest_path);
        }
        diagnostic.code = ErrorCode::PLATFORM_MISMATCH;
        diagnostic.message = "module " + module + " has no manifest for this platform (" + host.os +
                             "-" + host.arch + "); found only " + found;
        diagnostic.location.file = path_to_utf8(other_platforms.front()->manifest_path);
        diagnostic.location.pointer = "/platform";
    } else {
        std::string searched;
        for (const auto& path : search_paths_) {
            searched += (searched.empty() ? "" : ", ") + quoted(path);
        }
        diagnostic.code = ErrorCode::MODULE_NOT_FOUND;
        diagnostic.message = "module " + module + " was not found; searched " +
                             (searched.empty() ? std::string("no paths") : searched);
        if (!other_versions.empty()) {
            std::string versions;
            for (const auto* manifest : other_versions) {
                versions += (versions.empty() ? "" : ", ") + to_string(manifest->version) + " (" +
                            platform_of(*manifest) + ") in " + quoted(manifest->manifest_path);
            }
            diagnostic.message += "; other versions found: " + versions;
        }
    }
    *why_not = std::move(diagnostic);
    return nullptr;
}

std::vector<const ModuleManifest*> ModuleIndex::entries() const {
    std::vector<const ModuleManifest*> entries;
    entries.reserve(manifests_.size());
    for (const auto& manifest : manifests_) {
        entries.push_back(&manifest);
    }
    return entries;
}

const std::vector<fs::path>& ModuleIndex::search_paths() const { return search_paths_; }

} // namespace praenora::detail
