// Provider discovery: the generated fixture manifests, the provider search paths, scanning,
// shadowing, usability and the resolution of services. Nothing here loads a provider.
#include "compat/compat.hpp"
#include "core/semver.hpp"
#include "discovery/module_index.hpp"
#include "platform/platform.hpp"
#include "provider/provider_abi.h"
#include "provider/provider_index.hpp"
#include "provider/provider_manifest.hpp"

#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "scoped_environment.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Diagnostic;
using praenora::ErrorCode;
using praenora::Severity;
using praenora::detail::installation_module_dir;
using praenora::detail::installation_provider_dir;
using praenora::detail::load_provider_manifest;
using praenora::detail::provider_problem;
using praenora::detail::provider_search_paths;
using praenora::detail::ProviderIndex;
using praenora::detail::ProviderManifest;
using praenora::detail::ServiceVersion;
using praenora::test::ScopedEnvironment;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::write_text_file;

namespace {

std::string utf8(const fs::path& path) { return path.u8string(); }

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string path_list(const std::vector<std::string>& entries) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string list;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            list += separator;
        }
        list += entries[i];
    }
    return list;
}

// A provider manifest that this runtime can use: this platform, this runtime's version, this
// provider interface major. `services` are "<name>@<major>.<minor>" texts.
json usable_manifest(const std::string& name, const std::vector<std::string>& services) {
    const auto host = praenora::detail::host_platform();
    json list = json::array();
    for (const auto& service : services) {
        const auto at = service.find('@');
        const auto dot = service.find('.', at);
        list.push_back({{"name", service.substr(0, at)},
                        {"major", std::stoul(service.substr(at + 1, dot - at - 1))},
                        {"minor", std::stoul(service.substr(dot + 1))}});
    }
    return {{"kind", "provider-manifest"},
            {"format_version", "0.1.0"},
            {"provider", {{"name", name}, {"version", "0.1.0"}}},
            {"runtime_version", praenora::version_info().runtime},
            {"abi",
             {{"major", PRAENORA_PROVIDER_ABI_MAJOR}, {"minor", PRAENORA_PROVIDER_ABI_MINOR}}},
            {"library", {{"path", name + ".so"}}},
            {"platform", {{"os", host.os}, {"arch", host.arch}}},
            {"services", list},
            {"engine", {{"name", name + "-engine"}}}};
}

// Writes `manifest` to `file` and returns the file's lexically normal path.
fs::path write_manifest(const fs::path& file, const json& manifest) {
    write_text_file(file, manifest.dump(2));
    return file.lexically_normal();
}

std::vector<std::string> names_of(const ProviderIndex& index) {
    std::vector<std::string> names;
    for (const auto& entry : index.entries()) {
        names.push_back(entry.manifest.name + (entry.shadowed_by ? " (shadowed)" : ""));
    }
    return names;
}

