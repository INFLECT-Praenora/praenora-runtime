#include "discovery/manifest_reader.hpp"

#include "core/paths.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"

#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

std::string shown(const std::string& pointer) {
    return pointer.empty() ? std::string("(root)") : pointer;
}

bool is_ascii_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// True for a path that is absolute, or names a drive or a network share, in POSIX or Windows
// notation.
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

// The bytes of `file`, read once; a file beyond the document size limit is refused before it is
// read.
std::string read_manifest_bytes(const fs::path& file) {
    check_document_size(file);
    return read_file_bytes(file);
}

} // namespace

ManifestReader::ManifestReader(const fs::path& file, std::string kind, std::string what,
                               std::string plural)
    : kind_(std::move(kind)), what_(std::move(what)), plural_(std::move(plural)),
      bytes_(read_manifest_bytes(file)),
      document_(parse_document_text(bytes_, path_to_utf8(file))) {
    check_kind_and_schema();
}

const std::string& ManifestReader::bytes() const noexcept { return bytes_; }

const LoadedDocument& ManifestReader::document() const noexcept { return document_; }

const nlohmann::json& ManifestReader::json() const noexcept { return document_.json; }

void ManifestReader::fail(ErrorCode code, const std::string& pointer, const std::string& message,
                          std::string details_json) const {
    throw Error(code, what_ + " " + document_.origin + ": " + shown(pointer) + ": " + message,
                locate(document_, pointer), std::move(details_json));
}

const std::string& ManifestReader::string_at(const std::string& pointer) const {
    return json().at(nlohmann::json::json_pointer(pointer)).get_ref<const std::string&>();
}

SemVer ManifestReader::version_at(const std::string& pointer) const {
    const std::string& text = string_at(pointer);
    try {
        return parse_semver(text);
    } catch (const Error&) {
        fail(ErrorCode::MANIFEST_INVALID, pointer, "'" + text + "' is not a semantic version");
    }
}

std::uint32_t ManifestReader::uint32_at(const std::string& pointer) const {
    const auto value = uint32_of(json().at(nlohmann::json::json_pointer(pointer)));
    if (!value) {
        fail(ErrorCode::MANIFEST_INVALID, pointer,
             "must be an integer from 0 to " +
                 std::to_string(std::numeric_limits<std::uint32_t>::max()));
    }
    return *value;
}

fs::path ManifestReader::relative_path_at(const std::string& pointer, const fs::path& directory,
                                          const char* what) const {
    const std::string& text = string_at(pointer);
    if (is_absolute_anywhere(text)) {
        fail(ErrorCode::MANIFEST_INVALID, pointer,
             std::string(what) + " '" + text +
                 "' is absolute; it must be relative to the manifest's directory");
    }
    return (directory / path_from_utf8(text)).lexically_normal();
}

void ManifestReader::check_kind_and_schema() const {
    const nlohmann::json& root = json();
    if (!root.is_object()) {
        fail(ErrorCode::MANIFEST_INVALID, "", "a " + what_ + " is a JSON object");
    }
    const auto kind = root.find("kind");
    if (kind == root.end() || *kind != kind_) {
        fail(ErrorCode::MANIFEST_INVALID, kind == root.end() ? "" : "/kind",
             "kind must be \"" + kind_ + "\"");
    }

    // The schema of the document's format_version; FORMAT_VERSION_UNSUPPORTED when there is none.
    const auto format = root.find("format_version");
    if (format == root.end() || !format->is_string()) {
        fail(ErrorCode::MANIFEST_INVALID, format == root.end() ? "" : "/format_version",
             "format_version must be a version string such as \"0.1.0\"");
    }
    const std::string& text = format->get_ref<const std::string&>();
    SemVer version;
    try {
        version = parse_semver(text);
    } catch (const Error&) {
        fail(ErrorCode::MANIFEST_INVALID, "/format_version",
             "format_version '" + text + "' is not a semantic version");
    }
    const SchemaRegistry& registry = SchemaRegistry::builtin();
    const CompiledSchema* schema = nullptr;
    try {
        schema = &registry.for_kind(kind_, version);
    } catch (const Error& error) {
        if (error.code() != ErrorCode::FORMAT_VERSION_UNSUPPORTED) {
            throw;
        }
    }
    if (schema == nullptr) {
        std::string supported;
        for (const auto& [entry_kind, entry_version] : registry.entries()) {
            if (entry_kind == kind_) {
                supported += (supported.empty() ? "" : ", ") +
                             std::to_string(entry_version.major) + "." +
                             std::to_string(entry_version.minor) + ".x";
            }
        }
        fail(ErrorCode::FORMAT_VERSION_UNSUPPORTED, "/format_version",
             "format_version " + text + " is not supported; this runtime reads " + plural_ +
                 " of format " + (supported.empty() ? std::string("(none)") : supported));
    }

    const auto issues = schema->validate(root);
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
    fail(ErrorCode::MANIFEST_INVALID, issues.front().pointer, message,
         nlohmann::json{{"issues", std::move(list)}}.dump());
}

fs::path absolute_manifest_path(const fs::path& file) {
    std::error_code ec;
    fs::path absolute = fs::absolute(file, ec);
    if (ec) {
        absolute = file;
    }
    return absolute.lexically_normal();
}

} // namespace praenora::detail
