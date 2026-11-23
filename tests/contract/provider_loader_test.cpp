// The process-wide provider loader: the checks it makes, in order, and each failure's code and
// reason; loading and starting once per process; refusing a second library of a provider's name;
// the provider's own logger and the provider.loaded event; the export list of
// praenora_add_provider(). With the fixture providers of tests/fixtures/providers.
//
// Loaded providers stay loaded for the rest of the process. CTest runs each test in a process of
// its own; the tests also pass when one process runs them all, so none of them expects to be the
// first to load a provider unless loaded_provider() says it is.
#include "core/sha256.hpp"
#include "discovery/manifest.hpp"
#include "document/yaml_loader.hpp"
#include "loader/shared_library.hpp"
#include "observability/logging.hpp"
#include "provider/provider_abi.h"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "log_capture.hpp"
#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "test_echo.h"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::load_provider;
using praenora::detail::load_provider_manifest;
using praenora::detail::loaded_provider;
using praenora::detail::LoadedProvider;
using praenora::detail::LogHub;
using praenora::detail::ProviderManifest;
using praenora::detail::ServiceVersion;
using praenora::test::fixture_provider_manifest;
using praenora::test::ScratchDir;

namespace {

const ServiceVersion kEcho{"test.echo", 0, 0};

ProviderManifest fixture(std::string_view name) {
    return load_provider_manifest(fixture_provider_manifest(name));
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// How loading fails: the error, or one with code OK when the provider loads.
Error load_failure(const ProviderManifest& manifest,
                   const std::vector<ServiceVersion>& services = {kEcho},
                   const std::optional<std::string>& validated_sha256 = std::nullopt) {
    try {
        static_cast<void>(load_provider(manifest, services, validated_sha256, nullptr));
    } catch (const Error& error) {
        return error;
    }
    return Error(ErrorCode::OK, "loaded");
}

json details_of(const Error& error) { return json::parse(error.details_json()); }

// Copies the library of fixture provider `name` into `directory`, writes a manifest for it there
// (the fixture's manifest after `edit`) and returns the manifest's path.
template <class Edit>
fs::path copy_fixture(std::string_view name, const fs::path& directory, Edit edit) {
    const ProviderManifest manifest = fixture(name);
    fs::create_directories(directory);
    fs::copy_file(manifest.library_path, directory / manifest.library_path.filename(),
                  fs::copy_options::overwrite_existing);
    json document = praenora::detail::load_document_file(manifest.manifest_path).json;
    edit(document);
    const fs::path copied = directory / manifest.manifest_path.filename();
    praenora::test::write_text_file(copied, document.dump(2));
    return copied;
}

fs::path copy_fixture(std::string_view name, const fs::path& directory) {
    return copy_fixture(name, directory, [](json&) {});
}

praenora::LogOptions quiet_debug_logging() {
    praenora::LogOptions options;
    options.to_stderr = false;
    options.level = praenora::LogLevel::debug;
    return options;
}

std::vector<json> records_of(const praenora::test::LogCapture& capture, const std::string& logger,
                             const std::string& body_part) {
    std::vector<json> found;
    for (const auto& record : capture.json_lines()) {
        if (record.at("attributes").at("logger") == logger &&
            contains(record.at("body").get<std::string>(), body_part)) {
            found.push_back(record);
        }
    }
    return found;
}

// Whether the library at `path` is loaded in this process (without loading it).
bool in_process(const fs::path& path) {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(0, path.c_str(), &module)) {
        return false;
    }
    FreeLibrary(module);
    return true;
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
#endif
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::size_t occurrences(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
        ++count;
    }
    return count;
}

praenora_status PRAENORA_CALL append_to_string(void* context, const char* data, std::size_t size) {
    static_cast<std::string*>(context)->append(data, size);
    return PRAENORA_STATUS_OK;
}

} // namespace

