#include "discovery/manifest.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "document/schema_registry.hpp"
#include "document/yaml_loader.hpp"

#include <praenora/runtime/error.hpp>

#include <cstddef>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

constexpr const char* kKind = "module-manifest";

std::string shown(const std::string& pointer) {
    return pointer.empty() ? std::string("(root)") : pointer;
}

[[noreturn]] void fail(ErrorCode code, const LoadedDocument& document, const std::string& pointer,
                       const std::string& message, std::string details_json = "{}") {
    throw Error(code, "module manifest " + document.origin + ": " + shown(pointer) + ": " + message,
                locate(document, pointer), std::move(details_json));
}

bool is_ascii_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// True for a path that is absolute, or names a drive or a network share, in POSIX or Windows
// notation. A manifest may be read on any platform, so both notations are refused everywhere.
bool is_absolute_anywhere(std::string_view text) {
    if (!text.empty() && (text[0] == '/' || text[0] == '\\')) {
        return true;
    }
    if (text.size() >= 2 && is_ascii_letter(text[0]) && text[1] == ':') {
        return true;
    }
    const fs::path path = path_from_utf8(text);
    return path.is_absolute() || path.has_root_name();
}

// The string at `pointer`; the schema has already checked that it is one.
const std::string& string_at(const LoadedDocument& document, const std::string& pointer) {
    return document.json.at(nlohmann::json::json_pointer(pointer)).get_ref<const std::string&>();
}

SemVer version_at(const LoadedDocument& document, const std::string& pointer) {
    const std::string& text = string_at(document, pointer);
    try {
        return parse_semver(text);
    } catch (const Error&) {
        fail(ErrorCode::MANIFEST_INVALID, document, pointer,
             "'" + text + "' is not a semantic version");
    }
}

std::uint32_t uint32_at(const LoadedDocument& document, const std::string& pointer) {
    const auto& value = document.json.at(nlohmann::json::json_pointer(pointer));
    if (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
        fail(ErrorCode::MANIFEST_INVALID, document, pointer,
             "must be an integer from 0 to " +
                 std::to_string(std::numeric_limits<std::uint32_t>::max()));
    }
    return static_cast<std::uint32_t>(value.get<std::uint64_t>());
}

// A path from the manifest, relative to its directory.
fs::path relative_path_at(const LoadedDocument& document, const std::string& pointer,
                          const fs::path& directory, const char* what) {
    const std::string& text = string_at(document, pointer);
    if (is_absolute_anywhere(text)) {
        fail(ErrorCode::MANIFEST_INVALID, document, pointer,
             std::string(what) + " '" + text +
                 "' is absolute; it must be relative to the manifest's directory");
    }
    return (directory / path_from_utf8(text)).lexically_normal();
}

// The schema of the document's format_version; FORMAT_VERSION_UNSUPPORTED when there is none.
const CompiledSchema& schema_for(const LoadedDocument& document) {
    const auto& json = document.json;
    const auto format = json.find("format_version");
    if (format == json.end() || !format->is_string()) {
        fail(ErrorCode::MANIFEST_INVALID, document, format == json.end() ? "" : "/format_version",
             "format_version must be a version string such as \"0.1.0\"");
    }
    const std::string& text = format->get_ref<const std::string&>();
    SemVer version;
    try {
        version = parse_semver(text);
    } catch (const Error&) {
        fail(ErrorCode::MANIFEST_INVALID, document, "/format_version",
             "format_version '" + text + "' is not a semantic version");
    }
    const SchemaRegistry& registry = SchemaRegistry::builtin();
    try {
        return registry.for_kind(kKind, version);
    } catch (const Error& error) {
        if (error.code() != ErrorCode::FORMAT_VERSION_UNSUPPORTED) {
            throw;
        }
    }
    std::string supported;
    for (const auto& [kind, entry_version] : registry.entries()) {
        if (kind == kKind) {
            supported += (supported.empty() ? "" : ", ") + std::to_string(entry_version.major) +
                         "." + std::to_string(entry_version.minor) + ".x";
        }
    }
    fail(ErrorCode::FORMAT_VERSION_UNSUPPORTED, document, "/format_version",
         "format_version " + text + " is not supported; this runtime reads module manifests of " +
             "format " + (supported.empty() ? std::string("(none)") : supported));
}

