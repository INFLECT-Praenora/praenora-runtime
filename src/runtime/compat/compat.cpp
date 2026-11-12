#include "compat/compat.hpp"

#include "core/paths.hpp"

#include <praenora/sdk/abi.h>

#include <utility>

namespace praenora::detail {

namespace {

Diagnostic incompatible(const ModuleManifest& manifest, ErrorCode code, std::string pointer,
                        const std::string& message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::error;
    diagnostic.code = code;
    diagnostic.module = manifest.name + "@" + to_string(manifest.version);
    diagnostic.message = "module " + diagnostic.module + ": " + message;
    diagnostic.location.file = path_to_utf8(manifest.manifest_path);
    diagnostic.location.pointer = std::move(pointer);
    return diagnostic;
}

std::string capability_name(const CapabilityRef& capability) {
    return capability.id + "@" + to_string(capability.version);
}

} // namespace

HostPlatform host_platform() {
    HostPlatform host;
#if defined(_WIN32)
    host.os = "windows";
#elif defined(__APPLE__)
    host.os = "macos";
#elif defined(__linux__)
    host.os = "linux";
#else
    host.os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
    host.arch = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    host.arch = "arm64";
#else
    host.arch = "unknown";
#endif
    return host;
}

std::optional<Diagnostic> check_module_compat(const ModuleManifest& manifest,
                                              const CapabilityRef& required) {
    const HostPlatform host = host_platform();
    if (manifest.os != host.os || manifest.arch != host.arch) {
        return incompatible(manifest, ErrorCode::PLATFORM_MISMATCH, "/platform",
                            "built for " + manifest.os + "-" + manifest.arch + "; this host is " +
                                host.os + "-" + host.arch);
    }

    if (manifest.abi_major != PRAENORA_ABI_MAJOR) {
        return incompatible(
            manifest, ErrorCode::ABI_INCOMPATIBLE, "/abi/major",
            "uses ABI " + std::to_string(manifest.abi_major) + "." +
                std::to_string(manifest.abi_minor) + "; this runtime implements ABI " +
                std::to_string(PRAENORA_ABI_MAJOR) + "." + std::to_string(PRAENORA_ABI_MINOR) +
                " and accepts only the same major");
    }

    const SemVer runtime_pmp = parse_semver(kPmpVersion);
    if (!same_compatibility_class(manifest.pmp_version, runtime_pmp) ||
        manifest.pmp_version > runtime_pmp) {
        return incompatible(manifest, ErrorCode::PROTOCOL_INCOMPATIBLE, "/pmp_version",
                            "uses PMP " + to_string(manifest.pmp_version) +
                                "; this runtime implements PMP " + kPmpVersion +
                                " and accepts versions of its compatibility class that are not "
                                "newer");
    }

    std::string implemented;
    for (const auto& capability : manifest.capabilities) {
        if (capability.id == required.id && capability.version == required.version) {
            return std::nullopt;
        }
        implemented += (implemented.empty() ? "" : ", ") + capability_name(capability);
    }
    return incompatible(manifest, ErrorCode::CAPABILITY_NOT_IMPLEMENTED, "/capabilities",
                        "does not implement capability " + capability_name(required) +
                            "; it implements " +
                            (implemented.empty() ? std::string("none") : implemented));
}

} // namespace praenora::detail
