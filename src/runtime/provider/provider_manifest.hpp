// Provider manifests (files named *.provider.json): what the runtime knows about a service provider
// before loading any of its code (docs/specs/runtime-services.md).
#ifndef PRAENORA_RUNTIME_PROVIDER_PROVIDER_MANIFEST_HPP
#define PRAENORA_RUNTIME_PROVIDER_PROVIDER_MANIFEST_HPP

#include "core/semver.hpp"
#include "instance/services.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

struct ProviderManifest {
    // Absolute and lexically normal. `directory` holds the manifest; `library_path` is resolved
    // against it.
    std::filesystem::path manifest_path, directory, library_path;
    std::string name;
    SemVer version;
    SemVer runtime_version;                    // the runtime the provider was built with
    std::uint32_t abi_major = 0, abi_minor = 0; // the provider interface it was built for
    std::optional<std::string> library_sha256;  // 64 lowercase hex digits, when pinned
    std::string os, arch; // platform vocabulary: linux|macos|windows, x86_64|arm64
    std::vector<ServiceVersion> services; // in manifest order; unique by name and major
    nlohmann::json engine;                // a JSON object
    std::string manifest_sha256;          // SHA-256 of the manifest file's bytes
};

// Reads and checks a provider manifest, as load_manifest() reads a module manifest
// (discovery/manifest.hpp): the file is a strict JSON document, read once, so manifest_sha256 is
// the digest of exactly the bytes parsed. Throws praenora::Error with the manifest's file, line,
// column and JSON pointer:
// - PARSE_ERROR when the file is not JSON (or exceeds the document limits);
// - FORMAT_VERSION_UNSUPPORTED when no provider-manifest schema exists for its format_version;
// - MANIFEST_INVALID when its kind is not "provider-manifest", it violates the provider-manifest
//   schema (unknown top-level fields are allowed only with an "x-" prefix), provider.version or
//   runtime_version is not a usable semantic version, abi.major, abi.minor or a service's major or
//   minor does not fit in 32 bits, library.path is absolute (in POSIX or Windows notation), or a
//   service name and major are listed twice;
// - IO_ERROR when the file cannot be read.
// The library itself is not looked at.
ProviderManifest load_provider_manifest(const std::filesystem::path& file);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_PROVIDER_PROVIDER_MANIFEST_HPP
