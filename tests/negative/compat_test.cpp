// The compatibility rules between the runtime and a module's manifest (check_module_compat), on
// manifests built in memory from one compatible manifest.
#include "compat/compat.hpp"
#include "core/semver.hpp"
#include "discovery/manifest.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>

using praenora::ErrorCode;
using praenora::Severity;
using praenora::detail::CapabilityRef;
using praenora::detail::check_module_compat;
using praenora::detail::host_platform;
using praenora::detail::ModuleManifest;
using praenora::detail::parse_semver;

namespace {

// A manifest of module fx-compat@1.2.3 that this runtime accepts for test.echo@0.1.0.
ModuleManifest compatible_manifest() {
    ModuleManifest manifest;
    manifest.manifest_path = "/modules/fx-compat/fx-compat.pmm.json";
    manifest.name = "fx-compat";
    manifest.version = parse_semver("1.2.3");
    manifest.abi_major = 0;
    manifest.abi_minor = 0;
    manifest.pmp_version = parse_semver("0.1.0");
    manifest.sdk_version = parse_semver("0.1.0");
    manifest.os = host_platform().os;
    manifest.arch = host_platform().arch;
    manifest.capabilities = {{"test.other", parse_semver("2.0.0")},
                             {"test.echo", parse_semver("0.1.0")}};
    return manifest;
}

const CapabilityRef kEcho{"test.echo", parse_semver("0.1.0")};

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The diagnostic for `manifest`, which must be incompatible with `code`.
praenora::Diagnostic expect_incompatible(const ModuleManifest& manifest, ErrorCode code,
                                         const CapabilityRef& required = kEcho) {
    const std::optional<praenora::Diagnostic> diagnostic = check_module_compat(manifest, required);
    if (!diagnostic) {
        ADD_FAILURE() << "the module was accepted";
        return {};
    }
    EXPECT_EQ(diagnostic->code, code) << diagnostic->message;
    EXPECT_EQ(diagnostic->severity, Severity::error);
    EXPECT_EQ(diagnostic->module, manifest.name + "@" + to_string(manifest.version));
    EXPECT_EQ(diagnostic->location.file, manifest.manifest_path.u8string());
    return *diagnostic;
}

} // namespace

TEST(Compat, HostPlatformMatchesTheBuild) {
    const auto host = host_platform();
    EXPECT_EQ(host.os + "-" + host.arch, praenora::version_info().platform);
}

TEST(Compat, AcceptsMatchingModule) {
    const ModuleManifest manifest = compatible_manifest();
    const auto diagnostic = check_module_compat(manifest, kEcho);
    EXPECT_FALSE(diagnostic.has_value()) << diagnostic->message;

    // Build metadata is ignored, a newer ABI minor is negotiated, an older PMP patch is fine.
    ModuleManifest variant = manifest;
    variant.abi_minor = 3;
    variant.capabilities.back().version = parse_semver("0.1.0+build.7");
    EXPECT_FALSE(check_module_compat(variant, kEcho).has_value());
    EXPECT_FALSE(check_module_compat(manifest, {"test.other", parse_semver("2.0.0")}).has_value());
}

TEST(Compat, WrongPlatform) {
    ModuleManifest manifest = compatible_manifest();
    manifest.os = manifest.os == "windows" ? "linux" : "windows";
    const auto diagnostic = expect_incompatible(manifest, ErrorCode::PLATFORM_MISMATCH);
    EXPECT_EQ(diagnostic.location.pointer, "/platform");
    EXPECT_TRUE(contains(diagnostic.message, manifest.os + "-" + manifest.arch))
        << diagnostic.message;

    ModuleManifest other_arch = compatible_manifest();
    other_arch.arch = other_arch.arch == "arm64" ? "x86_64" : "arm64";
    expect_incompatible(other_arch, ErrorCode::PLATFORM_MISMATCH);

    // The platform is checked first.
    manifest.abi_major = 7;
    manifest.pmp_version = parse_semver("9.0.0");
    manifest.capabilities.clear();
    expect_incompatible(manifest, ErrorCode::PLATFORM_MISMATCH);
}

