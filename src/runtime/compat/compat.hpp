// The compatibility rules between the runtime and a module, decided from the module's manifest
// before any of its code is loaded.
#ifndef PRAENORA_RUNTIME_COMPAT_COMPAT_HPP
#define PRAENORA_RUNTIME_COMPAT_COMPAT_HPP

#include "discovery/manifest.hpp"

#include <praenora/runtime/error.hpp>

#include <optional>
#include <string>

namespace praenora::detail {

// The platform the runtime was compiled for, in manifest vocabulary: os linux|macos|windows, arch
// x86_64|arm64 ("unknown" for anything else, which no manifest can match).
struct HostPlatform {
    std::string os, arch;
};

HostPlatform host_platform();

// The first rule `manifest` breaks for a step that requires `required`, as an error diagnostic
// located in the manifest, or nullopt when the module is compatible. The rules, in this order:
//   1. platform: os and arch equal the host's (PLATFORM_MISMATCH);
//   2. ABI: the major equals the runtime's ABI major; any minor is accepted, because the tables'
//      struct_size negotiates it (ABI_INCOMPATIBLE);
//   3. protocol: pmp_version is in the runtime's compatibility class (0.y.* with 0.y.* before 1.0,
//      the same major afterwards) and not newer than the runtime's (PROTOCOL_INCOMPATIBLE);
//   4. capability: the manifest lists required.id at exactly required.version (build metadata
//      ignored) (CAPABILITY_NOT_IMPLEMENTED).
std::optional<Diagnostic> check_module_compat(const ModuleManifest& manifest,
                                              const CapabilityRef& required);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_COMPAT_COMPAT_HPP
