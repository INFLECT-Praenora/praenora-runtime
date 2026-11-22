// Malformed module manifests (tests/fixtures/manifests/bad/*.pmm.json): load_manifest() refuses
// each one with the error named by its sibling <name>.pmm.expect.json
// ({"code": "<ERROR NAME>", "pointer": "<json pointer>", "line": <line>}; pointer and line are
// optional), located in the manifest file.
#include "discovery/manifest.hpp"
#include "document/yaml_loader.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::load_manifest;

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The bad manifests, sorted by file name.
std::vector<fs::path> bad_manifests() {
    std::vector<fs::path> files;
    const fs::path directory = praenora::test::fixture_path("manifests/bad");
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (ends_with(entry.path().filename().u8string(), ".pmm.json")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST(Manifest, BadFixturesFailWithExpectedCode) {
    const auto files = bad_manifests();
    std::vector<std::string> names;
    for (const auto& file : files) {
        names.push_back(file.filename().u8string());
    }
    // The cases this test must cover.
    for (const char* required : {"bad-json.pmm.json", "missing-field.pmm.json",
                                 "absolute-library-path.pmm.json", "format-version-9.pmm.json",
                                 "invalid-parameters-schema.pmm.json", "unknown-field.pmm.json"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), required), names.end()) << required;
    }

    for (const auto& file : files) {
        SCOPED_TRACE(file.u8string());
        fs::path expect_path = file;
        expect_path.replace_extension(".expect.json");
        ASSERT_TRUE(fs::exists(expect_path)) << expect_path.u8string();
        const nlohmann::json expect = praenora::detail::load_document_file(expect_path).json;
        const auto code = praenora::error_code_from_name(expect.at("code").get<std::string>());
        ASSERT_TRUE(code.has_value()) << expect.dump();

        try {
            (void)load_manifest(file);
            ADD_FAILURE() << "load_manifest accepted the manifest";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), *code) << error.to_json();
            EXPECT_TRUE(ends_with(error.location().file, file.filename().u8string()))
                << error.to_json();
            if (expect.contains("pointer")) {
                EXPECT_EQ(error.location().pointer, expect.at("pointer").get<std::string>())
                    << error.to_json();
            }
            if (expect.contains("line")) {
                EXPECT_EQ(error.location().line, expect.at("line").get<std::int64_t>())
                    << error.to_json();
            }
            EXPECT_FALSE(std::string(error.what()).empty());
        }
    }
}
