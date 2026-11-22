#include "log_capture.hpp"

namespace praenora::test {

namespace {

std::string without_newline(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

} // namespace

LogCapture::LogCapture(praenora::detail::LogHub& hub, std::size_t capacity)
    : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(capacity)) {
    hub.add_sink(sink_);
}

std::vector<std::string> LogCapture::lines() const {
    std::vector<std::string> lines;
    for (auto& line : sink_->last_formatted()) {
        lines.push_back(without_newline(std::move(line)));
    }
    return lines;
}

std::vector<nlohmann::json> LogCapture::json_lines() const {
    const auto formatter = praenora::detail::make_log_formatter(praenora::LogFormat::json);
    std::vector<nlohmann::json> events;
    for (const auto& message : sink_->last_raw()) {
        spdlog::memory_buf_t buffer;
        formatter->format(message, buffer);
        events.push_back(
            nlohmann::json::parse(without_newline(std::string(buffer.data(), buffer.size()))));
    }
    return events;
}

} // namespace praenora::test
