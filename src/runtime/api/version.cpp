#include <praenora/runtime/version.hpp>

#include <nlohmann/json.hpp>

// The build defines the PRAENORA_BUILD_* facts for the runtime's sources
// (src/runtime/CMakeLists.txt).
#ifndef PRAENORA_BUILD_RUNTIME_VERSION
#error "version.cpp must be compiled with the PRAENORA_BUILD_* definitions of the build"
#endif

namespace praenora {

namespace {

struct FormatVersion {
    const char* kind;
    const char* version;
};

// Every document kind that carries a format_version, with the format version this runtime writes
// and reads (cmake/PraenoraVersions.cmake, generated into format_versions.inc).
constexpr FormatVersion kFormatVersions[] = {
#include "format_versions.inc"
};

} // namespace

VersionInfo version_info() {
    VersionInfo info;
    info.runtime = PRAENORA_BUILD_RUNTIME_VERSION;
    info.sdk = PRAENORA_BUILD_SDK_VERSION;
    info.pmp = PRAENORA_BUILD_PMP_VERSION;
    info.abi_major = PRAENORA_BUILD_ABI_MAJOR;
    info.abi_minor = PRAENORA_BUILD_ABI_MINOR;
    for (const auto& format : kFormatVersions) {
        info.formats.emplace(format.kind, format.version);
    }
    info.platform = PRAENORA_BUILD_PLATFORM;
    info.compiler = PRAENORA_BUILD_COMPILER;
    info.build_type = PRAENORA_BUILD_TYPE;
    return info;
}

std::string version_info_json() {
    const VersionInfo info = version_info();
    nlohmann::json json = nlohmann::json::object();
    json["runtime"] = info.runtime;
    json["sdk"] = info.sdk;
    json["pmp"] = info.pmp;
    json["abi"]["major"] = info.abi_major;
    json["abi"]["minor"] = info.abi_minor;
    json["formats"] = info.formats;
    json["platform"] = info.platform;
    json["compiler"] = info.compiler;
    json["build_type"] = info.build_type;
    return json.dump();
}

} // namespace praenora
