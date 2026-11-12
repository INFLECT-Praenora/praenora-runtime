// Praenora Module Manifests (PMM, files named *.pmm.json): what the runtime knows about a module
// before loading any of its code.
#ifndef PRAENORA_RUNTIME_DISCOVERY_MANIFEST_HPP
#define PRAENORA_RUNTIME_DISCOVERY_MANIFEST_HPP

#include "core/semver.hpp"
#include "document/schema.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// A capability id and its exact version, for example test.echo@0.1.0.
struct CapabilityRef {
    std::string id;
    SemVer version;
};

// The manifest's `features`, with the defaults of the module-manifest schema.
struct ModuleFeatures {
    std::string concurrency = "module"; // "module" or "instance"
    bool cancellation = false, diagnostics = false, self_test = false;
};

struct ModuleManifest {
    // Absolute and lexically normal. `directory` holds the manifest; `library_path`,
    // `dependency_paths` and `test_vectors` are resolved against it.
    std::filesystem::path manifest_path, directory, library_path;
    std::string name;
    SemVer version;
    std::uint32_t abi_major = 0, abi_minor = 0;
    SemVer pmp_version, sdk_version;
    std::optional<std::string> library_sha256; // 64 lowercase hex digits, when pinned
    std::vector<std::filesystem::path> dependency_paths;
    std::string os, arch; // platform vocabulary: linux|macos|windows, x86_64|arm64
    std::vector<CapabilityRef> capabilities;
    // The parameter schema as written ({"type": "object"} when the manifest has none) and its
    // compiled form, shared by every copy of the manifest.
    nlohmann::json parameters_schema;
    std::shared_ptr<const CompiledSchema> parameters;
    ModuleFeatures features;
    std::optional<std::filesystem::path> test_vectors;
    std::string manifest_sha256; // SHA-256 of the manifest file's bytes
};

// Reads and checks a manifest. The file is a JSON document (strict JSON, as every *.json file),
// read once: manifest_sha256 is the digest of exactly the bytes parsed. Throws praenora::Error
// with the manifest's file, line, column and JSON pointer:
// - PARSE_ERROR when the file is not JSON (or exceeds the document limits);
// - FORMAT_VERSION_UNSUPPORTED when no module-manifest schema exists for its format_version;
// - MANIFEST_INVALID when its kind is not "module-manifest", it violates the module-manifest
//   schema (unknown top-level fields are allowed only with an "x-" prefix), library.path,
//   a dependency path or tests.vectors is absolute (in POSIX or Windows notation), a capability
//   is listed twice, or parameters_schema is not a valid draft-07 schema;
// - IO_ERROR when the file cannot be read.
// The library itself is not looked at.
ModuleManifest load_manifest(const std::filesystem::path& file);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DISCOVERY_MANIFEST_HPP