const praenora::detail::IndexedProvider* entry_named(const ProviderIndex& index,
                                                      const std::string& name) {
    for (const auto& entry : index.entries()) {
        if (entry.manifest.name == name && !entry.shadowed_by) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

TEST(ProviderManifest, LoadsGeneratedFixtureManifest) {
    const fs::path path = praenora::test::fixture_provider_manifest("test-echo");
    const ProviderManifest manifest = load_provider_manifest(path);

    EXPECT_EQ(manifest.name, "test-echo");
    EXPECT_EQ(praenora::detail::to_string(manifest.version), "0.1.0");
    EXPECT_EQ(praenora::detail::to_string(manifest.runtime_version),
              praenora::version_info().runtime);
    EXPECT_EQ(manifest.abi_major, PRAENORA_PROVIDER_ABI_MAJOR);
    EXPECT_EQ(manifest.abi_minor, PRAENORA_PROVIDER_ABI_MINOR);
    const auto host = praenora::detail::host_platform();
    EXPECT_EQ(manifest.os, host.os);
    EXPECT_EQ(manifest.arch, host.arch);
    ASSERT_EQ(manifest.services.size(), 1u);
    EXPECT_EQ(praenora::detail::to_string(manifest.services[0]), "test.echo@0.0");
    EXPECT_EQ(manifest.engine, json::parse(R"({"name":"test-echo-engine","version":"1.0.0"})"));
    EXPECT_FALSE(manifest.library_sha256.has_value());

    EXPECT_TRUE(manifest.manifest_path.is_absolute());
    EXPECT_TRUE(fs::equivalent(manifest.manifest_path, path));
    EXPECT_EQ(manifest.library_path.parent_path(), manifest.directory);
    EXPECT_EQ(manifest.library_path.stem().u8string(), "test-echo");
    EXPECT_TRUE(fs::is_regular_file(manifest.library_path));
    EXPECT_FALSE(provider_problem(manifest).has_value()) << *provider_problem(manifest);
}

// Every manifest praenora_add_provider() generated for the fixture providers is valid and names
// its library, and its services are those the fixture declared.
TEST(ProviderManifest, GeneratedFixtureManifestsAreValid) {
    int count = 0;
    for (const auto& directory :
         fs::directory_iterator(utf8_path(PRAENORA_TEST_PROVIDERS_DIR))) {
        if (!directory.is_directory()) {
            continue;
        }
        const std::string name = directory.path().filename().u8string();
        SCOPED_TRACE(name);
        const auto manifest =
            load_provider_manifest(praenora::test::fixture_provider_manifest(name));
        EXPECT_EQ(manifest.name, name);
        EXPECT_TRUE(fs::is_regular_file(manifest.library_path));
        EXPECT_FALSE(manifest.services.empty());
        ++count;
    }
    EXPECT_GE(count, 11);
}

TEST(ProviderDiscovery, SearchOrderExplicitThenEnvironmentThenInstallation) {
    const ScratchDir scratch;
    const fs::path first = scratch.path() / "first";
    const fs::path second = scratch.path() / "second";
    const fs::path from_environment = scratch.path() / "environment";
    const ScopedEnvironment variable("PRAENORA_PROVIDER_PATH",
                                     path_list({utf8(from_environment), "", utf8(first)}));

    const auto paths = provider_search_paths({utf8(first), utf8(second) + "/", utf8(first)}, true);
    ASSERT_GE(paths.size(), 3u);
    EXPECT_EQ(paths[0], first.lexically_normal());
    EXPECT_EQ(paths[1], second.lexically_normal()) << "trailing separators are dropped";
    EXPECT_EQ(paths[2], from_environment.lexically_normal());
    // A path named twice is searched once; the installation's directory comes last.
    const auto installed = installation_provider_dir();
    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(paths.size(), 4u);
    EXPECT_EQ(paths.back(), *installed);

    // Without the environment, PRAENORA_PROVIDER_PATH is ignored.
    const auto without = provider_search_paths({utf8(first)}, false);
    ASSERT_EQ(without.size(), 2u);
    EXPECT_EQ(without[0], first.lexically_normal());
    EXPECT_EQ(without[1], *installed);
}

TEST(ProviderDiscovery, RelativePathsAreAgainstTheCurrentDirectory) {
    const auto paths = provider_search_paths({"relative/providers"}, false);
    ASSERT_FALSE(paths.empty());
    EXPECT_TRUE(paths[0].is_absolute());
    EXPECT_EQ(paths[0], (fs::current_path() / "relative" / "providers").lexically_normal());
}

TEST(ProviderDiscovery, InstallationDirectoryIsTheModuleDirectorysSibling) {
    const auto providers = installation_provider_dir();
    const auto modules = installation_module_dir();
    ASSERT_TRUE(providers.has_value());
    ASSERT_TRUE(modules.has_value());
    EXPECT_EQ(*providers, modules->parent_path() / "providers");
    EXPECT_EQ(providers->parent_path().filename(), "praenora");
}

TEST(ProviderIndex, ScansOwnFilesThenImmediateSubdirectoriesInByteOrder) {
    const ScratchDir scratch;
    const fs::path root = scratch.path() / "providers";
    write_manifest(root / "b.provider.json", usable_manifest("p-b", {"test.echo@0.0"}));
    write_manifest(root / "a.provider.json", usable_manifest("p-a", {"test.echo@0.0"}));
    write_manifest(root / "Z" / "z.provider.json", usable_manifest("p-z", {"test.echo@0.0"}));
    write_manifest(root / "m" / "m.provider.json", usable_manifest("p-m", {"test.echo@0.0"}));
    write_manifest(root / "m" / "deeper" / "d.provider.json",
                   usable_manifest("p-deep", {"test.echo@0.0"}));
    write_text_file(root / "notes.json", "{}");
    write_text_file(root / "m.pmm.json", "{}");

    const ProviderIndex index = ProviderIndex::scan({root});
    // Own files first (a, b), then each subdirectory's, by UTF-8 bytes ("Z" before "m"); nothing
    // deeper than one level, and no file of another kind.
    EXPECT_EQ(names_of(index), (std::vector<std::string>{"p-a", "p-b", "p-z", "p-m"}));
    EXPECT_TRUE(index.diagnostics().empty());
    EXPECT_EQ(index.search_paths(), std::vector<fs::path>{root});
}

TEST(ProviderIndex, MissingPathsAreSkippedAndFilesAreWarnings) {
    const ScratchDir scratch;
    const fs::path file = scratch.path() / "a-file";
    write_text_file(file, "not a directory");
    const ProviderIndex index = ProviderIndex::scan({scratch.path() / "absent", file});
    EXPECT_TRUE(index.entries().empty());
    ASSERT_EQ(index.diagnostics().size(), 1u);
    const Diagnostic& warning = index.diagnostics().front();
    EXPECT_EQ(warning.severity, Severity::warning);
    EXPECT_EQ(warning.code, ErrorCode::NOT_FOUND);
    EXPECT_TRUE(contains(warning.message, "provider search path")) << warning.message;
    EXPECT_EQ(warning.location.file, utf8(file));
}

TEST(ProviderIndex, InvalidManifestsBecomeWarningsAndAreSkipped) {
    const ScratchDir scratch;
    const fs::path root = scratch.path() / "providers";
    write_text_file(root / "a.provider.json", "{ not json");
    json absolute = usable_manifest("p-absolute", {"test.echo@0.0"});
    absolute["library"]["path"] = "/opt/p-absolute.so";
    write_manifest(root / "b.provider.json", absolute);
    write_manifest(root / "c.provider.json", usable_manifest("p-good", {"test.echo@0.0"}));

    const ProviderIndex index = ProviderIndex::scan({root});
    EXPECT_EQ(names_of(index), std::vector<std::string>{"p-good"});
    ASSERT_EQ(index.diagnostics().size(), 2u);
    EXPECT_EQ(index.diagnostics()[0].code, ErrorCode::PARSE_ERROR);
    EXPECT_EQ(index.diagnostics()[0].severity, Severity::warning);
    EXPECT_TRUE(contains(index.diagnostics()[0].location.file, "a.provider.json"));
    EXPECT_GT(index.diagnostics()[0].location.line, 0);
    EXPECT_EQ(index.diagnostics()[1].code, ErrorCode::MANIFEST_INVALID);
    EXPECT_EQ(index.diagnostics()[1].location.pointer, "/library/path");
    EXPECT_TRUE(contains(index.diagnostics()[1].message, "skipped"))
        << index.diagnostics()[1].message;
}

TEST(ProviderIndex, ASecondManifestOfANameIsShadowed) {
    const ScratchDir scratch;
    const fs::path first = write_manifest(scratch.path() / "one" / "p.provider.json",
                                          usable_manifest("p", {"test.echo@0.0"}));
    const fs::path second = write_manifest(scratch.path() / "two" / "p.provider.json",
                                           usable_manifest("p", {"test.echo@0.5"}));

    const ProviderIndex index =
        ProviderIndex::scan({scratch.path() / "one", scratch.path() / "two"});
    EXPECT_EQ(names_of(index), (std::vector<std::string>{"p", "p (shadowed)"}));
    ASSERT_EQ(index.diagnostics().size(), 1u);
    const Diagnostic& warning = index.diagnostics().front();
    EXPECT_EQ(warning.severity, Severity::warning);
    EXPECT_EQ(warning.code, ErrorCode::OK);
    EXPECT_TRUE(contains(warning.message, utf8(first))) << warning.message;
    EXPECT_TRUE(contains(warning.message, utf8(second))) << warning.message;
    EXPECT_EQ(warning.location.file, utf8(second));

    // The index keeps which manifest shadows it.
    EXPECT_EQ(index.entries().at(0).shadowed_by, std::nullopt);
    EXPECT_EQ(index.entries().at(1).shadowed_by, first);

    // The shadowed manifest is never used: its higher minor does not help, and the reason names
    // the manifest that shadows it.
    const auto resolution = index.resolve({"test.echo", 0, 1});
    EXPECT_EQ(resolution.provider, nullptr);
    ASSERT_EQ(resolution.rejected.size(), 2u);
    EXPECT_EQ(resolution.rejected[1].reason.rfind("shadowed by '" + utf8(first) + "'", 0), 0u)
        << resolution.rejected[1].reason;
    const auto found = index.resolve({"test.echo", 0, 0});
    ASSERT_NE(found.provider, nullptr);
    EXPECT_EQ(found.provider->manifest_path, first);
}

TEST(ProviderIndex, AFileReachedTwiceIsIndexedOnce) {
    const ScratchDir scratch;
    write_manifest(scratch.path() / "sub" / "p.provider.json",
                   usable_manifest("p", {"test.echo@0.0"}));
    const ProviderIndex index = ProviderIndex::scan({scratch.path(), scratch.path() / "sub"});
    EXPECT_EQ(names_of(index), std::vector<std::string>{"p"});
    EXPECT_TRUE(index.diagnostics().empty());
}

TEST(ProviderIndex, OtherPlatformManifestsAreUnusableAndShadowNothing) {
    const ScratchDir scratch;
    const auto host = praenora::detail::host_platform();
    json other = usable_manifest("p", {"test.echo@0.0"});
    other["platform"] = {{"os", host.os == "windows" ? "linux" : "windows"}, {"arch", "x86_64"}};
    write_manifest(scratch.path() / "a" / "p.provider.json", other);
    const fs::path here = write_manifest(scratch.path() / "b" / "p.provider.json",
                                         usable_manifest("p", {"test.echo@0.0"}));

    const ProviderIndex index = ProviderIndex::scan({scratch.path() / "a", scratch.path() / "b"});
    ASSERT_EQ(index.entries().size(), 2u);
    EXPECT_TRUE(index.diagnostics().empty()) << "nothing is shadowed";
    const auto& unusable = index.entries()[0];
    ASSERT_TRUE(unusable.problem.has_value());
    EXPECT_TRUE(contains(*unusable.problem, "built for ")) << *unusable.problem;
    EXPECT_TRUE(contains(*unusable.problem, host.os + "-" + host.arch)) << *unusable.problem;
    EXPECT_FALSE(index.entries()[1].problem.has_value());

    const auto resolution = index.resolve({"test.echo", 0, 0});
    ASSERT_NE(resolution.provider, nullptr);
    EXPECT_EQ(resolution.provider->manifest_path, here);
}

TEST(ProviderIndex, UsabilityNeedsThisRuntimeAndProviderInterface) {
    const std::string runtime = praenora::version_info().runtime;
    ProviderManifest manifest = load_provider_manifest(
        praenora::test::fixture_provider_manifest("test-echo"));
    EXPECT_FALSE(provider_problem(manifest).has_value());

    // The runtime version must be equal; build metadata is ignored.
    manifest.runtime_version = praenora::detail::parse_semver(runtime + "+build.7");
    EXPECT_FALSE(provider_problem(manifest).has_value());
    manifest.runtime_version = praenora::detail::parse_semver("0.1.0");
    ASSERT_TRUE(provider_problem(manifest).has_value());
    EXPECT_TRUE(contains(*provider_problem(manifest), "runtime 0.1.0"))
        << *provider_problem(manifest);
    EXPECT_TRUE(contains(*provider_problem(manifest), runtime)) << *provider_problem(manifest);
    manifest.runtime_version = praenora::detail::parse_semver(runtime + "-rc.1");
    EXPECT_TRUE(provider_problem(manifest).has_value()) << "a pre-release is another version";
    manifest.runtime_version = praenora::detail::parse_semver(runtime);

    // The provider interface major must be equal; any minor is accepted.
    manifest.abi_minor = PRAENORA_PROVIDER_ABI_MINOR + 4;
    EXPECT_FALSE(provider_problem(manifest).has_value());
    manifest.abi_major = PRAENORA_PROVIDER_ABI_MAJOR + 1;
    ASSERT_TRUE(provider_problem(manifest).has_value());
    EXPECT_TRUE(contains(*provider_problem(manifest), "provider interface"))
        << *provider_problem(manifest);
}

TEST(ProviderIndex, ResolvesTheFirstUsableProviderThatOffersTheService) {
    const ScratchDir scratch;
    const fs::path low =
        write_manifest(scratch.path() / "a" / "low.provider.json",
                       usable_manifest("low", {"test.echo@0.1", "test.other@2.0"}));
    const fs::path high = write_manifest(scratch.path() / "b" / "high.provider.json",
                                         usable_manifest("high", {"test.echo@0.3"}));
    const ProviderIndex index = ProviderIndex::scan({scratch.path() / "a", scratch.path() / "b"});

    // The first provider in search order whose offered minor is high enough.
    auto resolution = index.resolve({"test.echo", 0, 0});
    ASSERT_NE(resolution.provider, nullptr);
    EXPECT_EQ(resolution.provider->manifest_path, low);
    EXPECT_EQ(resolution.offered_minor, 1u);
    resolution = index.resolve({"test.echo", 0, 1});
    ASSERT_NE(resolution.provider, nullptr);
    EXPECT_EQ(resolution.provider->manifest_path, low);
    resolution = index.resolve({"test.echo", 0, 2});
    ASSERT_NE(resolution.provider, nullptr);
    EXPECT_EQ(resolution.provider->manifest_path, high);
    EXPECT_EQ(resolution.offered_minor, 3u);
    EXPECT_TRUE(resolution.rejected.empty());

    resolution = index.resolve({"test.other", 2, 0});
    ASSERT_NE(resolution.provider, nullptr);
    EXPECT_EQ(resolution.provider->name, "low");
    EXPECT_EQ(index.resolve({"test.other", 1, 0}).provider, nullptr);
    EXPECT_EQ(index.resolve({"test.echo", 0, 4}).provider, nullptr);
    EXPECT_EQ(index.resolve({"test.nothing", 0, 0}).provider, nullptr);
}

TEST(ProviderIndex, AnUnresolvedServiceNamesTheProvidersThatComeClose) {
    const ScratchDir scratch;
    json unusable = usable_manifest("old-runtime", {"test.echo@0.9"});
    unusable["runtime_version"] = "0.0.1";
    write_manifest(scratch.path() / "a.provider.json", unusable);
    write_manifest(scratch.path() / "b.provider.json",
                   usable_manifest("other-major", {"test.echo@1.0"}));
    write_manifest(scratch.path() / "c.provider.json",
                   usable_manifest("low-minor", {"test.echo@0.1"}));
    write_manifest(scratch.path() / "d.provider.json",
                   usable_manifest("unrelated", {"test.unrelated@0.0"}));
    const ProviderIndex index = ProviderIndex::scan({scratch.path()});

    const auto resolution = index.resolve({"test.echo", 0, 2});
    EXPECT_EQ(resolution.provider, nullptr);
    ASSERT_EQ(resolution.rejected.size(), 3u);
    EXPECT_EQ(resolution.rejected[0].provider->name, "old-runtime");
    EXPECT_TRUE(contains(resolution.rejected[0].reason, "runtime 0.0.1"))
        << resolution.rejected[0].reason;
    EXPECT_EQ(resolution.rejected[1].provider->name, "other-major");
    EXPECT_TRUE(contains(resolution.rejected[1].reason, "test.echo@1.0"))
        << resolution.rejected[1].reason;
    EXPECT_EQ(resolution.rejected[2].provider->name, "low-minor");
    EXPECT_TRUE(contains(resolution.rejected[2].reason, "test.echo@0.1"))
        << resolution.rejected[2].reason;

    // The details of SERVICE_UNAVAILABLE: the service, the searched paths and those providers.
    const json details = json::parse(index.unresolved_details({"test.echo", 0, 2}, resolution));
    EXPECT_EQ(details.at("service"), "test.echo");
    EXPECT_EQ(details.at("major"), 0);
    EXPECT_EQ(details.at("minor"), 2);
    EXPECT_EQ(details.at("search_paths"), json::array({utf8(scratch.path().lexically_normal())}));
    ASSERT_EQ(details.at("providers").size(), 3u);
    EXPECT_EQ(details.at("providers")[1].at("name"), "other-major");
    EXPECT_EQ(details.at("providers")[1].at("version"), "0.1.0");
    EXPECT_TRUE(details.at("providers")[1].at("manifest").get<std::string>().size() > 0);
    EXPECT_EQ(details.at("providers")[1].at("reason"), resolution.rejected[1].reason);
}

TEST(ProviderIndex, EntriesKeepManifestsAndProblemsInScanOrder) {
    const ScratchDir scratch;
    json future = usable_manifest("future", {"test.echo@0.0"});
    future["abi"]["major"] = PRAENORA_PROVIDER_ABI_MAJOR + 1;
    write_manifest(scratch.path() / "a.provider.json", future);
    write_manifest(scratch.path() / "b.provider.json", usable_manifest("fine", {"test.echo@0.0"}));
    const ProviderIndex index = ProviderIndex::scan({scratch.path()});
    const auto* unusable = entry_named(index, "future");
    const auto* usable = entry_named(index, "fine");
    ASSERT_NE(unusable, nullptr);
    ASSERT_NE(usable, nullptr);
    EXPECT_TRUE(unusable->problem.has_value());
    EXPECT_FALSE(usable->problem.has_value());
    EXPECT_FALSE(unusable->shadowed_by);
    // The unusable provider is not a candidate.
    const auto resolution = index.resolve({"test.echo", 0, 0});
    ASSERT_NE(resolution.provider, nullptr);
    EXPECT_EQ(resolution.provider->name, "fine");
}
