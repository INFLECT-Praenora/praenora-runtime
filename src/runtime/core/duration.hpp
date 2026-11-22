// Configuration durations: `<number><unit>` with unit ns, us, ms, s, m or h.
#ifndef PRAENORA_RUNTIME_CORE_DURATION_HPP
#define PRAENORA_RUNTIME_CORE_DURATION_HPP

#include <chrono>
#include <string>
#include <string_view>

namespace praenora::detail {

// Parses a configuration duration. The number is `[0-9]+(\.[0-9]+)?` and must come to a whole
// number of nanoseconds; "none" and "0" give 0, which means "disabled" for timeouts. Throws
// praenora::Error(CONFIG_INVALID) for anything else, including negative values, a missing or
// unknown unit, surrounding whitespace and values that do not fit in 64-bit nanoseconds.
std::chrono::nanoseconds parse_duration(std::string_view text);

// The canonical text of a duration, which parse_duration() reads back: the count in the largest of
// s, ms, us and ns that holds it exactly ("300s", "1500ms", "7ns"), and "none" for 0 or less
// (disabled).
std::string format_duration(std::chrono::nanoseconds duration);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_DURATION_HPP
