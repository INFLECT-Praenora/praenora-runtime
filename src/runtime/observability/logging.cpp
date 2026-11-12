#include "observability/logging.hpp"

#include "core/paths.hpp"
#include "core/time.hpp"

#include <praenora/runtime/error.hpp>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace {

// log_event() hands the event to the sinks' formatters through the spdlog payload:
//   kStart <attributes as text> kSplit <attributes as JSON key-value pairs> kStart <message>
// Both renderings escape control characters, so the separators cannot occur inside them. A
// payload without this shape (a direct spdlog call) is a plain message without attributes.
constexpr char kStart = '\x1e';
constexpr char kSplit = '\x1f';

spdlog::level::level_enum to_spdlog(LogLevel level) {
    switch (level) {
    case LogLevel::trace:
        return spdlog::level::trace;
    case LogLevel::debug:
        return spdlog::level::debug;
    case LogLevel::info:
        return spdlog::level::info;
    case LogLevel::warn:
        return spdlog::level::warn;
    case LogLevel::error:
        return spdlog::level::err;
    case LogLevel::critical:
        return spdlog::level::critical;
    case LogLevel::off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

struct SeverityName {
    std::string_view text;
    int number;
};

// OpenTelemetry log data model severity numbers.
SeverityName severity_of(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace:
        return {"TRACE", 1};
    case spdlog::level::debug:
        return {"DEBUG", 5};
    case spdlog::level::info:
        return {"INFO", 9};
    case spdlog::level::warn:
        return {"WARN", 13};
    case spdlog::level::err:
        return {"ERROR", 17};
    case spdlog::level::critical:
        return {"CRITICAL", 21};
    default:
        return {"INFO", 9};
    }
}

// A JSON string; invalid UTF-8 is replaced rather than fatal.
std::string json_string(std::string_view text) {
    return nlohmann::json(std::string(text))
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string json_value(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Text that can appear in a text line unquoted.
bool is_bare(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ':' ||
                        c == '/' || c == '@' || c == '+';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string text_value(const nlohmann::json& value) {
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return is_bare(text) ? text : json_string(text);
    }
    return json_value(value);
}

struct Event {
    std::string_view text_attributes, json_attributes, body;
};

Event decode(std::string_view payload) {
    if (!payload.empty() && payload[0] == kStart) {
        const auto split = payload.find(kSplit, 1);
        const auto end = split == std::string_view::npos ? split : payload.find(kStart, split + 1);
        if (end != std::string_view::npos) {
            return Event{payload.substr(1, split - 1), payload.substr(split + 1, end - split - 1),
                         payload.substr(end + 1)};
        }
    }
    return Event{{}, {}, payload};
}

class LineFormatter final : public spdlog::formatter {
public:
    explicit LineFormatter(LogFormat format) : format_(format) {}

    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        const Event event = decode(std::string_view(msg.payload.data(), msg.payload.size()));
        const std::string_view logger(msg.logger_name.data(), msg.logger_name.size());
        const SeverityName severity = severity_of(msg.level);
        std::string line = format_ == LogFormat::text ? text(msg, event, logger, severity)
                                                      : json(msg, event, logger, severity);
        line += '\n';
        dest.append(line.data(), line.data() + line.size());
    }

    std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<LineFormatter>(format_);
    }

private:
    static std::string timestamp(const spdlog::details::log_msg& msg) {
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(msg.time.time_since_epoch())
                .count();
        try {
            return format_rfc3339_utc(static_cast<std::int64_t>(ns));
        } catch (const std::exception&) {
            return "0000-00-00T00:00:00.000000000Z";
        }
    }

    static std::string text(const spdlog::details::log_msg& msg, const Event& event,
                            std::string_view logger, const SeverityName& severity) {
        std::string line = timestamp(msg);
        line += ' ';
        line += severity.text;
        for (std::size_t i = severity.text.size(); i < 5; ++i) {
            line += ' ';
        }
        line += ' ';
        line += logger;
        line += ' ';
        if (!event.text_attributes.empty()) {
            line += '[';
            line += event.text_attributes;
            line += "] ";
        }
        line += event.body;
        return line;
    }

    static std::string json(const spdlog::details::log_msg& msg, const Event& event,
                            std::string_view logger, const SeverityName& severity) {
        std::string line = "{\"timestamp\":\"" + timestamp(msg) + "\",\"severity_text\":\"";
        line += severity.text;
        line += "\",\"severity_number\":" + std::to_string(severity.number);
        line += ",\"body\":" + json_string(event.body);
        line += ",\"attributes\":{\"logger\":" + json_string(logger);
        if (!event.json_attributes.empty()) {
            line += ',';
            line += event.json_attributes;
        }
        line += "}}";
        return line;
    }

    LogFormat format_;
};

// Appends to a file opened in binary mode through std::filesystem (wide paths on Windows).
class FileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit FileSink(const std::filesystem::path& file) {
        std::error_code ec;
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path(), ec);
        }
        stream_.open(file, std::ios::binary | std::ios::app);
        if (!stream_.is_open()) {
            throw Error(ErrorCode::IO_ERROR, "cannot open the log file '" + path_to_utf8(file) +
                                                 "'" + (ec ? ": " + ec.message() : std::string()));
        }
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        stream_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    }

    void flush_() override { stream_.flush(); }

