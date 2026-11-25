// The directory digest (digest.hpp): the files are found with std::filesystem, without following
// a symbolic link, and hashed with the runtime's SHA-256 (src/runtime/core/sha256.cpp, compiled
// into the core).
#include "digest.hpp"

#include "core/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora::detail::python {

namespace {

namespace fs = std::filesystem;

std::string quoted(const fs::path& path) { return "'" + path.u8string() + "'"; }

[[noreturn]] void refuse(praenora_status code, const std::string& message) {
    throw DigestError(code, message);
}

std::string file_digest(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::optional<std::string> digest;
    if (in.is_open()) {
        digest = sha256_stream_hex(in);
    }
    if (!digest) {
        refuse(PRAENORA_STATUS_IO_ERROR, "cannot read the file " + quoted(file));
    }
    return *std::move(digest);
}

// Adds the regular files below `directory` to `files`, each as (its path relative to the root,
// its path), skipping the directories named __pycache__ and refusing what the digest does not
// cover. A link is refused before anything is skipped, so a link named __pycache__ is refused, as
// the CMake function refuses it.
void collect(const fs::path& directory, const std::string& relative,
             std::vector<std::pair<std::string, fs::path>>& files) {
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        const std::string name = path.filename().u8string();
        const std::string child = relative.empty() ? name : relative + "/" + name;
        std::error_code status_error;
        const fs::file_status status = it->symlink_status(status_error);
        if (status_error) {
            refuse(PRAENORA_STATUS_IO_ERROR,
                   "cannot read " + quoted(path) + ": " + status_error.message());
        }
        if (fs::is_symlink(status)) {
            refuse(PRAENORA_STATUS_DATA_INVALID,
                   quoted(path) + " is a symbolic link; the digest covers regular files only");
        }
        if (fs::is_directory(status)) {
            if (name != "__pycache__") {
                collect(path, child, files);
            }
        } else if (fs::is_regular_file(status)) {
            files.emplace_back(child, path);
        } else {
            refuse(PRAENORA_STATUS_DATA_INVALID,
                   quoted(path) + " is not a regular file; the digest covers regular files only");
        }
    }
    if (ec) {
        refuse(PRAENORA_STATUS_IO_ERROR,
               "cannot list the directory " + quoted(directory) + ": " + ec.message());
    }
}

} // namespace

std::string directory_digest(const fs::path& root) {
    std::error_code ec;
    const fs::file_status status = fs::status(root, ec);
    if (ec && status.type() != fs::file_type::not_found) {
        refuse(PRAENORA_STATUS_IO_ERROR, "cannot read " + quoted(root) + ": " + ec.message());
    }
    if (!fs::exists(status)) {
        refuse(PRAENORA_STATUS_IO_ERROR, "the directory " + quoted(root) + " does not exist");
    }
    if (!fs::is_directory(status)) {
        refuse(PRAENORA_STATUS_IO_ERROR, quoted(root) + " is not a directory");
    }
    std::vector<std::pair<std::string, fs::path>> files;
    collect(root, std::string(), files);
    // std::string compares as unsigned bytes: the order of the bytes of the UTF-8 paths.
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    Sha256 listing;
    for (const auto& [relative, path] : files) {
        const std::string line = file_digest(path) + "  " + relative + "\n";
        listing.update(line.data(), line.size());
    }
    return listing.finish_hex();
}

} // namespace praenora::detail::python