TEST(ProviderLoader, LoadsAndStartsTheEchoProvider) {
    const ProviderManifest manifest = fixture("test-echo");
    const LoadedProvider& provider = load_provider(manifest, {kEcho}, std::nullopt, nullptr);
    EXPECT_EQ(provider.manifest().name, "test-echo");
    EXPECT_EQ(provider.library_sha256(), praenora::detail::sha256_file_hex(manifest.library_path));
    EXPECT_EQ(provider.describe().at("name"), "test-echo");
    EXPECT_EQ(provider.describe().at("engine"), manifest.engine);
    EXPECT_EQ(loaded_provider("test-echo"), &provider);

    // The service table the provider returned, used as a module would.
    const auto* echo = static_cast<const praenora_test_echo_v0*>(provider.service("test.echo", 0));
    ASSERT_NE(echo, nullptr);
    EXPECT_EQ(echo->header.abi_major, 0u);
    EXPECT_EQ(echo->header.reserved, 0u);
    EXPECT_GE(echo->header.struct_size, sizeof(praenora_test_echo_v0));
    std::string output;
    praenora_sink sink{&output, append_to_string};
    const std::string input = "{\"hello\": [1, 2]}";
    EXPECT_EQ(echo->echo(echo->context, nullptr, praenora_bytes{input.data(), input.size()}, &sink),
              PRAENORA_STATUS_OK);
    EXPECT_EQ(output, input);
    EXPECT_EQ(provider.service("test.echo", 1), nullptr) << "never asked for";
}

TEST(ProviderLoader, LoadsOncePerProcessAndStartsOnce) {
    const ProviderManifest manifest = fixture("test-echo");
    const LoadedProvider& first = load_provider(manifest, {kEcho}, std::nullopt, nullptr);
    // test-echo refuses a second start(), so these loads would fail if they started it again.
    const LoadedProvider& second = load_provider(manifest, {kEcho}, std::nullopt, nullptr);
    const LoadedProvider& third =
        load_provider(fixture("test-echo"), {}, first.library_sha256(), nullptr);
    EXPECT_EQ(&first, &second);
    EXPECT_EQ(&first, &third);
    EXPECT_EQ(first.service("test.echo", 0), second.service("test.echo", 0));
}

TEST(ProviderLoader, ConcurrentFirstLoadsShareOneProvider) {
    std::vector<const LoadedProvider*> loaded(8, nullptr);
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        threads.emplace_back([&loaded, &failures, i] {
            try {
                loaded[i] = &load_provider(fixture("test-reordered"),
                                           {{"test.echo", 0, 0}, {"test.other", 1, 2}},
                                           std::nullopt, nullptr);
            } catch (const Error&) {
                ++failures;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // test-reordered refuses a second start(), so a second start would make a thread fail.
    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(std::all_of(loaded.begin(), loaded.end(),
                            [&](const LoadedProvider* p) { return p == loaded.front(); }));
    EXPECT_EQ(loaded_provider("test-reordered"), loaded.front());
}

TEST(ProviderLoader, ComparesServicesAsASet) {
    // test-reordered's describe() lists its services in the other order.
    const LoadedProvider& provider = load_provider(
        fixture("test-reordered"), {{"test.other", 1, 0}}, std::nullopt, nullptr);
    EXPECT_NE(provider.service("test.other", 1), nullptr);
}

TEST(ProviderLoader, RefusesAServiceTheManifestDoesNotOffer) {
    const Error error = load_failure(fixture("test-echo"), {{"test.echo", 0, 1}});
    EXPECT_EQ(error.code(), ErrorCode::INVALID_ARGUMENT) << error.to_json();
    EXPECT_TRUE(contains(error.what(), "test.echo@0.1")) << error.what();
}

TEST(ProviderLoader, MissingLibraryIsLibraryNotFound) {
    const ScratchDir scratch;
    const fs::path path = copy_fixture("test-echo", scratch.path() / "missing");
    const ProviderManifest manifest = load_provider_manifest(path);
    fs::remove(manifest.library_path);
    const Error error = load_failure(manifest);
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_NOT_FOUND) << error.to_json();
    EXPECT_EQ(error.location().file, manifest.manifest_path.u8string());
    EXPECT_EQ(error.location().pointer, "/library/path");
    EXPECT_TRUE(contains(error.what(), "provider test-echo@0.1.0: the provider library"))
        << error.what();
}

TEST(ProviderLoader, VerifiesThePinnedAndTheValidatedDigest) {
    const ProviderManifest original = fixture("test-echo");
    const std::string digest = praenora::detail::sha256_file_hex(original.library_path);
    std::string wrong = digest;
    wrong[0] = wrong[0] == '0' ? '1' : '0';

    // A pinned digest that differs: refused before the library is opened.
    ProviderManifest pinned = original;
    pinned.library_sha256 = wrong;
    Error error = load_failure(pinned);
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_DIGEST_MISMATCH) << error.to_json();
    EXPECT_EQ(error.location().pointer, "/library/sha256");
    EXPECT_TRUE(contains(error.what(), digest)) << error.what();

    // A validated digest that differs: the library changed after validation.
    error = load_failure(original, {kEcho}, wrong);
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_DIGEST_MISMATCH) << error.to_json();
    EXPECT_EQ(error.location().pointer, "/library/path");
    EXPECT_TRUE(contains(error.what(), "changed after validation")) << error.what();

    // Both right: it loads.
    pinned.library_sha256 = digest;
    EXPECT_EQ(load_failure(pinned, {kEcho}, digest).code(), ErrorCode::OK);
}

