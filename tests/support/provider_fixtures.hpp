// Helpers for tests that use the fixture service providers built by tests/fixtures/providers:
// where their libraries and manifests are, and copies of them in scratch directories. Header-only;
// uses only the standard library and nlohmann/json.
#ifndef PRAENORA_TESTS_SUPPORT_PROVIDER_FIXTURES_HPP
#define PRAENORA_TESTS_SUPPORT_PROVIDER_FIXTURES_HPP

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>

namespace praenora::test {

// The directory of fixture provider `name` in the build tree, holding its library and manifest.
inline std::filesystem::path fixture_provider_dir(std::string_view name) {
    return utf8_path(PRAENORA_TEST_PROVIDERS_DIR) / utf8_path(name);
}

// The manifest that praenora_add_provider() generated for fixture provider `name`.
inline std::filesystem::path fixture_provider_manifest(std::string_view name) {
    return fixture_provider_dir(name) / utf8_path(std::string(name) + ".provider.json");
}

// Copies the library and the manifest in `from` (a manifest beside its library, named in its
// library.path) into the directory `to` (created when missing), the manifest's JSON changed by
// `edit` when given, and returns the copied manifest's path.
inline std::filesystem::path
copy_with_library(const std::filesystem::path& from, const std::filesystem::path& to,
                  const std::function<void(nlohmann::json&)>& edit = {}) {
    std::ifstream in(from, std::ios::binary);
    nlohmann::json manifest = nlohmann::json::parse(in);
    const std::string library = manifest.at("library").at("path").get<std::string>();
    std::filesystem::create_directories(to);
    std::filesystem::copy_file(from.parent_path() / utf8_path(library), to / utf8_path(library),
                               std::filesystem::copy_options::overwrite_existing);
    if (edit) {
        edit(manifest);
    }
    const std::filesystem::path copied = to / from.filename();
    write_text_file(copied, manifest.dump(2) + "\n");
    return copied;
}

// A copy of fixture provider `name` in `dir`, as copy_with_library() makes it.
inline std::filesystem::path
copy_fixture_provider(std::string_view name, const std::filesystem::path& dir,
                      const std::function<void(nlohmann::json&)>& edit = {}) {
    return copy_with_library(fixture_provider_manifest(name), dir, edit);
}

// A copy of fixture module `name` in `dir`, as copy_with_library() makes it.
inline std::filesystem::path
copy_fixture_module(std::string_view name, const std::filesystem::path& dir,
                    const std::function<void(nlohmann::json&)>& edit = {}) {
    return copy_with_library(fixture_manifest(name), dir, edit);
}

// Appends `bytes` to `file`, which changes its SHA-256 (and nothing a loader looks at when the file
// is not loaded afterwards).
inline void append_to_file(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_PROVIDER_FIXTURES_HPP
