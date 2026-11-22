// Logging through spdlog, in the runtime's own logger objects: nothing is registered in spdlog's
// global registry and spdlog's default logger is never touched, so a host application that uses
// spdlog itself is unaffected.
//
// Line formats:
//   text  <rfc3339> <LEVEL padded to 5> <logger> [k=v ...] <message>
//   json  {"timestamp","severity_text","severity_number","body","attributes":{"logger",...}}
// Severity texts are TRACE, DEBUG, INFO, WARN, ERROR and CRITICAL; severity numbers follow the
// OpenTelemetry log data model: 1, 5, 9, 13, 17 and 21.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_LOGGING_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_LOGGING_HPP

#include <praenora/runtime/options.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/formatter.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// Owns the sinks of one runtime: standard error without colors (LogOptions::to_stderr) and an
// optional file (LogOptions::file; appended to, parent directories created, written in binary
// mode). Every logger it builds writes to all of its sinks, including sinks added later.
class LogHub {
public:
    // Throws praenora::Error(IO_ERROR) if the log file cannot be opened.
    explicit LogHub(LogOptions options);
    ~LogHub();
    LogHub(const LogHub&) = delete;
    LogHub& operator=(const LogHub&) = delete;

    // A new logger named `name` (for example "praenora.engine" or "module.<step>") at the hub's
    // level. It is not registered anywhere; it stays usable after the hub is gone.
    std::shared_ptr<spdlog::logger> logger(const std::string& name);

    // Adds a sink (for example a test's capture sink) and gives it the hub's line format.
    void add_sink(spdlog::sink_ptr sink);

    // Calls `callback` when the hub is destroyed, before its sinks are: for holders of the hub's
    // loggers that outlive it (service providers, which live until the process ends), so they let
    // go of the hub's sinks and its file. Thread-safe; callbacks must not throw.
    void on_destroy(std::function<void()> callback);

    const LogOptions& options() const;

private:
    LogOptions options_;
    std::shared_ptr<spdlog::sinks::dist_sink<std::mutex>> sinks_;
    std::mutex on_destroy_mutex_;
    std::vector<std::function<void()>> on_destroy_; // guarded by on_destroy_mutex_
};

// One attribute of a log event, for example {"tick", 3}.
struct LogAttribute {
    std::string key;
    nlohmann::json value;
};

// Logs `message` with `attrs` when `logger` lets `level` through. LogLevel::off logs nothing. The
// attribute "logger" is reserved for the logger's name; an attribute with that key is dropped.
void log_event(spdlog::logger& logger, LogLevel level, std::string_view message,
               const std::vector<LogAttribute>& attrs = {});

// An event that log_event() logged, taken apart again from what a sink receives (the payload of
// spdlog's message): its attributes, in the order they were given, and its message. A payload that
// log_event() did not make (a direct spdlog call) is a message without attributes. A module host
// passes its providers' events on to the runtime this way.
struct LoggedEvent {
    std::vector<LogAttribute> attributes;
    std::string message;
};
LoggedEvent decode_logged_event(std::string_view payload);

// Sets the level of one logger, for example a step's `log_level`.
void set_log_level(spdlog::logger& logger, LogLevel level);

// The level of a PRAENORA_LOG_* value of the ABI (<praenora/sdk/abi.h>), as modules and providers
// pass it: values below PRAENORA_LOG_TRACE count as trace, above PRAENORA_LOG_CRITICAL as critical.
LogLevel log_level_from_abi(std::int32_t level) noexcept;

// Whether `logger` records events at `level` (never at LogLevel::off).
bool log_enabled(const spdlog::logger& logger, LogLevel level) noexcept;

// "trace", "debug", "info", "warn", "error", "critical" or "off" (lower case only). Throws
// praenora::Error(CONFIG_INVALID) for anything else.
LogLevel parse_log_level(std::string_view name);

// "text" or "json". Throws praenora::Error(CONFIG_INVALID) for anything else.
LogFormat parse_log_format(std::string_view name);

// A formatter that writes events in `format`, one line each ending in '\n'.
std::unique_ptr<spdlog::formatter> make_log_formatter(LogFormat format);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_LOGGING_HPP
