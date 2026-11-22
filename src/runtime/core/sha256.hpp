// SHA-256 (FIPS 180-4), used for library, manifest, catalog and input digests and for the run
// fingerprint. Digests are written as 64 lowercase hexadecimal digits.
#ifndef PRAENORA_RUNTIME_CORE_SHA256_HPP
#define PRAENORA_RUNTIME_CORE_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace praenora::detail {

class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);

    // The digest of everything passed to update(). The object then starts over, as if new.
    std::array<std::uint8_t, 32> finish_digest();
    std::string finish_hex();

private:
    void reset();
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::string sha256_hex(std::string_view data);

// The digest of a file's bytes, read in chunks. Throws praenora::Error(IO_ERROR) if the file cannot
// be read.
std::string sha256_file_hex(const std::filesystem::path& file);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_SHA256_HPP
