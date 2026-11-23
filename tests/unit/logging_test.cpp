// Logging: the LogHub's sinks and formats (text and OpenTelemetry-style JSON lines), level
// filtering and isolation from spdlog's global registry.
#include "observability/logging.hpp"

#include "log_capture.hpp"
#include "temp_dir.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/sdk/abi.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::LogOptions;
using praenora::detail::log_event;
using praenora::detail::LogHub;
using praenora::test::LogCapture;
using praenora::test::ScratchDir;

namespace {

LogOptions quiet(LogFormat format, LogLevel level = LogLevel::info) {
    LogOptions options;
    options.format = format;
    options.level = level;
    options.to_stderr = false;
    return options;
}

const std::regex kTimestamp(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z)");

std::vector<std::string> read_lines(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST(Logging, JsonLineHasOtelFields) {
    LogHub hub(quiet(LogFormat::json));
    const LogCapture capture(hub);
    const auto logger = hub.logger("praenora.engine");
    log_event(*logger, LogLevel::info, "invocation completed",
              {{"praenora.tick", 3},
               {"praenora.step", "estimate"},
               {"praenora.module", "example-estimator@0.1.0"},
               {"praenora.duration_ns", 1234567}});

    const auto lines = capture.lines();
    ASSERT_EQ(lines.size(), 1u);
    // One JSON object per line, fields in the documented order.
    EXPECT_EQ(lines[0].find('\n'), std::string::npos);
    EXPECT_EQ(lines[0].rfind(R"({"timestamp":")", 0), 0u) << lines[0];

    const auto entries = capture.json_lines();
    ASSERT_EQ(entries.size(), 1u);
    const auto& entry = entries[0];
    EXPECT_TRUE(std::regex_match(entry.at("timestamp").get<std::string>(), kTimestamp))
        << entry.dump();
    EXPECT_EQ(entry.at("severity_text"), "INFO");
    EXPECT_EQ(entry.at("severity_number"), 9);
    EXPECT_EQ(entry.at("body"), "invocation completed");
    const auto& attributes = entry.at("attributes");
    EXPECT_EQ(attributes.at("logger"), "praenora.engine");
    EXPECT_EQ(attributes.at("praenora.tick"), 3);
    EXPECT_EQ(attributes.at("praenora.step"), "estimate");
    EXPECT_EQ(attributes.at("praenora.module"), "example-estimator@0.1.0");
    EXPECT_EQ(attributes.at("praenora.duration_ns"), 1234567);
    EXPECT_EQ(entry.size(), 5u) << entry.dump();
}

TEST(Logging, TextLineContainsAttributes) {
    LogHub hub(quiet(LogFormat::text));
    const LogCapture capture(hub);
    const auto logger = hub.logger("praenora.engine");
    log_event(*logger, LogLevel::info, "invocation completed (1.2 ms)",
              {{"praenora.tick", 3}, {"praenora.step", "estimate"}});
    log_event(*logger, LogLevel::warn, "no attributes");
    log_event(*logger, LogLevel::error, "quoted", {{"path", "a dir/x"}, {"ok", true}});

    const auto lines = capture.lines();
    ASSERT_EQ(lines.size(), 3u);
    const std::regex line(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z INFO  praenora\.engine )"
        R"(\[praenora\.tick=3 praenora\.step=estimate\] invocation completed \(1\.2 ms\)$)");
    EXPECT_TRUE(std::regex_match(lines[0], line)) << lines[0];
    EXPECT_NE(lines[1].find(" WARN  praenora.engine no attributes"), std::string::npos) << lines[1];
    EXPECT_NE(lines[2].find(" ERROR praenora.engine [path=\"a dir/x\" ok=true] quoted"),
              std::string::npos)
        << lines[2];
    // json_lines() renders the same events as JSON whatever the hub's format.
    EXPECT_EQ(capture.json_lines().at(2).at("attributes").at("path"), "a dir/x");

    // Messages are not format strings, structured values stay JSON, and "logger" is reserved.
    log_event(*logger, LogLevel::info, "braces {} {0} %s",
              {{"obj", json::parse(R"({"a":[1,2]})")}, {"logger", "ignored"}});
    const auto braces = capture.lines().at(3);
    EXPECT_NE(braces.find(R"( praenora.engine [obj={"a":[1,2]}] braces {} {0} %s)"),
              std::string::npos)
        << braces;
    const auto event = capture.json_lines().at(3);
    EXPECT_EQ(event.at("attributes").at("obj"), json::parse(R"({"a":[1,2]})"));
    EXPECT_EQ(event.at("attributes").at("logger"), "praenora.engine");
    EXPECT_EQ(event.at("attributes").size(), 2u);
}

TEST(Logging, LevelFiltering) {
    LogHub hub(quiet(LogFormat::json, LogLevel::warn));
    const LogCapture capture(hub);
    const auto logger = hub.logger("praenora.loader");
    log_event(*logger, LogLevel::debug, "hidden");
    log_event(*logger, LogLevel::info, "hidden too");
    log_event(*logger, LogLevel::warn, "shown");
    log_event(*logger, LogLevel::error, "also shown");
    log_event(*logger, LogLevel::off, "never");
    auto entries = capture.json_lines();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].at("body"), "shown");
    EXPECT_EQ(entries[0].at("severity_text"), "WARN");
    EXPECT_EQ(entries[1].at("body"), "also shown");

