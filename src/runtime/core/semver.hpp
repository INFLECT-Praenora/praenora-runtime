// Semantic Versioning 2.0.0: parsing, precedence and the compatibility classes of the
// compatibility policy.
#ifndef PRAENORA_RUNTIME_CORE_SEMVER_HPP
#define PRAENORA_RUNTIME_CORE_SEMVER_HPP

#include <praenora/sdk/version.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace praenora::detail {

struct SemVer {
    std::uint64_t major = 0, minor = 0, patch = 0;
    std::string prerelease; // dot-separated identifiers after '-', empty if none
    std::string build;      // dot-separated identifiers after '+', empty if none
};

// Parses `MAJOR.MINOR.PATCH[-prerelease][+build]` exactly as SemVer 2.0.0 defines it (no leading
// zeros in numeric identifiers, no surrounding whitespace, no "v" prefix). Throws
// praenora::Error(INVALID_ARGUMENT) otherwise.
SemVer parse_semver(std::string_view text);

std::string to_string(const SemVer& version);

// SemVer precedence: <0 when a < b, 0 when equal, >0 when a > b. Build metadata is ignored.
int compare(const SemVer& a, const SemVer& b);

inline bool operator==(const SemVer& a, const SemVer& b) { return compare(a, b) == 0; }
inline bool operator!=(const SemVer& a, const SemVer& b) { return compare(a, b) != 0; }
inline bool operator<(const SemVer& a, const SemVer& b) { return compare(a, b) < 0; }
inline bool operator<=(const SemVer& a, const SemVer& b) { return compare(a, b) <= 0; }
inline bool operator>(const SemVer& a, const SemVer& b) { return compare(a, b) > 0; }
inline bool operator>=(const SemVer& a, const SemVer& b) { return compare(a, b) >= 0; }

// True when a and b are in the same compatibility class: before 1.0 (0.y.z) the same y, from 1.0
// on the same major. A 0.y version and a version of 1.0 or later are never in the same class.
bool same_compatibility_class(const SemVer& a, const SemVer& b);

// The protocol (PMP) version this runtime implements. CMake owns the value
// (cmake/PraenoraVersions.cmake); it reaches C++ through the generated <praenora/sdk/version.h>.
inline constexpr const char* kPmpVersion = PRAENORA_PMP_VERSION_STRING;

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_SEMVER_HPP
