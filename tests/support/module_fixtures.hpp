// Helpers for tests that use the fixture modules built by tests/fixtures/modules: where their
// libraries and manifests are, scratch directories to copy them into (ScratchDir, from
// temp_dir.hpp) and files to write there. Header-only; uses only the standard library.
#ifndef PRAENORA_TESTS_SUPPORT_MODULE_FIXTURES_HPP
#define PRAENORA_TESTS_SUPPORT_MODULE_FIXTURES_HPP

#include "temp_dir.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace praenora::test {

// The directory of fixture module `name` in the build tree, holding its library and manifest.
inline std::filesystem::path fixture_module_dir(std::string_view name) {
    return utf8_path(PRAENORA_TEST_MODULES_DIR) / utf8_path(name);
}

// The manifest that praenora_add_module() generated for fixture module `name`.
inline std::filesystem::path fixture_manifest(std::string_view name) {
    return fixture_module_dir(name) / utf8_path(std::string(name) + ".pmm.json");
}

// Writes `text` to `file` in binary mode, creating its parent directories.
inline void write_text_file(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + file.u8string());
    }
}

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_MODULE_FIXTURES_HPP
