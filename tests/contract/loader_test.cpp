// The loader: shared libraries, the entry point, function-table validation, the describe()
// identity cross-check and library digests, with the fixture modules.
#include "core/sha256.hpp"
#include "discovery/manifest.hpp"
#include "document/yaml_loader.hpp"
#include "loader/loaded_module.hpp"
#include "loader/shared_library.hpp"
#include "platform/platform.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/abi.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::check_library_file;
using praenora::detail::load_manifest;
using praenora::detail::LoadedModule;
using praenora::detail::ModuleManifest;
using praenora::detail::SharedLibrary;
using praenora::test::fixture_manifest;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;

namespace {

std::shared_ptr<LoadedModule> load_fixture(std::string_view name) {
    return LoadedModule::load(load_manifest(fixture_manifest(name)));
}

struct LoadFailure {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    praenora::SourceLocation location;
};

// How loading `manifest` fails; code OK if it loads.
LoadFailure load_failure(const ModuleManifest& manifest) {
    try {
        static_cast<void>(LoadedModule::load(manifest));
    } catch (const Error& error) {
        return LoadFailure{error.code(), error.what(), error.location()};
    }
    return LoadFailure{};
}

LoadFailure fixture_load_failure(std::string_view name) {
    return load_failure(load_manifest(fixture_manifest(name)));
}

// How check_library_file() refuses `manifest`; code OK if it accepts it.
LoadFailure library_check_failure(const ModuleManifest& manifest) {
    try {
        static_cast<void>(check_library_file(manifest));
    } catch (const Error& error) {
        return LoadFailure{error.code(), error.what(), error.location()};
    }
    return LoadFailure{};
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Copies the library and the manifest of fixture module `name` into `directory` and returns the
// copied manifest's path. The manifest names its library relative to itself, so the copy works.
fs::path copy_fixture(std::string_view name, const fs::path& directory) {
    const ModuleManifest manifest = load_manifest(fixture_manifest(name));
    fs::create_directories(directory);
    const auto options = fs::copy_options::overwrite_existing;
    fs::copy_file(manifest.library_path, directory / manifest.library_path.filename(), options);
    const fs::path copied = directory / manifest.manifest_path.filename();
    fs::copy_file(manifest.manifest_path, copied, options);
    return copied;
}

// Rewrites the manifest at `path` after `edit` has changed its JSON.
template <class Edit> void edit_manifest(const fs::path& path, Edit edit) {
    nlohmann::json json = praenora::detail::load_document_file(path).json;
    edit(json);
    praenora::test::write_text_file(path, json.dump(2));
}

// A host API without callbacks: enough for modules that need none.
praenora_host_api bare_host() {
    praenora_host_api host{};
    host.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
    host.abi_major = PRAENORA_ABI_MAJOR;
    host.abi_minor = PRAENORA_ABI_MINOR;
    return host;
}

praenora_status PRAENORA_CALL append_to_string(void* context, const char* data, std::size_t size) {
    static_cast<std::string*>(context)->append(data, size);
    return PRAENORA_STATUS_OK;
}

// Sets an environment variable (or removes it, for NULL) and restores its earlier value when
// destroyed.
class ScopedVariable {
public:
    ScopedVariable(const char* name, const char* value)
        : name_(name), previous_(praenora::detail::environment_variable(name)) {
        set(value);
    }
    ~ScopedVariable() { set(previous_ ? previous_->c_str() : nullptr); }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    void set(const char* value) const {
#ifdef _WIN32
        SetEnvironmentVariableA(name_, value);
#else
        if (value != nullptr) {
            setenv(name_, value, 1);
        } else {
            unsetenv(name_);
        }
#endif
    }

    const char* name_;
    std::optional<std::string> previous_;
};

// Whether the environment asks for every library to stay loaded (some sanitizer test runs do).
bool keep_libraries_requested() {
    return praenora::detail::environment_variable("PRAENORA_DEBUG_KEEP_LIBRARIES") ==
           std::optional<std::string>("1");
}

} // namespace

TEST(Loader, LoadsMinimalCModule) {
    const auto module = load_fixture("fx-minimal-c");
    ASSERT_NE(module, nullptr);
    const praenora_module_api_v0& api = module->api();
    EXPECT_EQ(api.header.abi_major, 0u);
    EXPECT_EQ(api.header.reserved, 0u);
    EXPECT_GE(api.header.struct_size, PRAENORA_MODULE_API_V0_BASE_SIZE);
    EXPECT_NE(api.describe, nullptr);
    EXPECT_NE(api.create, nullptr);
    EXPECT_NE(api.initialize, nullptr);
    EXPECT_NE(api.invoke, nullptr);
    EXPECT_NE(api.finalize, nullptr);
    EXPECT_NE(api.destroy, nullptr);
    EXPECT_EQ(api.diagnostics, nullptr);
    EXPECT_EQ(api.self_test, nullptr);

    EXPECT_EQ(module->describe().at("name"), "fx-minimal-c");
    EXPECT_EQ(module->describe().at("version"), "0.1.0");
    EXPECT_EQ(module->manifest().name, "fx-minimal-c");
    EXPECT_EQ(module->library_sha256(),
              praenora::detail::sha256_file_hex(module->manifest().library_path));
    EXPECT_EQ(module->pinned(), keep_libraries_requested());
}

TEST(Loader, LoadsCppAdapterModule) {
    const auto module = load_fixture("fx-echo");
    const nlohmann::json& describe = module->describe();
    EXPECT_EQ(describe.at("name"), "fx-echo");
    EXPECT_EQ(describe.at("version"), "0.1.0");
    EXPECT_EQ(describe.at("abi"), (nlohmann::json{{"major", 0}, {"minor", 0}}));
    EXPECT_EQ(describe.at("sdk_version"), "0.1.0");
    EXPECT_EQ(describe.at("pmp_version"), "0.1.0");
    EXPECT_EQ(describe.at("capabilities"),
              nlohmann::json::parse(R"([{"id":"test.echo","version":"0.1.0"}])"));
    EXPECT_NE(module->api().diagnostics, nullptr);
    EXPECT_EQ(module->api().self_test, nullptr);

    // The table works: describe() through it gives the same document.
    std::string text;
    praenora_sink sink{&text, &append_to_string};
    ASSERT_EQ(module->api().describe(&sink), PRAENORA_STATUS_OK);
    EXPECT_EQ(nlohmann::json::parse(text), describe);
}

TEST(Loader, RejectsMissingEntryPoint) {
    const auto failure = fixture_load_failure("fx-no-entry");
    EXPECT_EQ(failure.code, ErrorCode::ENTRY_POINT_MISSING) << failure.message;
    EXPECT_TRUE(contains(failure.message, "praenora_module_get_api")) << failure.message;
    EXPECT_TRUE(contains(failure.location.file, "fx-no-entry.pmm.json"));
}

TEST(Loader, RejectsNullTable) {
    const auto failure = fixture_load_failure("fx-null-table");
    EXPECT_EQ(failure.code, ErrorCode::API_TABLE_INVALID) << failure.message;
    EXPECT_TRUE(contains(failure.message, "NULL")) << failure.message;
}

TEST(Loader, RejectsAbiMajorMismatch) {
    const auto failure = fixture_load_failure("fx-abi-major");
    EXPECT_EQ(failure.code, ErrorCode::ABI_INCOMPATIBLE) << failure.message;
    EXPECT_TRUE(contains(failure.message, "7")) << failure.message;
}

TEST(Loader, RejectsShortTable) {
    const auto failure = fixture_load_failure("fx-short-table");
    EXPECT_EQ(failure.code, ErrorCode::API_TABLE_INVALID) << failure.message;
    EXPECT_TRUE(contains(failure.message, "struct_size")) << failure.message;
}

namespace {

praenora_status PRAENORA_CALL stub_diagnostics(praenora_instance*, praenora_sink*) {
    return PRAENORA_STATUS_OK;
}

praenora_status PRAENORA_CALL stub_self_test(const praenora_host_api*, praenora_sink*) {
    return PRAENORA_STATUS_OK;
}

} // namespace

TEST(Loader, CopiesTheTableInWholeFunctionPointers) {
    // A hand-built table whose struct_size covers each optional function wholly, in part or not
    // at all: only the wholly covered ones are copied, the others read as NULL.
    praenora_module_api_v0 table{};
    table.diagnostics = stub_diagnostics;
    table.self_test = stub_self_test;
    const std::size_t base = PRAENORA_MODULE_API_V0_BASE_SIZE;
    const std::size_t fn = sizeof(table.diagnostics);
    const auto copy = [&table](std::size_t struct_size) {
        table.header.struct_size = static_cast<std::uint32_t>(struct_size);
        return praenora::detail::copy_module_table(table.header);
    };
    for (std::size_t part = 0; part < fn; ++part) {
        const praenora_module_api_v0 none = copy(base + part);
        EXPECT_EQ(none.header.struct_size, base + part);
        EXPECT_EQ(none.diagnostics, nullptr) << "struct_size: base + " << part;
        EXPECT_EQ(none.self_test, nullptr) << "struct_size: base + " << part;
        const praenora_module_api_v0 one = copy(base + fn + part);
        EXPECT_TRUE(one.diagnostics == &stub_diagnostics) << "struct_size: base + fn + " << part;
        EXPECT_EQ(one.self_test, nullptr) << "struct_size: base + fn + " << part;
    }
    // The whole table, and a later minor version's larger one, cut to this runtime's size.
    for (const std::size_t size : {sizeof(table), sizeof(table) + 64}) {
        const praenora_module_api_v0 all = copy(size);
        EXPECT_TRUE(all.diagnostics == &stub_diagnostics) << "struct_size: " << size;
        EXPECT_TRUE(all.self_test == &stub_self_test) << "struct_size: " << size;
    }
}

TEST(Loader, RejectsMissingRequiredFunction) {
    const auto failure = fixture_load_failure("fx-missing-fn");
    EXPECT_EQ(failure.code, ErrorCode::API_TABLE_INVALID) << failure.message;
    EXPECT_TRUE(contains(failure.message, "create")) << failure.message;
}

TEST(Loader, RejectsReservedHeaderField) {
    const auto failure = fixture_load_failure("fx-reserved-field");
    EXPECT_EQ(failure.code, ErrorCode::API_TABLE_INVALID) << failure.message;
    EXPECT_TRUE(contains(failure.message, "reserved")) << failure.message;
}

// A describe() that does not produce a usable identity document is reported like a describe()
// that disagrees with the manifest: MODULE_IDENTITY_MISMATCH, owned by the module.
TEST(Loader, RejectsDescribeThatIsNotJson) {
    const auto failure = fixture_load_failure("fx-describe-not-json");
    EXPECT_EQ(failure.code, ErrorCode::MODULE_IDENTITY_MISMATCH) << failure.message;
    EXPECT_TRUE(contains(failure.message, "describe() did not write JSON")) << failure.message;
}

TEST(Loader, RejectsDescribeViolatingItsSchema) {
    const auto failure = fixture_load_failure("fx-describe-invalid");
    EXPECT_EQ(failure.code, ErrorCode::MODULE_IDENTITY_MISMATCH) << failure.message;
    EXPECT_TRUE(contains(failure.message, "pmp-describe")) << failure.message;
    EXPECT_TRUE(contains(failure.message, "capabilities")) << failure.message;
}

TEST(Loader, RejectsUnusableDescribeVersion) {
    // The version matches the schema's pattern, but its patch number overflows 64 bits.
    const auto failure = fixture_load_failure("fx-describe-overflow");
    EXPECT_EQ(failure.code, ErrorCode::MODULE_IDENTITY_MISMATCH) << failure.message;
    EXPECT_TRUE(contains(failure.message, "0.1.99999999999999999999")) << failure.message;
}

TEST(Loader, RejectsIdentityMismatch) {
    const auto failure = fixture_load_failure("fx-identity-mismatch");
    EXPECT_EQ(failure.code, ErrorCode::MODULE_IDENTITY_MISMATCH) << failure.message;
    EXPECT_TRUE(contains(failure.message, "version")) << failure.message;
    EXPECT_TRUE(contains(failure.message, "9.9.9")) << failure.message;
    // Only the version differs, so no other field is named.
    EXPECT_FALSE(contains(failure.message, "capabilities")) << failure.message;
}

TEST(Loader, VerifiesLibraryDigest) {
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "digest");
    const std::string digest =
        praenora::detail::sha256_file_hex(load_manifest(manifest_path).library_path);