    // A step's level filters only its own logger.
    const auto module_logger = hub.logger("module.estimate");
    praenora::detail::set_log_level(*module_logger, LogLevel::trace);
    log_event(*module_logger, LogLevel::debug, "module detail");
    log_event(*logger, LogLevel::debug, "still hidden");
    entries = capture.json_lines();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[2].at("body"), "module detail");
    EXPECT_EQ(entries[2].at("attributes").at("logger"), "module.estimate");

    // Severity numbers follow the OpenTelemetry log data model.
    LogHub all(quiet(LogFormat::json, LogLevel::trace));
    const LogCapture everything(all);
    const auto each = all.logger("praenora.runtime");
    const std::vector<std::pair<LogLevel, std::pair<const char*, int>>> levels = {
        {LogLevel::trace, {"TRACE", 1}},  {LogLevel::debug, {"DEBUG", 5}},
        {LogLevel::info, {"INFO", 9}},    {LogLevel::warn, {"WARN", 13}},
        {LogLevel::error, {"ERROR", 17}}, {LogLevel::critical, {"CRITICAL", 21}},
    };
    for (const auto& level : levels) {
        log_event(*each, level.first, "x");
    }
    entries = everything.json_lines();
    ASSERT_EQ(entries.size(), levels.size());
    for (std::size_t i = 0; i < levels.size(); ++i) {
        EXPECT_EQ(entries[i].at("severity_text"), levels[i].second.first);
        EXPECT_EQ(entries[i].at("severity_number"), levels[i].second.second);
    }

    LogHub off(quiet(LogFormat::text, LogLevel::off));
    const LogCapture nothing(off);
    log_event(*off.logger("praenora.runtime"), LogLevel::critical, "dropped");
    EXPECT_TRUE(nothing.lines().empty());
}

// The PRAENORA_LOG_* levels of the ABI, as modules and providers pass them.
TEST(Logging, AbiLevelsMapToLogLevels) {
    using praenora::detail::log_level_from_abi;
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_TRACE), LogLevel::trace);
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_DEBUG), LogLevel::debug);
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_INFO), LogLevel::info);
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_WARN), LogLevel::warn);
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_ERROR), LogLevel::error);
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_CRITICAL), LogLevel::critical);
    // Below trace counts as trace, above critical as critical.
    EXPECT_EQ(log_level_from_abi(-3), LogLevel::trace);
    EXPECT_EQ(log_level_from_abi(PRAENORA_LOG_CRITICAL + 1), LogLevel::critical);

    LogHub hub(quiet(LogFormat::text, LogLevel::warn));
    const auto logger = hub.logger("provider.test");
    EXPECT_FALSE(praenora::detail::log_enabled(*logger, LogLevel::info));
    EXPECT_TRUE(praenora::detail::log_enabled(*logger, LogLevel::warn));
    EXPECT_TRUE(praenora::detail::log_enabled(*logger, LogLevel::critical));
    EXPECT_FALSE(praenora::detail::log_enabled(*logger, LogLevel::off));
}

