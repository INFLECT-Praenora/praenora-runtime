// What loading a module or a provider library shares: errors located in the library's manifest,
// and the first step of loading, also made during validation: the library file exists and has the
// digest its manifest pins. Nothing here loads a library.
#ifndef PRAENORA_RUNTIME_LOADER_LIBRARY_FILE_HPP
#define PRAENORA_RUNTIME_LOADER_LIBRARY_FILE_HPP

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/status.h>

#include <filesystem>
#include <optional>
#include <string>

namespace praenora::detail {

// Raises errors about what a library belongs to, located in its manifest. Messages start with
// "<owner>: ", for example "module fx-echo@0.1.0: ".
class LocatedFailure {
public:
    LocatedFailure(std::string owner, const std::filesystem::path& manifest_path);

    const std::string& owner() const noexcept;

    // Throws praenora::Error(code) located at `pointer` of the manifest.
    [[noreturn]] void operator()(ErrorCode code, const std::string& message,
                                 const std::string& pointer = "",
                                 std::string details_json = "{}") const;

private:
    std::string owner_;
    std::string file_; // the manifest's path, UTF-8
};

// A status of the ABI with its name, for messages: "11 (FAILED)", or "10001 (module-defined)" for
// a code from PRAENORA_STATUS_MODULE_BASE up.
std::string status_text(praenora_status status);

struct LibraryFile {
    std::string owner; // what the library belongs to, starting the messages: "module fx-echo@0.1.0"
    std::string kind;  // "module" or "provider": messages name "the <kind> library"
    std::filesystem::path manifest_path, library_path;
    std::optional<std::string> pinned_sha256; // the manifest's library.sha256, when it has one
};

// Checks the library file without loading it: it must exist (LIBRARY_NOT_FOUND) and be readable
// (IO_ERROR), both located at /library/path of the manifest, and, when the manifest pins a SHA-256,
// have it (LIBRARY_DIGEST_MISMATCH, located at /library/sha256). Returns the file's SHA-256 (64
// lowercase hex digits). Every error is a praenora::Error located in the manifest whose message
// starts with "<owner>: ".
std::string check_library_file(const LibraryFile& file);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_LIBRARY_FILE_HPP
