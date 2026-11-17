// Malformed provider manifests (tests/fixtures/manifests/bad-provider/*.provider.json):
// load_provider_manifest() refuses each one with the error named by its sibling
// <name>.provider.expect.json ({"code": "<ERROR NAME>", "pointer": "<json pointer>",
// "line": <line>}; pointer and line are optional), located in the manifest file. Also files that
// cannot be read or exceed the document size limit.
#include "core/sha256.hpp"
#include "document/yaml_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::load_provider_manifest;
using praenora::test::ScratchDir;

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The bad manifests, sorted by file name.
std::vector<fs::path> bad_manifests() {
    std::vector<fs::path> files;
    const fs::path directory = praenora::test::fixture_path("manifests/bad-provider");
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().u8string();
        if (ends_with(name, ".provider.json") && !ends_with(name, ".expect.json")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// How load_provider_manifest() refuses `file`: its error, or one with code OK when it accepts it.
Error refusal(const fs::path& file) {
    try {
        static_cast<void>(load_provider_manifest(file));
    } catch (const Error& error) {
        return error;
    }
    return Error(ErrorCode::OK, "accepted");
}

} // namespace

TEST(ProviderManifest, BadFixturesFailWithExpectedCode) {
    const auto files = bad_manifests();
    std::vector<std::string> names;
    for (const auto& file : files) {
        names.push_back(file.filename().u8string());
    }
    // The cases this test must cover: one per rule of the reader.
    for (const char* required :
         {"bad-json.provider.json", "not-an-object.provider.json", "wrong-kind.provider.json",
          "format-version-9.provider.json", "format-version-not-semver.provider.json",
          "missing-field.provider.json", "unknown-field.provider.json",
          "engine-not-object.provider.json", "no-services.provider.json",
          "duplicate-service.provider.json", "absolute-library-path.provider.json",
          "absolute-windows-library-path.provider.json",
          "provider-version-too-large.provider.json", "runtime-version-too-large.provider.json",
          "abi-major-too-large.provider.json", "service-minor-too-large.provider.json"}) {
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

        const Error error = refusal(file);
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

TEST(ProviderManifest, MessagesNameTheProviderManifest) {
    const fs::path directory = praenora::test::fixture_path("manifests/bad-provider");
    const Error unsupported = refusal(directory / "format-version-9.provider.json");
    EXPECT_TRUE(contains(unsupported.what(), "provider manifest ")) << unsupported.what();
    // The formats this runtime reads are named.
    EXPECT_TRUE(
        contains(unsupported.what(), "this runtime reads provider manifests of format 0.1.x"))
        << unsupported.what();

    const Error duplicate = refusal(directory / "duplicate-service.provider.json");
    EXPECT_TRUE(contains(duplicate.what(), "test.echo@0 is listed twice (also at /services/0)"))
        << duplicate.what();

    // Every problem the schema finds is in the details.
    const Error missing = refusal(directory / "missing-field.provider.json");
    const auto details = nlohmann::json::parse(missing.details_json());
    ASSERT_TRUE(details.contains("issues")) << missing.to_json();
    EXPECT_FALSE(details.at("issues").empty());
}

TEST(ProviderManifest, UnreadableOrOversizedFilesAreRefused) {
    const ScratchDir scratch;
    const Error missing = refusal(scratch.path() / "absent.provider.json");
    EXPECT_EQ(missing.code(), ErrorCode::IO_ERROR) << missing.to_json();

    // Beyond 16 MiB: refused before it is read.
    const fs::path large = scratch.path() / "large.provider.json";
    {
        std::ofstream out(large, std::ios::binary);
        out << "{\"x-padding\": \"";
        out << std::string(praenora::detail::kMaxDocumentBytes, 'a');
        out << "\"}";
    }
    const Error oversized = refusal(large);
    EXPECT_EQ(oversized.code(), ErrorCode::PARSE_ERROR) << oversized.to_json();
    EXPECT_TRUE(ends_with(oversized.location().file, "large.provider.json"))
        << oversized.to_json();
}

// A valid manifest: every field is read; paths are absolute and resolved against the manifest's
// directory; the digest is that of the file's bytes.
TEST(ProviderManifest, ReadsEveryField) {
    const ScratchDir scratch;
    const fs::path file = scratch.path() / "nested" / "test-echo.provider.json";
    const std::string text = R"({
  "kind": "provider-manifest",
  "format_version": "0.1.7",
  "provider": { "name": "test-echo", "version": "1.2.3-rc.1+build.5" },
  "runtime_version": "0.2.0",
  "abi": { "major": 0, "minor": 3 },
  "library": {
    "path": "lib/../test-echo.so",
    "sha256": "abababababababababababababababababababababababababababababababab"
  },
  "platform": { "os": "linux", "arch": "arm64" },
  "services": [
    { "name": "test.echo", "major": 0, "minor": 2 },
    { "name": "test.echo", "major": 1, "minor": 0 },
    { "name": "praenora.simulation", "major": 0, "minor": 0 }
  ],
  "engine": { "name": "echo", "version": "1.0.0", "options": { "fast": false } },
  "x-build": { "commit": "local" }
}
)";
    praenora::test::write_text_file(file, text);

    const auto manifest = load_provider_manifest(file);
    EXPECT_TRUE(manifest.manifest_path.is_absolute());
    EXPECT_EQ(manifest.directory, manifest.manifest_path.parent_path());
    EXPECT_EQ(manifest.library_path, (manifest.directory / "test-echo.so").lexically_normal());
    EXPECT_EQ(manifest.name, "test-echo");
    EXPECT_EQ(praenora::detail::to_string(manifest.version), "1.2.3-rc.1+build.5");
    EXPECT_EQ(praenora::detail::to_string(manifest.runtime_version), "0.2.0");
    EXPECT_EQ(manifest.abi_major, 0u);
    EXPECT_EQ(manifest.abi_minor, 3u);
    EXPECT_EQ(manifest.library_sha256,
              std::string("abababababababababababababababababababababababababababababababab"));
    EXPECT_EQ(manifest.os, "linux");
    EXPECT_EQ(manifest.arch, "arm64");
    ASSERT_EQ(manifest.services.size(), 3u);
    EXPECT_EQ(praenora::detail::to_string(manifest.services[0]), "test.echo@0.2");
    EXPECT_EQ(praenora::detail::to_string(manifest.services[1]), "test.echo@1.0");
    EXPECT_EQ(praenora::detail::to_string(manifest.services[2]), "praenora.simulation@0.0");
    const auto engine =
        nlohmann::json::parse(R"({"name":"echo","version":"1.0.0","options":{"fast":false}})");
    EXPECT_EQ(manifest.engine, engine);
    EXPECT_EQ(manifest.manifest_sha256, praenora::detail::sha256_hex(text));
}
