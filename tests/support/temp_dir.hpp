// Temporary directories for tests. Header-only; uses only the standard library, so white-box and
// black-box tests alike can use it.
#ifndef PRAENORA_TESTS_SUPPORT_TEMP_DIR_HPP
#define PRAENORA_TESTS_SUPPORT_TEMP_DIR_HPP

#include "test_support.hpp"

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace praenora::test {

// A new, empty directory below the system temporary directory (std::filesystem's
// temp_directory_path(), which honors TMPDIR), removed with its contents when the object is
// destroyed. Its name contains a space and a non-ASCII character, so every test that uses it also
// exercises such paths.
class ScratchDir {
public:
    ScratchDir() {
        std::random_device device;
        path_ = std::filesystem::temp_directory_path() /
                utf8_path("praenora t\xC3\xABst " + std::to_string(device()));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_TEMP_DIR_HPP