TEST(Compat, AbiMajorSeven) {
    ModuleManifest manifest = compatible_manifest();
    manifest.abi_major = 7;
    const auto diagnostic = expect_incompatible(manifest, ErrorCode::ABI_INCOMPATIBLE);
    EXPECT_EQ(diagnostic.location.pointer, "/abi/major");
    EXPECT_TRUE(contains(diagnostic.message, "7")) << diagnostic.message;

    // Checked before the protocol and the capability.
    manifest.pmp_version = parse_semver("9.0.0");
    manifest.capabilities.clear();
    expect_incompatible(manifest, ErrorCode::ABI_INCOMPATIBLE);
}

TEST(Compat, ProtocolNineIsIncompatible) {
    ModuleManifest manifest = compatible_manifest();
    manifest.pmp_version = parse_semver("9.0.0");
    const auto diagnostic = expect_incompatible(manifest, ErrorCode::PROTOCOL_INCOMPATIBLE);
    EXPECT_EQ(diagnostic.location.pointer, "/pmp_version");
    EXPECT_TRUE(contains(diagnostic.message, "9.0.0")) << diagnostic.message;

    // Another 0.y class is incompatible too, older or newer.
    manifest.pmp_version = parse_semver("0.2.0");
    expect_incompatible(manifest, ErrorCode::PROTOCOL_INCOMPATIBLE);
    manifest.pmp_version = parse_semver("0.0.9");
    expect_incompatible(manifest, ErrorCode::PROTOCOL_INCOMPATIBLE);

    // Checked before the capability.
    manifest.capabilities.clear();
    expect_incompatible(manifest, ErrorCode::PROTOCOL_INCOMPATIBLE);
}

TEST(Compat, ProtocolZeroOnePatchNewerThanHostIsIncompatible) {
    ASSERT_EQ(std::string(praenora::detail::kPmpVersion), "0.1.0");
    ModuleManifest manifest = compatible_manifest();
    manifest.pmp_version = parse_semver("0.1.1");
    const auto diagnostic = expect_incompatible(manifest, ErrorCode::PROTOCOL_INCOMPATIBLE);
    EXPECT_TRUE(contains(diagnostic.message, "0.1.1")) << diagnostic.message;
    EXPECT_TRUE(contains(diagnostic.message, "0.1.0")) << diagnostic.message;

    // A pre-release of the runtime's own version is older, hence compatible.
    manifest.pmp_version = parse_semver("0.1.0-rc.1");
    EXPECT_FALSE(check_module_compat(manifest, kEcho).has_value());
}

TEST(Compat, UnknownCapability) {
    const ModuleManifest manifest = compatible_manifest();
    const auto diagnostic = expect_incompatible(manifest, ErrorCode::CAPABILITY_NOT_IMPLEMENTED,
                                                {"test.unknown", parse_semver("0.1.0")});
    EXPECT_EQ(diagnostic.location.pointer, "/capabilities");
    EXPECT_TRUE(contains(diagnostic.message, "test.unknown@0.1.0")) << diagnostic.message;
    EXPECT_TRUE(contains(diagnostic.message, "test.echo@0.1.0")) << diagnostic.message;
}

TEST(Compat, CapabilityVersionMismatch) {
    const ModuleManifest manifest = compatible_manifest();
    // Exact versions only: neither an older nor a newer version of the capability is accepted.
    for (const char* version : {"0.1.1", "0.2.0", "0.0.9", "1.0.0"}) {
        SCOPED_TRACE(version);
        const auto diagnostic = expect_incompatible(manifest, ErrorCode::CAPABILITY_NOT_IMPLEMENTED,
                                                    {"test.echo", parse_semver(version)});
        EXPECT_TRUE(contains(diagnostic.message, std::string("test.echo@") + version))
            << diagnostic.message;
        EXPECT_TRUE(contains(diagnostic.message, "test.echo@0.1.0")) << diagnostic.message;
    }
}