    // The right digest: the library loads, and its digest is reported.
    edit_manifest(manifest_path, [&](nlohmann::json& json) { json["library"]["sha256"] = digest; });
    const auto module = LoadedModule::load(load_manifest(manifest_path));
    EXPECT_EQ(module->library_sha256(), digest);
    EXPECT_EQ(module->manifest().library_sha256, digest);

    // A wrong digest: refused before the library is opened.
    std::string wrong = digest;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    edit_manifest(manifest_path, [&](nlohmann::json& json) { json["library"]["sha256"] = wrong; });
    const auto failure = load_failure(load_manifest(manifest_path));
    EXPECT_EQ(failure.code, ErrorCode::LIBRARY_DIGEST_MISMATCH) << failure.message;
    EXPECT_TRUE(contains(failure.message, digest)) << failure.message;
    EXPECT_TRUE(contains(failure.message, wrong)) << failure.message;
    EXPECT_EQ(failure.location.pointer, "/library/sha256");
}

TEST(Loader, MissingLibraryIsLibraryNotFound) {
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "missing");
    const ModuleManifest manifest = load_manifest(manifest_path);
    fs::remove(manifest.library_path);
    const auto failure = load_failure(manifest);
    EXPECT_EQ(failure.code, ErrorCode::LIBRARY_NOT_FOUND) << failure.message;
    EXPECT_TRUE(contains(failure.message, manifest.library_path.u8string())) << failure.message;
    EXPECT_EQ(failure.location.pointer, "/library/path");
}