TEST(ProviderLoader, AFileThatIsNoLibraryFailsToLoad) {
    const ScratchDir scratch;
    const fs::path path = copy_fixture("test-echo", scratch.path() / "text", [](json& document) {
        document["provider"]["name"] = "test-text";
        document["library"]["path"] = "not-a-library.bin";
    });
    praenora::test::write_text_file(scratch.path() / "text" / "not-a-library.bin",
                                    "This is a text file, not a provider library.\n");
    const Error error = load_failure(load_provider_manifest(path));
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_LOAD_FAILED) << error.to_json();
    EXPECT_EQ(details_of(error).at("reason"), "library");
    EXPECT_EQ(error.location().pointer, "/library/path");
    EXPECT_TRUE(contains(error.what(), "cannot load provider library")) << error.what();
}

TEST(ProviderLoader, ALibraryWithoutTheEntryPointIsRefused) {
    // A copy of test-echo's manifest under another name, whose library is replaced by that of
    // fx-no-entry: a fixture module library that exports nothing. That library stays in the
    // process, so a later run of this test in the same process meets it as another library of the
    // name.
    static int runs = 0;
    const bool first_run = runs++ == 0;
    const ScratchDir scratch;
    const fs::path path =
        copy_fixture("test-echo", scratch.path() / "no-entry", [](json& document) {
            document["provider"]["name"] = "test-no-entry";
        });
    const ProviderManifest manifest = load_provider_manifest(path);
    const auto module =
        praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-no-entry"));
    fs::copy_file(module.library_path, manifest.library_path, fs::copy_options::overwrite_existing);

    const Error error = load_failure(manifest);
    EXPECT_EQ(error.location().file, manifest.manifest_path.u8string());
    EXPECT_EQ(loaded_provider("test-no-entry"), nullptr);
    if (!first_run) {
        EXPECT_EQ(details_of(error).at("reason"), "conflict") << error.to_json();
        return;
    }
    EXPECT_EQ(error.code(), ErrorCode::ENTRY_POINT_MISSING) << error.to_json();
    EXPECT_TRUE(contains(error.what(), PRAENORA_PROVIDER_ENTRY_POINT)) << error.what();
}

TEST(ProviderLoader, ATableThatBreaksTheHeaderRulesIsRefused) {
    const struct {
        const char* fixture;
        const char* message_part;
    } cases[] = {
        {"test-null-table", "returned NULL"},
        {"test-table-abi-major", "provider interface major 1"},
        {"test-table-reserved", "reserved is 7"},
        {"test-table-short", "struct_size"},
        {"test-table-null-start", "start"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.fixture);
        const ProviderManifest manifest = fixture(c.fixture);
        const Error error = load_failure(manifest);
        EXPECT_EQ(error.code(), ErrorCode::API_TABLE_INVALID) << error.to_json();
        EXPECT_TRUE(contains(error.what(), c.message_part)) << error.what();
        EXPECT_EQ(error.location().file, manifest.manifest_path.u8string());
        EXPECT_EQ(loaded_provider(c.fixture), nullptr);
    }
}

