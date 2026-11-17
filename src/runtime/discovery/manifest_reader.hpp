// What reading a manifest takes, whatever describes it: module manifests (discovery/manifest.hpp)
// and provider manifests (provider/provider_manifest.hpp) are read the same way up to their own
// fields, and their errors are located the same way.
#ifndef PRAENORA_RUNTIME_DISCOVERY_MANIFEST_READER_HPP
#define PRAENORA_RUNTIME_DISCOVERY_MANIFEST_READER_HPP

#include "core/semver.hpp"
#include "document/yaml_loader.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace praenora::detail {

class ManifestReader {
public:
    // Reads `file` and makes the checks every manifest shares, in this order:
    // - PARSE_ERROR when the file exceeds the document size limit or is not JSON (strict JSON, as
    //   every *.json file); IO_ERROR when it cannot be read. The bytes are read once, so bytes()
    //   are exactly what was parsed;
    // - MANIFEST_INVALID when it is not an object whose `kind` is `kind`, or its format_version is
    //   not a semantic version;
    // - FORMAT_VERSION_UNSUPPORTED when no schema of `kind` exists for its format_version (the
    //   message names the formats this runtime reads);
    // - MANIFEST_INVALID when it violates that schema, located at the first problem, with every
    //   problem in the details ({"issues": [{"pointer", "message"}]}).
    // Errors carry the file, line, column and JSON pointer of the problem; messages start with
    // "<what> <file>: <pointer>: ". `what` names the document ("module manifest"), `plural` the
    // documents ("module manifests").
    ManifestReader(const std::filesystem::path& file, std::string kind, std::string what,
                   std::string plural);

    const std::string& bytes() const noexcept;
    const LoadedDocument& document() const noexcept;
    const nlohmann::json& json() const noexcept;

    // Throws praenora::Error(code) located at `pointer` of the manifest.
    [[noreturn]] void fail(ErrorCode code, const std::string& pointer, const std::string& message,
                           std::string details_json = "{}") const;

    // The string at `pointer`, which the schema has already checked is one.
    const std::string& string_at(const std::string& pointer) const;
    // The semantic version at `pointer`; MANIFEST_INVALID when the text is not a usable one.
    SemVer version_at(const std::string& pointer) const;
    // The integer at `pointer`; MANIFEST_INVALID when it does not fit in 32 bits.
    std::uint32_t uint32_at(const std::string& pointer) const;
    // The path at `pointer`, resolved against `directory` and lexically normal. MANIFEST_INVALID
    // when it is absolute in POSIX or in Windows notation (a manifest may be read on any
    // platform, so both notations are refused everywhere); `what` names the path in the message.
    std::filesystem::path relative_path_at(const std::string& pointer,
                                           const std::filesystem::path& directory,
                                           const char* what) const;

private:
    void check_kind_and_schema() const;

    std::string kind_, what_, plural_;
    std::string bytes_;
    LoadedDocument document_;
};

// `file` made absolute (when possible) and lexically normal: where a manifest says it is.
std::filesystem::path absolute_manifest_path(const std::filesystem::path& file);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DISCOVERY_MANIFEST_READER_HPP
