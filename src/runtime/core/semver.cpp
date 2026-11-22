#include "core/semver.hpp"

#include <praenora/runtime/error.hpp>

#include <limits>
#include <vector>

namespace praenora::detail {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_identifier_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool all_digits(std::string_view s) {
    for (const char c : s) {
        if (!is_digit(c)) {
            return false;
        }
    }
    return !s.empty();
}

[[noreturn]] void invalid(std::string_view text, const std::string& reason) {
    throw Error(ErrorCode::INVALID_ARGUMENT,
                "invalid version '" + std::string(text) + "': " + reason);
}

std::uint64_t parse_number(std::string_view text, std::string_view part, const char* what) {
    if (!all_digits(part)) {
        invalid(text, std::string(what) + " must be a non-negative integer");
    }
    if (part.size() > 1 && part[0] == '0') {
        invalid(text, std::string(what) + " must not have leading zeros");
    }
    std::uint64_t value = 0;
    for (const char c : part) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            invalid(text, std::string(what) + " is too large");
        }
        value = value * 10 + digit;
    }
    return value;
}

std::vector<std::string_view> split(std::string_view s, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = s.find(separator, start);
        parts.push_back(s.substr(start, end == std::string_view::npos ? s.npos : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

void check_identifiers(std::string_view text, std::string_view identifiers, bool prerelease) {
    const char* what = prerelease ? "pre-release" : "build metadata";
    for (const auto identifier : split(identifiers, '.')) {
        if (identifier.empty()) {
            invalid(text, std::string(what) + " has an empty identifier");
        }
        for (const char c : identifier) {
            if (!is_identifier_char(c)) {
                invalid(text, std::string(what) + " may contain only [0-9A-Za-z-]");
            }
        }
        if (prerelease && all_digits(identifier) && identifier.size() > 1 && identifier[0] == '0') {
            invalid(text, "numeric pre-release identifiers must not have leading zeros");
        }
    }
}

// Numeric identifiers compare numerically (they have no leading zeros, so by length first),
// alphanumeric ones in ASCII order, and numeric ones before alphanumeric ones.
int compare_identifier(std::string_view a, std::string_view b) {
    const bool a_numeric = all_digits(a);
    const bool b_numeric = all_digits(b);
    if (a_numeric && b_numeric) {
        if (a.size() != b.size()) {
            return a.size() < b.size() ? -1 : 1;
        }
        return a.compare(b) < 0 ? -1 : (a.compare(b) > 0 ? 1 : 0);
    }
    if (a_numeric != b_numeric) {
        return a_numeric ? -1 : 1;
    }
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

int compare_number(std::uint64_t a, std::uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }

} // namespace

SemVer parse_semver(std::string_view text) {
    std::string_view rest = text;
    std::string_view build;
    if (const auto plus = rest.find('+'); plus != std::string_view::npos) {
        build = rest.substr(plus + 1);
        rest = rest.substr(0, plus);
        if (build.empty()) {
            invalid(text, "empty build metadata");
        }
        check_identifiers(text, build, false);
    }
    std::string_view prerelease;
    if (const auto dash = rest.find('-'); dash != std::string_view::npos) {
        prerelease = rest.substr(dash + 1);
        rest = rest.substr(0, dash);
        if (prerelease.empty()) {
            invalid(text, "empty pre-release");
        }
        check_identifiers(text, prerelease, true);
    }
    const auto core = split(rest, '.');
    if (core.size() != 3) {
        invalid(text, "expected MAJOR.MINOR.PATCH");
    }
    SemVer version;
    version.major = parse_number(text, core[0], "major version");
    version.minor = parse_number(text, core[1], "minor version");
    version.patch = parse_number(text, core[2], "patch version");
    version.prerelease = std::string(prerelease);
    version.build = std::string(build);
    return version;
}

std::string to_string(const SemVer& version) {
    std::string text = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                       std::to_string(version.patch);
    if (!version.prerelease.empty()) {
        text += "-" + version.prerelease;
    }
    if (!version.build.empty()) {
        text += "+" + version.build;
    }
    return text;
}

int compare(const SemVer& a, const SemVer& b) {
    if (const int c = compare_number(a.major, b.major); c != 0) {
        return c;
    }
    if (const int c = compare_number(a.minor, b.minor); c != 0) {
        return c;
    }
    if (const int c = compare_number(a.patch, b.patch); c != 0) {
        return c;
    }
    // A version without a pre-release has higher precedence than one with.
    if (a.prerelease.empty() || b.prerelease.empty()) {
        if (a.prerelease.empty() && b.prerelease.empty()) {
            return 0;
        }
        return a.prerelease.empty() ? 1 : -1;
    }
    const auto left = split(a.prerelease, '.');
    const auto right = split(b.prerelease, '.');
    for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
        if (const int c = compare_identifier(left[i], right[i]); c != 0) {
            return c;
        }
    }
    return compare_number(left.size(), right.size());
}

bool same_compatibility_class(const SemVer& a, const SemVer& b) {
    if (a.major == 0 || b.major == 0) {
        return a.major == b.major && a.minor == b.minor;
    }
    return a.major == b.major;
}

} // namespace praenora::detail
