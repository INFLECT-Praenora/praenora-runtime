#include "core/duration.hpp"

#include <praenora/runtime/error.hpp>

#include <cstdint>
#include <limits>
#include <string>

namespace praenora::detail {

namespace {

// A unit is coefficient * 10^exponent nanoseconds.
struct Unit {
    std::string_view name;
    std::uint64_t coefficient;
    int exponent;
};

constexpr Unit kUnits[] = {
    {"ns", 1, 0}, {"us", 1, 3}, {"ms", 1, 6}, {"s", 1, 9}, {"m", 6, 10}, {"h", 36, 11},
};

constexpr std::uint64_t kMaxNanoseconds =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

std::uint64_t pow10(int exponent) {
    std::uint64_t value = 1;
    for (int i = 0; i < exponent; ++i) {
        value *= 10;
    }
    return value;
}

[[noreturn]] void invalid(std::string_view text, const std::string& reason) {
    throw Error(ErrorCode::CONFIG_INVALID,
                "invalid duration '" + std::string(text) + "': " + reason);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

} // namespace

std::chrono::nanoseconds parse_duration(std::string_view text) {
    if (text == "none" || text == "0") {
        return std::chrono::nanoseconds(0);
    }
    const std::string expected =
        "expected <number><unit> with unit ns, us, ms, s, m or h, or 0 or none";
    std::size_t pos = 0;
    while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
    }
    const std::string_view whole = text.substr(0, pos);
    std::string_view fraction;
    if (pos < text.size() && text[pos] == '.') {
        const std::size_t start = ++pos;
        while (pos < text.size() && is_digit(text[pos])) {
            ++pos;
        }
        fraction = text.substr(start, pos - start);
        if (fraction.empty()) {
            invalid(text, expected);
        }
    }
    if (whole.empty()) {
        invalid(text, expected);
    }
    const std::string_view unit_name = text.substr(pos);
    const Unit* unit = nullptr;
    for (const auto& candidate : kUnits) {
        if (candidate.name == unit_name) {
            unit = &candidate;
        }
    }
    if (unit == nullptr) {
        invalid(text, expected);
    }
    const std::uint64_t unit_ns = unit->coefficient * pow10(unit->exponent);

    // Whole part, overflow-checked.
    std::uint64_t value = 0;
    for (const char c : whole) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (kMaxNanoseconds - digit) / 10) {
            invalid(text, "too large");
        }
        value = value * 10 + digit;
    }
    if (value > kMaxNanoseconds / unit_ns) {
        invalid(text, "too large");
    }
    std::uint64_t total = value * unit_ns;

    // Fraction: trailing zeros do not change the value. What remains must make whole
    // nanoseconds: fraction * coefficient * 10^(exponent - digits) must be an integer.
    while (!fraction.empty() && fraction.back() == '0') {
        fraction.remove_suffix(1);
    }
    if (!fraction.empty()) {
        const int digits = static_cast<int>(fraction.size());
        // With trailing zeros removed, more than exponent + 2 digits can never come to whole
        // nanoseconds (the coefficients are 1, 6 and 36).
        if (digits > unit->exponent + 2) {
            invalid(text, "not a whole number of nanoseconds");
        }
        std::uint64_t numerator = 0;
        for (const char c : fraction) {
            numerator = numerator * 10 + static_cast<std::uint64_t>(c - '0');
        }
        numerator *= unit->coefficient;
        std::uint64_t part = 0;
        if (digits <= unit->exponent) {
            part = numerator * pow10(unit->exponent - digits);
        } else {
            const std::uint64_t divisor = pow10(digits - unit->exponent);
            if (numerator % divisor != 0) {
                invalid(text, "not a whole number of nanoseconds");
            }
            part = numerator / divisor;
        }
        if (part > kMaxNanoseconds - total) {
            invalid(text, "too large");
        }
        total += part;
    }
    return std::chrono::nanoseconds(static_cast<std::int64_t>(total));
}

std::string format_duration(std::chrono::nanoseconds duration) {
    const std::int64_t count = duration.count();
    if (count <= 0) {
        return "none";
    }
    constexpr struct {
        std::int64_t nanoseconds;
        const char* name;
    } kExact[] = {{1'000'000'000, "s"}, {1'000'000, "ms"}, {1'000, "us"}};
    for (const auto& unit : kExact) {
        if (count % unit.nanoseconds == 0) {
            return std::to_string(count / unit.nanoseconds) + unit.name;
        }
    }
    return std::to_string(count) + "ns";
}

} // namespace praenora::detail
