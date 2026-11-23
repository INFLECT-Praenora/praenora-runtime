#include "clock.hpp"

#include <chrono>
#include <cstdio>

namespace praenora::server {

std::int64_t now_unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string format_rfc3339_utc(std::int64_t unix_ns) {
    constexpr std::int64_t kPerSecond = 1000000000;
    constexpr std::int64_t kPerDay = 86400;
    // Floor divisions, so instants before 1970 keep a fraction and a time of day >= 0.
    std::int64_t seconds = unix_ns / kPerSecond;
    std::int64_t nanos = unix_ns % kPerSecond;
    if (nanos < 0) {
        nanos += kPerSecond;
        seconds -= 1;
    }
    std::int64_t days = seconds / kPerDay;
    std::int64_t of_day = seconds % kPerDay;
    if (of_day < 0) {
        of_day += kPerDay;
        days -= 1;
    }
    // The civil date of a day count (Howard Hinnant's civil_from_days, proleptic Gregorian), with
    // no call to the C library's time functions, whose range differs between platforms.
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const std::int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%09lldZ",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(of_day / 3600),
                  static_cast<long long>(of_day / 60 % 60), static_cast<long long>(of_day % 60),
                  static_cast<long long>(nanos));
    return buffer;
}

} // namespace praenora::server
