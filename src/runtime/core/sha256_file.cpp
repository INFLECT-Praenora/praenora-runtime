// The digest of a file's bytes (core/sha256.hpp), apart from SHA-256 itself (sha256.cpp), which
// needs no other part of the runtime.
#include "core/paths.hpp"
#include "core/sha256.hpp"

#include <praenora/runtime/error.hpp>

#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

namespace praenora::detail {

std::string sha256_file_hex(const std::filesystem::path& file) {
    std::error_code ec;
    std::ifstream in;
    if (!std::filesystem::is_directory(file, ec)) {
        in.open(file, std::ios::binary);
    }
    if (!in.is_open()) {
        throw Error(ErrorCode::IO_ERROR, "cannot open file '" + path_to_utf8(file) + "'");
    }
    std::optional<std::string> digest = sha256_stream_hex(in);
    if (!digest) {
        throw Error(ErrorCode::IO_ERROR, "cannot read file '" + path_to_utf8(file) + "'");
    }
    return *std::move(digest);
}

} // namespace praenora::detail