TEST(Loader, ChecksLibraryFileWithoutLoading) {
    const ModuleManifest echo = load_manifest(fixture_manifest("fx-echo"));
    EXPECT_EQ(check_library_file(echo), praenora::detail::sha256_file_hex(echo.library_path));

    // A file that is no library passes as long as it exists and matches its pinned digest:
    // nothing loads it. Loading it fails.
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "check");
    const fs::path text_library = scratch.path() / "check" / "not-a-library.bin";
    praenora::test::write_text_file(text_library, "Not a module library.\n");
    const std::string digest = praenora::detail::sha256_file_hex(text_library);
    edit_manifest(manifest_path, [&](nlohmann::json& json) {
        json["library"]["path"] = "not-a-library.bin";
        json["library"]["sha256"] = digest;
    });
    const ModuleManifest text = load_manifest(manifest_path);
    EXPECT_EQ(check_library_file(text), digest);
    EXPECT_EQ(load_failure(text).code, ErrorCode::LIBRARY_LOAD_FAILED);

    // A wrong digest and a missing file give the errors LoadedModule::load() gives.
    std::string wrong = digest;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    edit_manifest(manifest_path, [&](nlohmann::json& json) { json["library"]["sha256"] = wrong; });
    const ModuleManifest mismatched = load_manifest(manifest_path);
    const auto mismatch = library_check_failure(mismatched);
    EXPECT_EQ(mismatch.code, ErrorCode::LIBRARY_DIGEST_MISMATCH) << mismatch.message;
    EXPECT_EQ(mismatch.location.pointer, "/library/sha256");
    EXPECT_EQ(mismatch.message, load_failure(mismatched).message);

    fs::remove(text_library);
    const auto missing = library_check_failure(mismatched);
    EXPECT_EQ(missing.code, ErrorCode::LIBRARY_NOT_FOUND) << missing.message;
    EXPECT_EQ(missing.location.pointer, "/library/path");
    EXPECT_EQ(missing.location.file, mismatched.manifest_path.u8string());
    EXPECT_EQ(missing.message, load_failure(mismatched).message);
}

