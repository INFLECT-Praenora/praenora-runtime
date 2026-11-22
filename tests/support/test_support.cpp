#include "test_support.hpp"

namespace praenora::test {

std::filesystem::path utf8_path(std::string_view utf8) {
    return std::filesystem::u8path(utf8.begin(), utf8.end());
}

std::filesystem::path fixture_path(std::string_view relative) {
    const std::filesystem::path base = utf8_path(PRAENORA_TEST_FIXTURES_DIR);
    return relative.empty() ? base : base / utf8_path(relative);
}

} // namespace praenora::test