TEST(Logging, DoesNotRegisterGlobalLoggers) {
    auto* const default_logger = spdlog::default_logger_raw();
    LogHub hub(quiet(LogFormat::text));
    const auto logger = hub.logger("praenora.engine");
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->name(), "praenora.engine");
    EXPECT_EQ(spdlog::get("praenora.engine"), nullptr);
    EXPECT_EQ(spdlog::default_logger_raw(), default_logger);
    // A second hub is independent: its loggers write to its own sinks only.
    LogHub other(quiet(LogFormat::text));
    const LogCapture first(hub);
    const LogCapture second(other);
    log_event(*other.logger("praenora.engine"), LogLevel::info, "other hub");
    EXPECT_TRUE(first.lines().empty());
    EXPECT_EQ(second.lines().size(), 1u);
}

namespace {

// Keeps what each event hands the sinks: spdlog's payload, before any formatter.
class PayloadSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> payloads;

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        payloads.emplace_back(message.payload.data(), message.payload.size());
    }
    void flush_() override {}
};

} // namespace

// What a sink receives of an event is taken apart again into its attributes, in their order, and
// its message (a module host passes its providers' events on so); a payload that log_event() did
// not make is a message alone.
TEST(Logging, AnEventIsTakenApartAgainFromWhatASinkReceives) {
    LogHub hub(quiet(LogFormat::text, LogLevel::trace));
    const auto sink = std::make_shared<PayloadSink>();
    hub.add_sink(sink);
    const auto logger = hub.logger("provider.p");
    log_event(*logger, LogLevel::info, "loaded and started \xE2\x9C\x93",
              {{"praenora.provider", "p@0.1.0"},
               {"praenora.event", "provider.loaded"},
               {"count", 3},
               {"logger", "dropped"},
               {"nested", {{"b", 1}, {"a", {true, nullptr}}}},
               {"text", "a \"quoted\" word\nand a line"}});
    logger->info("plain");
    ASSERT_EQ(sink->payloads.size(), 2u);
    const praenora::detail::LoggedEvent event =
        praenora::detail::decode_logged_event(sink->payloads[0]);
    EXPECT_EQ(event.message, "loaded and started \xE2\x9C\x93");
    ASSERT_EQ(event.attributes.size(), 5u);
    const std::vector<std::string> keys = {"praenora.provider", "praenora.event", "count", "nested",
                                           "text"};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(event.attributes[i].key, keys[i]);
    }
    EXPECT_EQ(event.attributes[0].value, "p@0.1.0");
    EXPECT_EQ(event.attributes[2].value, 3);
    EXPECT_EQ(event.attributes[3].value, json({{"b", 1}, {"a", {true, nullptr}}}));
    EXPECT_EQ(event.attributes[4].value, "a \"quoted\" word\nand a line");

    const praenora::detail::LoggedEvent plain =
        praenora::detail::decode_logged_event(sink->payloads[1]);
    EXPECT_EQ(plain.message, "plain");
    EXPECT_TRUE(plain.attributes.empty());
    // Logged again elsewhere, it is the same event: the same attributes and message.
    LogHub again(quiet(LogFormat::text, LogLevel::trace));
    const auto copy = std::make_shared<PayloadSink>();
    again.add_sink(copy);
    log_event(*again.logger("provider.p"), LogLevel::info, event.message, event.attributes);
    ASSERT_EQ(copy->payloads.size(), 1u);
    EXPECT_EQ(copy->payloads.front(), sink->payloads.front());
}

TEST(Logging, SinksAddedLaterReachExistingLoggers) {
    LogHub hub(quiet(LogFormat::text));
    const auto logger = hub.logger("praenora.config");
    const LogCapture capture(hub);
    log_event(*logger, LogLevel::info, "after the capture");
    EXPECT_EQ(capture.lines().size(), 1u);
    EXPECT_EQ(hub.options().format, LogFormat::text);
}

