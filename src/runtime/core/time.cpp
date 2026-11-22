#include "core/time.hpp"

#include <praenora/runtime/error.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>

namespace praenora::detail {

std::int64_t now_unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string format_rfc3339_utc(std::int64_t unix_ns) {
    constexpr std::int64_t kPerSecond = 1000000000;
    // Floor division, so instants before 1970 keep a fraction in [0, 1 s).
    std::int64_t seconds = unix_ns / kPerSecond;
    std::int64_t nanos = unix_ns % kPerSecond;
    if (nanos < 0) {
        nanos += kPerSecond;
        seconds -= 1;
    }
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
#ifdef _WIN32
    const bool ok = gmtime_s(&utc, &time) == 0;
#else
    const bool ok = gmtime_r(&time, &utc) != nullptr;
#endif
    if (!ok) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "cannot represent the instant " + std::to_string(unix_ns) + " ns as UTC time");
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  utc.tm_sec, static_cast<long long>(nanos));
    return buffer;
}

} // namespace praenora::detail
