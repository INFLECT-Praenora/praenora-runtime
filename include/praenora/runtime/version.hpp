// Versions of the runtime and the facts of the build it came from.
#ifndef PRAENORA_RUNTIME_VERSION_HPP
#define PRAENORA_RUNTIME_VERSION_HPP

#include <praenora/runtime/export.h>

#include <cstdint>
#include <map>
#include <string>

namespace praenora {

struct VersionInfo {
    std::string runtime, sdk, pmp;              // package, module SDK and protocol (PMP) versions
    std::uint32_t abi_major = 0, abi_minor = 0; // module ABI version
    std::map<std::string, std::string> formats; // document kind -> its format version
    std::string platform, compiler, build_type; // e.g. "macos-arm64", "AppleClang 17.0.0", "Debug"
};

// The versions and build facts of this runtime library.
PRAENORA_RUNTIME_API VersionInfo version_info();

// version_info() as one compact JSON object with the keys runtime, sdk, pmp,
// abi {major, minor}, formats, platform, compiler and build_type.
PRAENORA_RUNTIME_API std::string version_info_json();

} // namespace praenora

#endif // PRAENORA_RUNTIME_VERSION_HPP