TEST(ProviderLoader, ADescribeThatDisagreesWithTheManifestIsRefused) {
    const Error mismatch = load_failure(fixture("test-identity-mismatch"));
    EXPECT_EQ(mismatch.code(), ErrorCode::LIBRARY_LOAD_FAILED) << mismatch.to_json();
    const json details = details_of(mismatch);
    EXPECT_EQ(details.at("reason"), "identity");
    std::vector<std::string> fields;
    for (const auto& difference : details.at("differences")) {
        fields.push_back(difference.at("field").get<std::string>());
    }
    // The version and the engine differ; name and services agree.
    EXPECT_EQ(fields, (std::vector<std::string>{"version", "engine"}));
    EXPECT_EQ(details.at("differences")[1].at("describe"), json::parse(R"({"name":"other"})"));
    EXPECT_TRUE(contains(mismatch.what(), "9.9.9")) << mismatch.what();

    const Error not_json = load_failure(fixture("test-describe-not-json"));
    EXPECT_EQ(not_json.code(), ErrorCode::LIBRARY_LOAD_FAILED) << not_json.to_json();
    EXPECT_EQ(details_of(not_json).at("reason"), "identity");
    EXPECT_TRUE(contains(not_json.what(), "describe()")) << not_json.what();
}

// JSON's -0 is the integer 0: a describe() that writes its service's major and minor as -0 agrees
// with a manifest that names test.echo@0.0.
TEST(ProviderLoader, ADescribeThatWritesNegativeZeroIsAccepted) {
    const Error error = load_failure(fixture("test-negative-zero"));
    ASSERT_EQ(error.code(), ErrorCode::OK) << error.to_json();
    const LoadedProvider* provider = loaded_provider("test-negative-zero");
    ASSERT_NE(provider, nullptr);
    // nlohmann/json keeps a -0 as a signed integer, which prints as 0.
    const json service = provider->describe().at("services").at(0);
    EXPECT_FALSE(service.at("major").is_number_unsigned());
    EXPECT_FALSE(service.at("minor").is_number_unsigned());
    EXPECT_EQ(service.dump(), R"({"major":0,"minor":0,"name":"test.echo"})");
}

TEST(ProviderLoader, AManifestThatDisagreesWithALoadedProviderIsRefused) {
    const ProviderManifest echo = fixture("test-echo");
    static_cast<void>(load_provider(echo, {kEcho}, std::nullopt, nullptr));
    // The same library, through a manifest that names another engine.
    ProviderManifest other = echo;
    other.engine = {{"name", "test-echo-engine"}, {"version", "2.0.0"}};
    const Error error = load_failure(other);
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_LOAD_FAILED) << error.to_json();
    EXPECT_EQ(details_of(error).at("reason"), "identity");
}

TEST(ProviderLoader, AStartThatFailsIsReportedAndNeverRetried) {
    const ProviderManifest manifest = fixture("test-start-fails");
    const Error first = load_failure(manifest);
    EXPECT_EQ(first.code(), ErrorCode::LIBRARY_LOAD_FAILED) << first.to_json();
    const json details = details_of(first);
    EXPECT_EQ(details.at("reason"), "start");
    EXPECT_EQ(details.at("cause").at("code"), 11);
    EXPECT_EQ(details.at("cause").at("message"), "the engine cannot start");
    EXPECT_TRUE(contains(first.what(), "the engine cannot start")) << first.what();
    EXPECT_EQ(loaded_provider("test-start-fails"), nullptr);

    // start() runs at most once per process: a second load reports the first failure, whose
    // error document says start() was called once.
    const Error second = load_failure(manifest);
    EXPECT_EQ(second.code(), ErrorCode::LIBRARY_LOAD_FAILED) << second.to_json();
    EXPECT_EQ(details_of(second).at("reason"), "start");
    EXPECT_EQ(details_of(second).at("cause").at("details").at("calls"), 1);
    EXPECT_TRUE(contains(second.what(), "not called again")) << second.what();
}

TEST(ProviderLoader, AServiceThatGetServiceDoesNotReturnIsRefused) {
    const ProviderManifest manifest = fixture("test-missing-service");
    const Error error = load_failure(manifest, {{"test.echo", 0, 0}, {"test.absent", 0, 0}});
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_LOAD_FAILED) << error.to_json();
    EXPECT_EQ(details_of(error).at("reason"), "service");
    EXPECT_EQ(details_of(error).at("services"), json::array({"test.absent@0"}));
    EXPECT_EQ(error.location().pointer, "/services/1");

    // The provider itself is loaded and started; a load that needs only what it returns succeeds.
    const LoadedProvider& provider = load_provider(manifest, {kEcho}, std::nullopt, nullptr);
    EXPECT_NE(provider.service("test.echo", 0), nullptr);
    EXPECT_EQ(provider.service("test.absent", 0), nullptr);
}

