#include "observability/otlp/log_sink.hpp"

#include "observability/logging.hpp"

#include <chrono>
#include <utility>

namespace praenora::detail {

OtlpLogSink::OtlpLogSink(std::shared_ptr<OtlpLogQueue> queue, std::shared_ptr<const OtlpRun> run)
    : queue_(std::move(queue)), run_(std::move(run)) {}

void OtlpLogSink::sink_it_(const spdlog::details::log_msg& msg) {
    QueuedLogRecord record;
    record.time_unix_ns = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(msg.time.time_since_epoch()).count());
    record.level = log_level_from_spdlog(msg.level);
    record.logger.assign(msg.logger_name.data(), msg.logger_name.size());
    record.payload.assign(msg.payload.data(), msg.payload.size());
    record.run = run_;
    queue_->push(std::move(record));
}

} // namespace praenora::detail
