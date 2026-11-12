// Captures what a LogHub logs, for white-box tests: a ring-buffer sink added to the hub.
#ifndef PRAENORA_TESTS_SUPPORT_LOG_CAPTURE_HPP
#define PRAENORA_TESTS_SUPPORT_LOG_CAPTURE_HPP

#include "observability/logging.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace praenora::test {

class LogCapture {
public:
    // Starts capturing everything the hub's loggers log from now on, including loggers made
    // before the capture. Only the newest `capacity` events are kept (the buffer is allocated up
    // front, so keep it small).
    explicit LogCapture(praenora::detail::LogHub& hub, std::size_t capacity = 2048);

    // The captured events in the hub's format, one line each, without the newline.
    std::vector<std::string> lines() const;

    // The captured events in the JSON format, parsed, whatever the hub's format.
    std::vector<nlohmann::json> json_lines() const;

private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_LOG_CAPTURE_HPP
