// Helpers shared by the test executables. Only the standard library, nlohmann/json and GoogleTest
// are used here, so both white-box and black-box tests can link this library.
//
// The build defines these locations as UTF-8 string literals:
//   PRAENORA_TEST_SOURCE_DIR      repository root
//   PRAENORA_TEST_BINARY_DIR      build tree root
//   PRAENORA_TEST_FIXTURES_DIR    tests/fixtures
//   PRAENORA_TEST_CATALOG_DIR     tests/fixtures/catalog
//   PRAENORA_TEST_MODULES_DIR     fixture modules in the build tree, one directory per module
//                                 holding the library and its manifest (<dir>/fx-echo/...)
//   PRAENORA_TEST_PROVIDERS_DIR   fixture service providers in the build tree, laid out the same
//                                 way (<dir>/test-echo/...)
//   PRAENORA_EXAMPLE_MODULES_DIR  example modules in the build tree, laid out the same way
// With a multi-config generator, these name the directory of the configuration being built.
#ifndef PRAENORA_TESTS_SUPPORT_TEST_SUPPORT_HPP
#define PRAENORA_TESTS_SUPPORT_TEST_SUPPORT_HPP

#include <filesystem>
#include <string_view>

namespace praenora::test {

// Converts a UTF-8 string, the way the build and the public API spell paths, to a path.
std::filesystem::path utf8_path(std::string_view utf8);

// A path below tests/fixtures, given as a relative UTF-8 path with '/' separators.
std::filesystem::path fixture_path(std::string_view relative);

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_TEST_SUPPORT_HPP
