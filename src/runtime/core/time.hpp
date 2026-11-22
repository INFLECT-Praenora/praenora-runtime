// Wall-clock instants: int64 nanoseconds since the Unix epoch (UTC, leap seconds ignored), written
// as RFC 3339 UTC with exactly nine fractional digits. Wall-clock time belongs only in logs, status
// and the execution section of the run record.
#ifndef PRAENORA_RUNTIME_CORE_TIME_HPP
#define PRAENORA_RUNTIME_CORE_TIME_HPP

#include <cstdint>
#include <string>

namespace praenora::detail {

// The current wall-clock time from std::chrono::system_clock.
std::int64_t now_unix_ns();

// For example 1700000000123456789 -> "2023-11-14T22:13:20.123456789Z". Throws
// praenora::Error(INVALID_ARGUMENT) if the platform cannot represent the instant.
std::string format_rfc3339_utc(std::int64_t unix_ns);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_TIME_HPP