TEST(ProviderLoader, AnotherLibraryOfALoadedProvidersNameIsAConflict) {
    const ProviderManifest echo = fixture("test-echo");
    const LoadedProvider& loaded = load_provider(echo, {kEcho}, std::nullopt, nullptr);

    const ScratchDir scratch;
    const ProviderManifest copy =
        load_provider_manifest(copy_fixture("test-echo", scratch.path() / "copy"));
    const Error error = load_failure(copy);
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_LOAD_FAILED) << error.to_json();
    const json details = details_of(error);
    EXPECT_EQ(details.at("reason"), "conflict");
    EXPECT_EQ(details.at("loaded").at("library"), loaded.manifest().library_path.u8string());
    EXPECT_EQ(error.location().file, copy.manifest_path.u8string());
    EXPECT_EQ(loaded_provider("test-echo"), &loaded) << "the loaded provider stays";
}

// A library of a provider's name that fails a check after it opened stays in the process, so a
// later load of another library of that name is a conflict too.
TEST(ProviderLoader, AnotherLibraryIsAConflictAlsoAfterAFailedCheck) {
    // A later run of this test in the same process meets the copy of the first run.
    static int runs = 0;
    const bool first_run = runs++ == 0;
    const ScratchDir scratch;
    const fs::path stale = copy_fixture("test-conflict", scratch.path() / "stale", [](json& doc) {
        doc["engine"]["version"] = "0.9.0";
    });
    const Error first = load_failure(load_provider_manifest(stale));
    EXPECT_EQ(first.code(), ErrorCode::LIBRARY_LOAD_FAILED) << first.to_json();
    EXPECT_EQ(details_of(first).at("reason"), first_run ? "identity" : "conflict")
        << first.to_json();

    const ProviderManifest original = fixture("test-conflict");
    const Error second = load_failure(original);
    EXPECT_EQ(second.code(), ErrorCode::LIBRARY_LOAD_FAILED) << second.to_json();
    EXPECT_EQ(details_of(second).at("reason"), "conflict");
    EXPECT_EQ(second.location().file, original.manifest_path.u8string());
    EXPECT_EQ(loaded_provider("test-conflict"), nullptr);
}

// The same library, retried with a corrected manifest after a failed identity check: its checks
// run again and it starts.
TEST(ProviderLoader, ARetryWithACorrectedManifestStartsTheProvider) {
    const bool started_before = loaded_provider("test-retry") != nullptr;
    const ProviderManifest manifest = fixture("test-retry");
    ProviderManifest stale = manifest;
    stale.engine = {{"name", "test-retry-engine"}, {"version", "0.9.0"}};
    const Error error = load_failure(stale);
    EXPECT_EQ(error.code(), ErrorCode::LIBRARY_LOAD_FAILED) << error.to_json();
    EXPECT_EQ(details_of(error).at("reason"), "identity");
    if (!started_before) {
        EXPECT_EQ(loaded_provider("test-retry"), nullptr);
    }

    // test-retry refuses a second start(): this load starts it once.
    const LoadedProvider& provider = load_provider(manifest, {kEcho}, std::nullopt, nullptr);
    EXPECT_EQ(loaded_provider("test-retry"), &provider);
    EXPECT_EQ(provider.manifest().engine, manifest.engine);
    EXPECT_NE(provider.service("test.echo", 0), nullptr);
}

// Provider libraries are never unloaded: neither one that started nor one that failed a check
// after it opened.
TEST(ProviderLoader, ProviderLibrariesStayLoaded) {
    const ProviderManifest echo = fixture("test-echo");
    static_cast<void>(load_provider(echo, {kEcho}, std::nullopt, nullptr));
    EXPECT_TRUE(in_process(echo.library_path));

    const ProviderManifest reserved = fixture("test-table-reserved");
    EXPECT_EQ(load_failure(reserved).code(), ErrorCode::API_TABLE_INVALID);
    EXPECT_TRUE(in_process(reserved.library_path));
}

