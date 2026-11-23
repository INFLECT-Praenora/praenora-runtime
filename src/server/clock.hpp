// The wall clock of the control API: instants as int64 nanoseconds since the Unix epoch (UTC, leap
// seconds ignored), written as RFC 3339 UTC with nine fractional digits, as the runtime writes
// them. Wall-clock time is reported (started_at, created_at), never given to a run.
#ifndef PRAENORA_SERVER_CLOCK_HPP
#define PRAENORA_SERVER_CLOCK_HPP

#include <cstdint>
#include <string>

namespace praenora::server {

// The current wall-clock time from std::chrono::system_clock.
std::int64_t now_unix_ns();

// For example 1700000000123456789 -> "2023-11-14T22:13:20.123456789Z"; instants before 1970 too.
std::string format_rfc3339_utc(std::int64_t unix_ns);

} // namespace praenora::server

#endif // PRAENORA_SERVER_CLOCK_HPP