TEST(Logging, FileSinkAppendsUtf8Lines) {
    ScratchDir dir;
    const auto file = dir.path() / fs::u8path("logs d\xC3\xA9j\xC3\xA0") / "praenora.log";
    LogOptions options = quiet(LogFormat::json);
    options.file = file.u8string();
    {
        LogHub hub(options);
        log_event(*hub.logger("praenora.runtime"), LogLevel::info, "caf\xC3\xA9",
                  {{"bytes", std::string("bad \xFF")}});
    }
    {
        LogHub hub(options);
        log_event(*hub.logger("praenora.runtime"), LogLevel::warn, "second run");
    }
    const auto lines = read_lines(file);
    ASSERT_EQ(lines.size(), 2u) << "the log file is appended to, not replaced";
    for (const auto& line : lines) {
        EXPECT_EQ(line.find('\r'), std::string::npos);
    }
    const auto first = json::parse(lines[0]);
    EXPECT_EQ(first.at("body"), "caf\xC3\xA9");
    EXPECT_EQ(first.at("attributes").at("bytes"), "bad \xEF\xBF\xBD")
        << "invalid UTF-8 is replaced, not fatal";
    EXPECT_EQ(json::parse(lines[1]).at("body"), "second run");
}

TEST(Logging, UnwritableLogFileIsIoError) {
    ScratchDir dir;
    const auto blocker = dir.path() / "blocker";
    std::ofstream(blocker) << "x";
    LogOptions options = quiet(LogFormat::text);
    options.file = (blocker / "praenora.log").u8string();
    try {
        LogHub hub(options);
        FAIL() << "no error";
    } catch (const praenora::Error& e) {
        EXPECT_EQ(e.code(), praenora::ErrorCode::IO_ERROR);
    }
}

TEST(Logging, ParseLogLevel) {
    using praenora::detail::parse_log_level;
    EXPECT_EQ(parse_log_level("trace"), LogLevel::trace);
    EXPECT_EQ(parse_log_level("debug"), LogLevel::debug);
    EXPECT_EQ(parse_log_level("info"), LogLevel::info);
    EXPECT_EQ(parse_log_level("warn"), LogLevel::warn);
    EXPECT_EQ(parse_log_level("error"), LogLevel::error);
    EXPECT_EQ(parse_log_level("critical"), LogLevel::critical);
    EXPECT_EQ(parse_log_level("off"), LogLevel::off);
    for (const char* bad : {"verbose", "INFO", "warning", "", " info"}) {
        try {
            parse_log_level(bad);
            ADD_FAILURE() << "no error for '" << bad << "'";
        } catch (const praenora::Error& e) {
            EXPECT_EQ(e.code(), praenora::ErrorCode::CONFIG_INVALID) << bad;
        }
    }
    EXPECT_EQ(praenora::detail::parse_log_format("text"), LogFormat::text);
    EXPECT_EQ(praenora::detail::parse_log_format("json"), LogFormat::json);
    try {
        praenora::detail::parse_log_format("xml");
        ADD_FAILURE() << "no error";
    } catch (const praenora::Error& e) {
        EXPECT_EQ(e.code(), praenora::ErrorCode::CONFIG_INVALID);
    }
}

TEST(Logging, DefaultOptionsFollowTheSpecification) {
    const LogOptions options;
    EXPECT_EQ(options.level, LogLevel::info);
    EXPECT_EQ(options.format, LogFormat::text);
    EXPECT_TRUE(options.file.empty());
    EXPECT_TRUE(options.to_stderr);
    const praenora::RuntimeOptions runtime;
    EXPECT_TRUE(runtime.use_environment);
    const praenora::WorkflowOptions workflow;
    EXPECT_FALSE(workflow.overwrite);
    EXPECT_FALSE(workflow.seed.has_value());
    const praenora::GraphRequest graph;
    EXPECT_EQ(graph.format, praenora::GraphFormat::text);
    EXPECT_FALSE(praenora::ValidateOptions{}.load);
}