// When the logs a provider writes to are destroyed, the provider lets go of their sinks and files;
// its records go nowhere until a load gives it logs again.
TEST(ProviderLoader, LetsGoOfLogsThatAreDestroyed) {
    const ScratchDir scratch;
    const fs::path log_file = scratch.path() / "runtime.log";
    std::weak_ptr<spdlog::sinks::sink> sink_of_destroyed_logs;
    {
        praenora::LogOptions options = quiet_debug_logging();
        options.format = praenora::LogFormat::json;
        options.file = log_file.u8string();
        LogHub logs(options);
        auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(16);
        logs.add_sink(sink);
        sink_of_destroyed_logs = sink;
        static_cast<void>(load_provider(fixture("test-echo"), {kEcho}, std::nullopt, &logs));
    }
    EXPECT_TRUE(sink_of_destroyed_logs.expired()) << "the provider still holds the logs' sinks";
    const std::string written = read_text(log_file);
    EXPECT_EQ(occurrences(written, "service test.echo@0 requested"), 1u) << written;

    // get_service() logs again; nothing reaches the destroyed logs' file.
    static_cast<void>(load_provider(fixture("test-echo"), {kEcho}, std::nullopt, nullptr));
    EXPECT_EQ(read_text(log_file), written);

    // A load with logs takes the provider's records there.
    LogHub next(quiet_debug_logging());
    const praenora::test::LogCapture capture(next);
    static_cast<void>(load_provider(fixture("test-echo"), {kEcho}, std::nullopt, &next));
    EXPECT_EQ(records_of(capture, "provider.test-echo", "requested").size(), 1u);
}

TEST(ProviderLoader, LogsThroughTheProvidersOwnLogger) {
    const bool first_load = loaded_provider("test-echo") == nullptr;
    LogHub first_hub(quiet_debug_logging());
    const praenora::test::LogCapture first_capture(first_hub);
    static_cast<void>(load_provider(fixture("test-echo"), {kEcho}, std::nullopt, &first_hub));

    // get_service() logs every request through the provider's logger, provider.test-echo.
    EXPECT_EQ(records_of(first_capture, "provider.test-echo", "service test.echo@0 requested")
                  .size(),
              1u);
    const auto loaded = records_of(first_capture, "provider.test-echo", "loaded");
    const auto started = records_of(first_capture, "provider.test-echo", "test-echo started");
    if (first_load) {
        // The first load in the process: start() logged, and the provider.loaded event.
        EXPECT_EQ(started.size(), 1u);
        ASSERT_EQ(loaded.size(), 1u);
        const json& attributes = loaded.front().at("attributes");
        EXPECT_EQ(attributes.at("praenora.event"), "provider.loaded");
        EXPECT_EQ(attributes.at("praenora.provider"), "test-echo@0.1.0");
        EXPECT_EQ(attributes.at("praenora.library_sha256"),
                  praenora::detail::sha256_file_hex(fixture("test-echo").library_path));
        EXPECT_EQ(loaded.front().at("severity_text"), "INFO");
    } else {
        EXPECT_TRUE(started.empty());
        EXPECT_TRUE(loaded.empty());
    }

    // A later load through another runtime's logs takes the provider's records there.
    LogHub second_hub(quiet_debug_logging());
    const praenora::test::LogCapture second_capture(second_hub);
    static_cast<void>(load_provider(fixture("test-echo"), {kEcho}, std::nullopt, &second_hub));
    EXPECT_EQ(records_of(second_capture, "provider.test-echo", "requested").size(), 1u);
    EXPECT_EQ(records_of(first_capture, "provider.test-echo", "requested").size(), 1u);
    EXPECT_TRUE(records_of(second_capture, "provider.test-echo", "loaded").empty())
        << "provider.loaded is logged once per process";
}

// praenora_add_provider() exports only the entry point, although the library defines another
// function with default visibility.
TEST(ProviderLoader, TheLibraryExportsOnlyTheEntryPoint) {
#ifdef _WIN32
    GTEST_SKIP() << "Windows exports what is marked __declspec(dllexport)";
#else
    const ProviderManifest manifest = fixture("test-echo");
    auto library = praenora::detail::SharedLibrary::open(manifest.library_path, {});
    library.pin(); // a provider library is never unloaded
    EXPECT_NE(library.symbol(PRAENORA_PROVIDER_ENTRY_POINT), nullptr);
    EXPECT_EQ(library.symbol("test_echo_unlisted_function"), nullptr);
    EXPECT_EQ(library.symbol("provider_fixture_describe"), nullptr);
#endif
}