private:
    std::ofstream stream_;
};

} // namespace

LogHub::LogHub(LogOptions options)
    : options_(std::move(options)), sinks_(std::make_shared<spdlog::sinks::dist_sink_mt>()) {
    if (options_.to_stderr) {
        add_sink(std::make_shared<spdlog::sinks::stderr_sink_mt>());
    }
    if (!options_.file.empty()) {
        add_sink(std::make_shared<FileSink>(path_from_utf8(options_.file)));
    }
}

LogHub::~LogHub() {
    try {
        sinks_->flush();
    } catch (...) {
        // Destructors never throw.
    }
}

std::shared_ptr<spdlog::logger> LogHub::logger(const std::string& name) {
    auto logger = std::make_shared<spdlog::logger>(name, sinks_);
    logger->set_level(to_spdlog(options_.level));
    // Every event reaches the file at once: a crash inside a module takes the process down.
    logger->flush_on(spdlog::level::trace);
    return logger;
}

void LogHub::add_sink(spdlog::sink_ptr sink) {
    sink->set_formatter(make_log_formatter(options_.format));
    sinks_->add_sink(std::move(sink));
}

const LogOptions& LogHub::options() const { return options_; }

void log_event(spdlog::logger& logger, LogLevel level, std::string_view message,
               const std::vector<LogAttribute>& attrs) {
    if (level == LogLevel::off) {
        return;
    }
    const auto spdlog_level = to_spdlog(level);
    if (!logger.should_log(spdlog_level)) {
        return;
    }
    std::string text_attributes;
    std::string json_attributes;
    for (const auto& attr : attrs) {
        if (attr.key == "logger") {
            continue;
        }
        if (!text_attributes.empty()) {
            text_attributes += ' ';
            json_attributes += ',';
        }
        text_attributes +=
            (is_bare(attr.key) ? attr.key : json_string(attr.key)) + "=" + text_value(attr.value);
        json_attributes += json_string(attr.key) + ":" + json_value(attr.value);
    }
    std::string payload;
    payload.reserve(3 + text_attributes.size() + json_attributes.size() + message.size());
    payload += kStart;
    payload += text_attributes;
    payload += kSplit;
    payload += json_attributes;
    payload += kStart;
    payload += message;
    logger.log(spdlog::source_loc{}, spdlog_level,
               spdlog::string_view_t(payload.data(), payload.size()));
}

void set_log_level(spdlog::logger& logger, LogLevel level) { logger.set_level(to_spdlog(level)); }

LogLevel parse_log_level(std::string_view name) {
    static constexpr std::pair<std::string_view, LogLevel> kLevels[] = {
        {"trace", LogLevel::trace}, {"debug", LogLevel::debug}, {"info", LogLevel::info},
        {"warn", LogLevel::warn},   {"error", LogLevel::error}, {"critical", LogLevel::critical},
        {"off", LogLevel::off},
    };
    for (const auto& [text, level] : kLevels) {
        if (text == name) {
            return level;
        }
    }
    throw Error(ErrorCode::CONFIG_INVALID,
                "invalid log level '" + std::string(name) +
                    "': expected trace, debug, info, warn, error, critical or off");
}

LogFormat parse_log_format(std::string_view name) {
    if (name == "text") {
        return LogFormat::text;
    }
    if (name == "json") {
        return LogFormat::json;
    }
    throw Error(ErrorCode::CONFIG_INVALID,
                "invalid log format '" + std::string(name) + "': expected text or json");
}

std::unique_ptr<spdlog::formatter> make_log_formatter(LogFormat format) {
    return std::make_unique<LineFormatter>(format);
}

} // namespace praenora::detail