TEST(Loader, UnreadableLibraryIsIoErrorAtTheManifest) {
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "unreadable");
    const ModuleManifest manifest = load_manifest(manifest_path);
    fs::permissions(manifest.library_path, fs::perms::none);
    const bool readable = std::ifstream(manifest.library_path, std::ios::binary).is_open();
    if (readable) {
        fs::permissions(manifest.library_path, fs::perms::owner_all);
        GTEST_SKIP() << "the file stays readable here (permissions are not enforced)";
    }

    const auto failure = library_check_failure(manifest);
    fs::permissions(manifest.library_path, fs::perms::owner_all);
    EXPECT_EQ(failure.code, ErrorCode::IO_ERROR) << failure.message;
    EXPECT_EQ(failure.location.file, manifest.manifest_path.u8string());
    EXPECT_EQ(failure.location.pointer, "/library/path");
    EXPECT_TRUE(contains(failure.message, manifest.library_path.u8string())) << failure.message;
}

TEST(Loader, LoadFailureIncludesSystemMessage) {
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "text");
    const fs::path text_library = scratch.path() / "text" / "not-a-library.bin";
    praenora::test::write_text_file(text_library, "This is a text file, not a module library.\n");
    edit_manifest(manifest_path,
                  [](nlohmann::json& json) { json["library"]["path"] = "not-a-library.bin"; });

    const auto failure = load_failure(load_manifest(manifest_path));
    EXPECT_EQ(failure.code, ErrorCode::LIBRARY_LOAD_FAILED) << failure.message;
    // The runtime's own text names the library; the operating system's explanation follows.
    const std::string prefix = "cannot load module library '" + text_library.u8string() + "': ";
    ASSERT_TRUE(contains(failure.message, prefix)) << failure.message;
    const std::string system_text =
        failure.message.substr(failure.message.find(prefix) + prefix.size());
    EXPECT_GT(system_text.size(), 5u) << failure.message;
