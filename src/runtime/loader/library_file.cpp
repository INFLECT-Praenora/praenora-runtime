#include "loader/library_file.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"

#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

LocatedFailure::LocatedFailure(std::string owner, const fs::path& manifest_path)
    : owner_(std::move(owner)), file_(path_to_utf8(manifest_path)) {}

const std::string& LocatedFailure::owner() const noexcept { return owner_; }

void LocatedFailure::operator()(ErrorCode code, const std::string& message,
                                const std::string& pointer, std::string details_json) const {
    SourceLocation location;
    location.file = file_;
    location.pointer = pointer;
    throw Error(code, owner_ + ": " + message, std::move(location), std::move(details_json));
}

std::string status_text(praenora_status status) {
    if (status >= PRAENORA_STATUS_MODULE_BASE) {
        return std::to_string(status) + " (module-defined)";
    }
    return std::to_string(status) + " (" + praenora_status_name(status) + ")";
}

std::string check_library_file(const LibraryFile& file) {
    const LocatedFailure fail(file.owner, file.manifest_path);
    const std::string library = path_to_utf8(file.library_path);
    std::error_code ec;
    if (!fs::is_regular_file(file.library_path, ec)) {
        fail(ErrorCode::LIBRARY_NOT_FOUND,
             "the " + file.kind + " library '" + library + "' does not exist or is not a file",
             "/library/path");
    }
    std::string digest;
    try {
        digest = sha256_file_hex(file.library_path);
    } catch (const Error& error) {
        // Unreadable (for example no permission): located like the other library errors.
        fail(error.code(),
             "cannot hash the " + file.kind + " library: " + std::string(error.what()),
             "/library/path", error.details_json());
    }
    if (file.pinned_sha256 && *file.pinned_sha256 != digest) {
        fail(ErrorCode::LIBRARY_DIGEST_MISMATCH,
             "the " + file.kind + " library '" + library + "' has SHA-256 " + digest +
                 ", but the manifest requires " + *file.pinned_sha256,
             "/library/sha256");
    }
    return digest;
}

} // namespace praenora::detail
