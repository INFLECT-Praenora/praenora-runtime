// The spdlog sink of the OTLP export: added to a log hub (the Runtime's, and each workflow's), it
// receives whatever the hub's level lets through and only appends each record, as it came, to the
// exporter's queue under a short lock. The exporter takes the records apart on its own thread.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_OTLP_LOG_SINK_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_OTLP_LOG_SINK_HPP

#include <praenora/runtime/options.hpp>

#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace praenora::detail {

// A run as its telemetry names it: its fingerprint, and its run number (Workflow::run_number()),
// which Runtime::open() sets once the workflow exists; 0 until then.
class OtlpRun {
public:
    explicit OtlpRun(std::string fingerprint) : fingerprint_(std::move(fingerprint)) {}

    const std::string& fingerprint() const noexcept { return fingerprint_; }
    std::uint64_t number() const noexcept { return number_.load(std::memory_order_acquire); }
    void set_number(std::uint64_t number) noexcept {
        number_.store(number, std::memory_order_release);
    }

private:
    std::string fingerprint_;
    std::atomic<std::uint64_t> number_{0};
};

// A log record as the sink queues it.
struct QueuedLogRecord {
    std::int64_t time_unix_ns = 0;
    LogLevel level = LogLevel::info;
    std::string logger;
    std::string payload;                // spdlog's payload, as log_event() made it
    std::shared_ptr<const OtlpRun> run; // none for the Runtime's own records
};

// Where a sink puts its records: the exporter's queue.
class OtlpLogQueue {
public:
    virtual ~OtlpLogQueue() = default;
    // Takes `record`, or, when the queue is full, drops and counts it; once the exporter has
    // stopped, drops it without counting (nobody reads the count then). Short, thread-safe, never
    // throws.
    virtual void push(QueuedLogRecord record) noexcept = 0;
};

class OtlpLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    // The records of `run` (none for the Runtime's own hub) go to `queue`.
    OtlpLogSink(std::shared_ptr<OtlpLogQueue> queue, std::shared_ptr<const OtlpRun> run);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    std::shared_ptr<OtlpLogQueue> queue_;
    std::shared_ptr<const OtlpRun> run_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_OTLP_LOG_SINK_HPP