#if defined(__APPLE__)
    EXPECT_TRUE(contains(system_text, "mach-o")) << failure.message;
#elif defined(__linux__)
    EXPECT_TRUE(contains(system_text, "ELF")) << failure.message;
#endif
}

TEST(Loader, LoadsModuleFromUnicodePath) {
    const ScratchDir scratch;
    const fs::path directory = scratch.path() / utf8_path("m\xC3\xB6"
                                                          "dule dir");
    const fs::path manifest_path = copy_fixture("fx-echo", directory);

    const auto module = LoadedModule::load(load_manifest(manifest_path));
    EXPECT_EQ(module->describe().at("name"), "fx-echo");
    EXPECT_EQ(module->manifest().library_path.parent_path(), directory);
    EXPECT_TRUE(contains(module->manifest().library_path.u8string(), "m\xC3\xB6"
                                                                     "dule dir"));
    EXPECT_EQ(module->library_sha256(),
              praenora::detail::sha256_file_hex(module->manifest().library_path));
}

TEST(Loader, RepeatedLoadUnload) {
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-echo"));
    const praenora_host_api host = bare_host();
    const std::string invocation = R"({"inputs":{"in":{"data":{"round":1}}}})";
    for (int round = 0; round < 100; ++round) {
        SCOPED_TRACE(round);
        const auto module = LoadedModule::load(manifest);
        ASSERT_EQ(module->describe().at("name"), "fx-echo");

        // Each load is usable: one instance through the raw table.
        const praenora_module_api_v0& api = module->api();
        std::string error;
        praenora_sink error_sink{&error, &append_to_string};
        praenora_instance* instance = nullptr;
        ASSERT_EQ(api.create(&host, praenora_bytes{"{}", 2}, &instance, &error_sink),
                  PRAENORA_STATUS_OK)
            << error;
        std::string result;
        praenora_sink result_sink{&result, &append_to_string};
        EXPECT_EQ(api.invoke(instance, praenora_bytes{invocation.data(), invocation.size()},
                             &result_sink, &error_sink),
                  PRAENORA_STATUS_OK)
            << error;
        EXPECT_EQ(result, R"({"outputs":{"out":{"data":{"round":1}}}})");
        api.destroy(instance);
    }
}

TEST(Loader, SharedLibraryLooksUpSymbolsAndPins) {
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-echo"));
    SharedLibrary library = SharedLibrary::open(manifest.library_path, {});
    EXPECT_EQ(library.path(), manifest.library_path);
    EXPECT_NE(library.symbol(PRAENORA_MODULE_ENTRY_POINT), nullptr);
    EXPECT_EQ(library.symbol("praenora_no_such_symbol"), nullptr);
    EXPECT_FALSE(library.pinned());

    SharedLibrary moved = std::move(library);
    EXPECT_NE(moved.symbol(PRAENORA_MODULE_ENTRY_POINT), nullptr);
    moved.pin();
    EXPECT_TRUE(moved.pinned());
}

TEST(Loader, KeepLibrariesVariablePinsEveryLibrary) {
    {
        const ScopedVariable keep("PRAENORA_DEBUG_KEEP_LIBRARIES", "1");
        EXPECT_TRUE(load_fixture("fx-noop")->pinned());
    }
    for (const char* value : {static_cast<const char*>(nullptr), "0", "yes"}) {
        SCOPED_TRACE(value != nullptr ? value : "(unset)");
        const ScopedVariable other("PRAENORA_DEBUG_KEEP_LIBRARIES", value);
        EXPECT_FALSE(load_fixture("fx-noop")->pinned());
    }
}