void check_schema(const LoadedDocument& document) {
    const auto issues = schema_for(document).validate(document.json);
    if (issues.empty()) {
        return;
    }
    nlohmann::json list = nlohmann::json::array();
    for (const auto& issue : issues) {
        list.push_back({{"pointer", issue.pointer}, {"message", issue.message}});
    }
    std::string message = issues.front().message;
    if (issues.size() > 1) {
        message += " (and " + std::to_string(issues.size() - 1) + " more problem" +
                   (issues.size() > 2 ? "s" : "") + ")";
    }
    fail(ErrorCode::MANIFEST_INVALID, document, issues.front().pointer, message,
         nlohmann::json{{"issues", std::move(list)}}.dump());
}

} // namespace

ModuleManifest load_manifest(const fs::path& file) {
    // The bytes are read once, so the digest is of exactly what is parsed. A file beyond the
    // document size limit is refused before it is read.
    check_document_size(file);
    const std::string bytes = read_file_bytes(file);
    const LoadedDocument document = parse_document_text(bytes, path_to_utf8(file));
    const nlohmann::json& json = document.json;

    if (!json.is_object()) {
        fail(ErrorCode::MANIFEST_INVALID, document, "", "a module manifest is a JSON object");
    }
    const auto kind = json.find("kind");
    if (kind == json.end() || *kind != kKind) {
        fail(ErrorCode::MANIFEST_INVALID, document, kind == json.end() ? "" : "/kind",
             std::string("kind must be \"") + kKind + "\"");
    }
    check_schema(document);

    ModuleManifest manifest;
    std::error_code ec;
    fs::path absolute = fs::absolute(file, ec);
    if (ec) {
        absolute = file;
    }
    manifest.manifest_path = absolute.lexically_normal();
    manifest.directory = manifest.manifest_path.parent_path();
    manifest.manifest_sha256 = sha256_hex(bytes);

    manifest.name = json.at("module").at("name").get<std::string>();
    manifest.version = version_at(document, "/module/version");
    manifest.abi_major = uint32_at(document, "/abi/major");
    manifest.abi_minor = uint32_at(document, "/abi/minor");
    manifest.pmp_version = version_at(document, "/pmp_version");
    manifest.sdk_version = version_at(document, "/sdk_version");

    const auto& library = json.at("library");
    manifest.library_path =
        relative_path_at(document, "/library/path", manifest.directory, "library.path");
    if (library.contains("sha256")) {
        manifest.library_sha256 = library.at("sha256").get<std::string>();
    }
    if (library.contains("dependency_paths")) {
        const auto& paths = library.at("dependency_paths");
        for (std::size_t i = 0; i < paths.size(); ++i) {
            manifest.dependency_paths.push_back(
                relative_path_at(document, "/library/dependency_paths/" + std::to_string(i),
                                 manifest.directory, "dependency path"));
        }
    }

    manifest.os = json.at("platform").at("os").get<std::string>();
    manifest.arch = json.at("platform").at("arch").get<std::string>();

    const auto& capabilities = json.at("capabilities");
    for (std::size_t i = 0; i < capabilities.size(); ++i) {
        const std::string pointer = "/capabilities/" + std::to_string(i);
        CapabilityRef capability{capabilities[i].at("id").get<std::string>(),
                                 version_at(document, pointer + "/version")};
        for (std::size_t k = 0; k < manifest.capabilities.size(); ++k) {
            const auto& earlier = manifest.capabilities[k];
            if (earlier.id == capability.id && earlier.version == capability.version) {
                fail(ErrorCode::MANIFEST_INVALID, document, pointer,
                     "capability " + capability.id + "@" + to_string(capability.version) +
                         " is listed twice (also at /capabilities/" + std::to_string(k) + ")");
            }
        }
        manifest.capabilities.push_back(std::move(capability));
    }

    manifest.parameters_schema = json.contains("parameters_schema")
                                     ? json.at("parameters_schema")
                                     : nlohmann::json{{"type", "object"}};
    try {
        manifest.parameters = std::make_shared<const CompiledSchema>(
            manifest.parameters_schema,
            manifest.name + "@" + to_string(manifest.version) + " parameters_schema");
    } catch (const Error& error) {
        fail(ErrorCode::MANIFEST_INVALID, document, "/parameters_schema",
             std::string("not a valid draft-07 schema: ") + error.what());
    }

    if (json.contains("features")) {
        const auto& features = json.at("features");
        manifest.features.concurrency = features.value("concurrency", std::string("module"));
        manifest.features.cancellation = features.value("cancellation", false);
        manifest.features.diagnostics = features.value("diagnostics", false);
        manifest.features.self_test = features.value("self_test", false);
    }
    if (json.contains("tests") && json.at("tests").contains("vectors")) {
        manifest.test_vectors =
            relative_path_at(document, "/tests/vectors", manifest.directory, "tests.vectors");
    }
    return manifest;
}

} // namespace praenora::detail
