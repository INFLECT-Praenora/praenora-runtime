// The OTLP exporter of a Runtime (docs/specs/observability.md#otlp-export): one thread,
// praenora:otlp, that sends the log records its sinks queue, in batches, and the metrics of the
// runs added to it, every export interval and once more when a run finishes, over an
// OtlpTransport. A failed export is counted and logged as event telemetry.export_failed (warn), the
// first one and then at most once per warning interval and signal; it never changes a run, its
// health or any snapshot. Its destructor flushes: one attempt per signal, without waiting, all
// within one export timeout.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_OTLP_EXPORTER_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_OTLP_EXPORTER_HPP

#include "observability/logging.hpp"
#include "observability/otlp/http_client.hpp"
#include "observability/otlp/log_sink.hpp"
#include "observability/otlp/otlp_json.hpp"
#include "observability/otlp/otlp_settings.hpp"

#include <praenora/runtime/status.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace praenora::detail {

enum class OtlpSignal { metrics, logs };

// "metrics" or "logs".
const char* signal_name(OtlpSignal signal) noexcept;

// The exporter's limits and waits. The defaults are the specified ones; tests make them shorter.
struct OtlpTiming {
    std::size_t max_batch = 512;  // log records in one request
    std::size_t max_queue = 2048; // log records waiting; a record beyond them is dropped
    // A batch goes at the latest this long after the oldest record waiting was queued.
    std::chrono::milliseconds batch_delay{1000};
    // The waits between the attempts of a retryable failure: as many attempts as waits plus one.
    std::vector<std::chrono::milliseconds> retry_delays{std::chrono::milliseconds(1000),
                                                        std::chrono::milliseconds(2000)};
    // A Retry-After of N (delay-seconds, at most max_retry_after) waits N of these instead.
    std::chrono::milliseconds retry_after_unit{1000};
    std::int64_t max_retry_after = 30;
    // At most one telemetry.export_failed per signal within this interval, after the first.
    std::chrono::milliseconds warning_interval{60000};
    // For tests: called with each wait before an attempt is retried.
    std::function<void(std::chrono::milliseconds)> on_retry_wait;
    // For tests: called by the destructor once it has asked the thread to stop, before it waits
    // for it.
    std::function<void()> on_stop;
};

class OtlpExporter {
    struct State;

public:
    // Exports with `settings` (at least one signal with an endpoint), the resource `resource` and
    // scopes of version `scope_version`, over `transport`, logging its warnings on `logger`
    // (praenora.runtime; may be null). Starts its thread.
    OtlpExporter(OtlpSettings settings, std::vector<LogAttribute> resource,
                 std::string scope_version, std::unique_ptr<OtlpTransport> transport,
                 std::shared_ptr<spdlog::logger> logger, OtlpTiming timing = {});
    // Stops the thread, ending a wait between attempts at once, then sends what is left: the
    // metrics of the finished runs and of those still added, and every queued log record, each
    // signal in one request, one attempt each. All of it takes one export timeout at most (the
    // largest of the signals'), from the destructor's start: a request of the thread that fails
    // while it stops is its signal's attempt, and an attempt gets what is left of that time, or is
    // not made once none is left; what is not sent is counted as a failed export. Records that
    // sinks queue afterwards are dropped.
    ~OtlpExporter();
    OtlpExporter(const OtlpExporter&) = delete;
    OtlpExporter& operator=(const OtlpExporter&) = delete;

    bool exports_metrics() const noexcept;
    bool exports_logs() const noexcept;

    // A sink for a log hub whose records go to the export, those of `run` with its attributes
    // (none for the Runtime's hub); nullptr when logs are not exported. It keeps the exporter's
    // queue, not the exporter: after the exporter is gone, it drops what it receives.
    std::shared_ptr<spdlog::sinks::sink> log_sink(std::shared_ptr<const OtlpRun> run);

    // A run added to the export (add_run()). Its destructor removes the run without a last
    // export.
    class Run {
    public:
        Run() = default;
        Run(Run&& other) noexcept;
        Run& operator=(Run&& other) noexcept;
        ~Run();
        Run(const Run&) = delete;
        Run& operator=(const Run&) = delete;

        // Takes the run's last snapshot, which the exporter sends soon, with the log records
        // queued so far, and removes the run. Idempotent; never throws.
        void finish() noexcept;

    private:
        friend class OtlpExporter;
        Run(std::shared_ptr<State> state, std::uint64_t id) : state_(std::move(state)), id_(id) {}
        void remove(bool final_export) noexcept;

        std::shared_ptr<State> state_;
        std::uint64_t id_ = 0;
    };

    // Adds a run: while its number is not 0, every export interval its metrics are those of
    // `snapshot()` (called on the exporter's thread, under a lock that Run's destructor and
    // finish() take, so never after they return), cumulative from its started_at, or from
    // `opened_at_unix_ns` before it starts.
    Run add_run(std::shared_ptr<const OtlpRun> run, std::function<StatusSnapshot()> snapshot,
                std::int64_t opened_at_unix_ns);

    // The failed exports of `signal` so far, and the log records dropped. Thread-safe.
    std::uint64_t failures(OtlpSignal signal) const noexcept;
    std::uint64_t dropped_log_records() const noexcept;

private:
    enum class Delivery { sent, failed, interrupted };

    void run_thread();
    bool stop_requested();
    void requeue_logs(std::vector<QueuedLogRecord>& batch);
    std::vector<OtlpRunMetrics> open_runs();
    Delivery export_metrics(std::vector<OtlpRunMetrics> runs, bool single_attempt,
                            std::chrono::milliseconds timeout);
    Delivery export_logs(const std::vector<QueuedLogRecord>& records, bool single_attempt,
                         std::chrono::milliseconds timeout);
    Delivery deliver(OtlpSignal signal, const std::string& body, bool single_attempt,
                     std::chrono::milliseconds timeout);
    void count_failure(OtlpSignal signal, const std::string& reason) noexcept;

    OtlpSettings settings_;
    std::vector<LogAttribute> resource_;
    std::string scope_version_;
    std::unique_ptr<OtlpTransport> transport_;
    std::shared_ptr<spdlog::logger> logger_;
    OtlpTiming timing_;
    std::int64_t created_at_unix_ns_ = 0;
    std::shared_ptr<State> state_;
    std::array<std::atomic<std::uint64_t>, 2> failures_{};
    // Used by the exporter's thread only, and by the destructor after it has ended.
    std::array<std::uint64_t, 2> failures_since_warning_{};
    std::array<std::chrono::steady_clock::time_point, 2> last_warning_{};
    std::array<bool, 2> warned_{};
    // Whether the signal's request failed while the exporter was stopping: its attempt at the end.
    std::array<bool, 2> ended_{};
    std::thread thread_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_OTLP_EXPORTER_HPP
