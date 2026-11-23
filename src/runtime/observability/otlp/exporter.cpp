#include "observability/otlp/exporter.hpp"

#include "core/time.hpp"
#include "observability/metrics.hpp"
#include "observability/otlp/otlp_json.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace praenora::detail {

namespace {

using Clock = std::chrono::steady_clock;

std::size_t index_of(OtlpSignal signal) { return signal == OtlpSignal::metrics ? 0 : 1; }

} // namespace

// What the exporter shares with its sinks and the runs added to it, so that either may outlive
// the other: the queue of log records (mutex), and the runs (runs_mutex). Nothing logs while it
// holds either mutex.
struct OtlpExporter::State final : OtlpLogQueue {
    State(std::size_t queue_limit, bool metrics)
        : max_queue(queue_limit), exports_metrics(metrics) {}

    void push(QueuedLogRecord record) noexcept override {
        std::unique_lock<std::mutex> lock(mutex);
        if (closed) {
            return;
        }
        if (logs.size() >= max_queue) {
            ++dropped;
            return;
        }
        try {
            logs.emplace_back(Clock::now(), std::move(record));
        } catch (...) {
            ++dropped;
            return;
        }
        // The exporter's thread computes its next wake-up from the oldest record, and wakes for a
        // full batch.
        const std::size_t size = logs.size();
        lock.unlock();
        if (size == 1 || size == batch) {
            wake.notify_all();
        }
    }

    const std::size_t max_queue;
    std::size_t batch = 0; // set once, before the thread starts
    const bool exports_metrics;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<Clock::time_point, QueuedLogRecord>> logs; // with the time queued
    std::uint64_t dropped = 0;
    bool flush_logs = false;            // send what is queued now (a run finished)
    std::vector<OtlpRunMetrics> finals; // the last metrics of finished runs, not sent yet
    bool stopping = false;              // the thread ends
    bool closed = false;                // the exporter is gone: records are dropped

    struct Entry {
        std::shared_ptr<const OtlpRun> run;
        std::function<StatusSnapshot()> snapshot;
        std::int64_t opened_at_unix_ns = 0;
    };
    std::mutex runs_mutex;
    std::map<std::uint64_t, Entry> runs; // in the order they were added
    std::uint64_t next_id = 1;
};

namespace {

// The metrics of `entry` now, or nothing when its snapshot cannot be taken.
std::optional<OtlpRunMetrics> metrics_of(const std::shared_ptr<const OtlpRun>& run,
                                         const std::function<StatusSnapshot()>& snapshot,
                                         std::int64_t opened_at_unix_ns) {
    try {
        const StatusSnapshot taken = snapshot();
        OtlpRunMetrics metrics;
        metrics.run_number = run->number();
        metrics.fingerprint = run->fingerprint();
        metrics.start_unix_ns = taken.started_at_unix_ns.value_or(opened_at_unix_ns);
        metrics.time_unix_ns = taken.generated_at_unix_ns;
        metrics.metrics_json = metrics_json(taken);
        return metrics;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

const char* signal_name(OtlpSignal signal) noexcept {
    return signal == OtlpSignal::metrics ? "metrics" : "logs";
}

OtlpExporter::OtlpExporter(OtlpSettings settings, std::vector<LogAttribute> resource,
                           std::string scope_version, std::unique_ptr<OtlpTransport> transport,
                           std::shared_ptr<spdlog::logger> logger, OtlpTiming timing)
    : settings_(std::move(settings)), resource_(std::move(resource)),
      scope_version_(std::move(scope_version)), transport_(std::move(transport)),
      logger_(std::move(logger)), timing_(std::move(timing)), created_at_unix_ns_(now_unix_ns()),
      state_(std::make_shared<State>(std::max<std::size_t>(timing_.max_queue, 1),
                                     settings_.metrics.endpoint.has_value())) {
    timing_.max_batch = std::max<std::size_t>(timing_.max_batch, 1);
    state_->batch = timing_.max_batch;
    thread_ = std::thread([this] { run_thread(); });
}

OtlpExporter::~OtlpExporter() {
    // The end takes one export timeout at most, the largest of the signals', from here: a request
    // in flight ends within it, and every attempt of the flush gets what is left of it.
    std::chrono::milliseconds budget(0);
    if (exports_metrics()) {
        budget = std::max(budget, settings_.metrics.timeout);
    }
    if (exports_logs()) {
        budget = std::max(budget, settings_.logs.timeout);
    }
    const auto deadline = Clock::now() + budget;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_all();
    if (timing_.on_stop) {
        try {
            timing_.on_stop();
        } catch (...) {
            // A test hook; nothing to do.
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // What is left, one attempt per signal: the metrics first, so that a warning about them still
    // reaches the logs. A signal whose request failed while the exporter was stopping has had its
    // attempt, and one for which no time is left gets none: their requests are dropped, and each
    // counts as a failed export.
    const auto flush = [&](OtlpSignal signal, const auto& send) {
        const std::size_t i = index_of(signal);
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (ended_[i]) {
            count_failure(signal, "not sent: the request in flight at the end failed");
        } else if (left <= std::chrono::milliseconds(0)) {
            count_failure(signal, "not sent: the end's export timeout is spent");
        } else {
            const OtlpSignalSettings& to =
                signal == OtlpSignal::metrics ? settings_.metrics : settings_.logs;
            static_cast<void>(send(std::min(to.timeout, left)));
        }
    };
    try {
        if (exports_metrics()) {
            std::vector<OtlpRunMetrics> runs;
            {
                const std::lock_guard<std::mutex> lock(state_->mutex);
                runs.swap(state_->finals);
            }
            std::vector<OtlpRunMetrics> open = open_runs();
            runs.insert(runs.end(), std::make_move_iterator(open.begin()),
                        std::make_move_iterator(open.end()));
            if (!runs.empty()) {
                flush(OtlpSignal::metrics, [&](std::chrono::milliseconds timeout) {
                    return export_metrics(std::move(runs), true, timeout);
                });
            }
        }
        std::vector<QueuedLogRecord> records;
        {
            const std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closed = true;
            for (auto& queued : state_->logs) {
                records.push_back(std::move(queued.second));
            }
            state_->logs.clear();
        }
        if (exports_logs() && !records.empty()) {
            flush(OtlpSignal::logs, [&](std::chrono::milliseconds timeout) {
                return export_logs(records, true, timeout);
            });
        }
    } catch (...) {
        // Destructors never throw; what was not sent is lost.
    }
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
}

bool OtlpExporter::exports_metrics() const noexcept {
    return settings_.metrics.endpoint.has_value();
}

bool OtlpExporter::exports_logs() const noexcept { return settings_.logs.endpoint.has_value(); }

std::shared_ptr<spdlog::sinks::sink> OtlpExporter::log_sink(std::shared_ptr<const OtlpRun> run) {
    if (!exports_logs()) {
        return nullptr;
    }
    return std::make_shared<OtlpLogSink>(state_, std::move(run));
}

OtlpExporter::Run::Run(Run&& other) noexcept
    : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}

OtlpExporter::Run& OtlpExporter::Run::operator=(Run&& other) noexcept {
    if (this != &other) {
        remove(false);
        state_ = std::move(other.state_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

OtlpExporter::Run::~Run() { remove(false); }

void OtlpExporter::Run::finish() noexcept { remove(true); }

void OtlpExporter::Run::remove(bool final_export) noexcept {
    if (!state_) {
        return;
    }
    std::optional<OtlpRunMetrics> last;
    {
        const std::lock_guard<std::mutex> lock(state_->runs_mutex);
        const auto found = state_->runs.find(id_);
        if (found != state_->runs.end()) {
            const State::Entry& entry = found->second;
            if (final_export && state_->exports_metrics && entry.run->number() != 0) {
                last = metrics_of(entry.run, entry.snapshot, entry.opened_at_unix_ns);
            }
            state_->runs.erase(found);
        }
    }
    if (final_export) {
        {
            const std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->closed) {
                try {
                    if (last) {
                        state_->finals.push_back(std::move(*last));
                    }
                } catch (...) {
                    // The last snapshot is lost; the logs still go.
                }
                state_->flush_logs = true;
            }
        }
        state_->wake.notify_all();
    }
    state_.reset();
    id_ = 0;
}

OtlpExporter::Run OtlpExporter::add_run(std::shared_ptr<const OtlpRun> run,
                                        std::function<StatusSnapshot()> snapshot,
                                        std::int64_t opened_at_unix_ns) {
    const std::lock_guard<std::mutex> lock(state_->runs_mutex);
    const std::uint64_t id = state_->next_id++;
    state_->runs.emplace(id, State::Entry{std::move(run), std::move(snapshot), opened_at_unix_ns});
    return Run(state_, id);
}

std::uint64_t OtlpExporter::failures(OtlpSignal signal) const noexcept {
    return failures_[index_of(signal)].load();
}

std::uint64_t OtlpExporter::dropped_log_records() const noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->dropped;
}

void OtlpExporter::run_thread() {
    set_current_thread_name("praenora:otlp");
    auto next_metrics = Clock::now() + settings_.metric_export_interval;
    std::unique_lock<std::mutex> lock(state_->mutex);
    while (!state_->stopping) {
        const auto now = Clock::now();
        const bool logs_waiting = exports_logs() && !state_->logs.empty();
        const auto logs_due_at = logs_waiting ? state_->logs.front().first + timing_.batch_delay
                                              : Clock::time_point::max();
        const bool logs_due = logs_waiting && (state_->logs.size() >= timing_.max_batch ||
                                               state_->flush_logs || now >= logs_due_at);
        const bool periodic = exports_metrics() && now >= next_metrics;
        const bool metrics_due = periodic || !state_->finals.empty();
        if (!logs_due && !metrics_due) {
            if (!logs_waiting) {
                state_->flush_logs = false;
            }
            const auto wake_at =
                std::min(logs_due_at, exports_metrics() ? next_metrics : Clock::time_point::max());
            if (wake_at == Clock::time_point::max()) {
                state_->wake.wait(lock);
            } else {
                state_->wake.wait_until(lock, wake_at);
            }
            continue;
        }
        std::vector<QueuedLogRecord> batch;
        if (logs_due) {
            const std::size_t count = std::min(state_->logs.size(), timing_.max_batch);
            try {
                batch.reserve(count);
            } catch (...) {
                state_->dropped += count; // no room for the batch: its records are dropped
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (batch.capacity() >= count) {
                    batch.push_back(std::move(state_->logs.front().second));
                }
                state_->logs.pop_front();
            }
            if (state_->logs.empty()) {
                state_->flush_logs = false;
            }
        }
        std::vector<OtlpRunMetrics> finals;
        finals.swap(state_->finals);
        if (periodic) {
            next_metrics = now + settings_.metric_export_interval;
        }
        lock.unlock();

        // A stop ends a wait between attempts at once: what waited goes back for the flush, which
        // the destructor makes, and nothing more is tried here. A request that fails while the
        // exporter stops has been its signal's attempt at the end (deliver()). Nothing that goes
        // wrong here may end the thread, let alone the process: the export is dropped and counted.
        bool stopped = false;
        if (metrics_due) {
            try {
                std::vector<OtlpRunMetrics> runs = finals;
                if (periodic) {
                    std::vector<OtlpRunMetrics> open = open_runs();
                    runs.insert(runs.end(), std::make_move_iterator(open.begin()),
                                std::make_move_iterator(open.end()));
                }
                const Delivery delivery =
                    export_metrics(std::move(runs), false, settings_.metrics.timeout);
                if (delivery == Delivery::interrupted) {
                    stopped = true;
                    const std::lock_guard<std::mutex> relock(state_->mutex);
                    state_->finals.insert(state_->finals.begin(),
                                          std::make_move_iterator(finals.begin()),
                                          std::make_move_iterator(finals.end()));
                } else {
                    stopped = ended_[index_of(OtlpSignal::metrics)];
                }
            } catch (...) {
                count_failure(OtlpSignal::metrics, "the request could not be made");
            }
        }
        if (!batch.empty()) {
            try {
                if (!stopped) {
                    stopped = stop_requested();
                }
                if (stopped ||
                    export_logs(batch, false, settings_.logs.timeout) == Delivery::interrupted) {
                    requeue_logs(batch);
                }
            } catch (...) {
                count_failure(OtlpSignal::logs, "the request could not be made");
            }
        }
        lock.lock();
    }
}

bool OtlpExporter::stop_requested() {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stopping;
}

void OtlpExporter::requeue_logs(std::vector<QueuedLogRecord>& batch) {
    // Back at the front, the oldest first, as far as the queue's limit allows; the newest of the
    // batch that do not fit are dropped and counted.
    const std::lock_guard<std::mutex> lock(state_->mutex);
    const std::size_t room =
        state_->logs.size() < state_->max_queue ? state_->max_queue - state_->logs.size() : 0;
    const std::size_t kept = std::min(room, batch.size());
    const auto now = Clock::now();
    for (std::size_t i = kept; i-- > 0;) {
        state_->logs.emplace_front(now, std::move(batch[i]));
    }
    state_->dropped += batch.size() - kept;
}

std::vector<OtlpRunMetrics> OtlpExporter::open_runs() {
    std::vector<OtlpRunMetrics> runs;
    const std::lock_guard<std::mutex> lock(state_->runs_mutex);
    for (const auto& [id, entry] : state_->runs) {
        static_cast<void>(id);
        if (entry.run->number() == 0) {
            continue; // not yet returned by Runtime::open()
        }
        if (std::optional<OtlpRunMetrics> metrics =
                metrics_of(entry.run, entry.snapshot, entry.opened_at_unix_ns)) {
            runs.push_back(std::move(*metrics));
        }
    }
    return runs;
}

OtlpExporter::Delivery OtlpExporter::export_metrics(std::vector<OtlpRunMetrics> runs,
                                                    bool single_attempt,
                                                    std::chrono::milliseconds timeout) {
    if (runs.empty()) {
        return Delivery::sent;
    }
    OtlpExporterCounters counters;
    counters.start_unix_ns = created_at_unix_ns_;
    counters.time_unix_ns = now_unix_ns();
    if (exports_metrics()) {
        counters.metrics_failures = failures(OtlpSignal::metrics);
    }
    if (exports_logs()) {
        counters.logs_failures = failures(OtlpSignal::logs);
    }
    counters.dropped_log_records = dropped_log_records();
    return deliver(OtlpSignal::metrics,
                   encode_otlp_metrics(resource_, scope_version_, runs, counters), single_attempt,
                   timeout);
}

OtlpExporter::Delivery OtlpExporter::export_logs(const std::vector<QueuedLogRecord>& records,
                                                 bool single_attempt,
                                                 std::chrono::milliseconds timeout) {
    std::vector<OtlpLogRecord> decoded;
    decoded.reserve(records.size());
    for (const QueuedLogRecord& queued : records) {
        LoggedEvent event = decode_logged_event(queued.payload);
        OtlpLogRecord record;
        record.time_unix_ns = queued.time_unix_ns;
        record.level = queued.level;
        record.logger = queued.logger;
        record.message = std::move(event.message);
        record.attributes = std::move(event.attributes);
        if (queued.run) {
            record.run_number = queued.run->number();
            record.run_fingerprint = queued.run->fingerprint();
        }
        decoded.push_back(std::move(record));
    }
    return deliver(OtlpSignal::logs, encode_otlp_logs(resource_, scope_version_, decoded),
                   single_attempt, timeout);
}

OtlpExporter::Delivery OtlpExporter::deliver(OtlpSignal signal, const std::string& body,
                                             bool single_attempt,
                                             std::chrono::milliseconds timeout) {
    const OtlpSignalSettings& to =
        signal == OtlpSignal::metrics ? settings_.metrics : settings_.logs;
    if (!to.endpoint) {
        return Delivery::sent;
    }
    const std::size_t attempts = single_attempt ? 1 : timing_.retry_delays.size() + 1;
    for (std::size_t attempt = 0;; ++attempt) {
        HttpResponse response;
        try {
            response = transport_->post(*to.endpoint, to.headers, body, timeout);
        } catch (const std::exception& e) {
            response = HttpResponse{0, e.what(), std::nullopt};
        } catch (...) {
            response = HttpResponse{0, "the transport failed", std::nullopt};
        }
        if (response.status == 200) {
            return Delivery::sent;
        }
        const int status = response.status;
        const bool retryable =
            status == 0 || status == 429 || status == 502 || status == 503 || status == 504;
        const std::string reason =
            status == 0 ? "no answer: " + response.error : "HTTP status " + std::to_string(status);
        // A request of the thread that failed while the exporter stops was the signal's attempt
        // at the end: it is not tried again, by the thread or by the flush.
        const bool ending = !single_attempt && stop_requested();
        if (ending) {
            ended_[index_of(signal)] = true;
        }
        if (ending || !retryable || attempt + 1 >= attempts) {
            count_failure(signal, reason);
            return Delivery::failed;
        }
        std::chrono::milliseconds wait = timing_.retry_delays[attempt];
        if (response.retry_after_seconds) {
            const std::int64_t seconds =
                std::clamp<std::int64_t>(*response.retry_after_seconds, 0, timing_.max_retry_after);
            wait = timing_.retry_after_unit * seconds;
        }
        if (timing_.on_retry_wait) {
            timing_.on_retry_wait(wait);
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->wake.wait_for(lock, wait, [this] { return state_->stopping; })) {
            return Delivery::interrupted;
        }
    }
}

void OtlpExporter::count_failure(OtlpSignal signal, const std::string& reason) noexcept {
    const std::size_t i = index_of(signal);
    failures_[i].fetch_add(1);
    ++failures_since_warning_[i];
    const auto now = Clock::now();
    if (warned_[i] && now - last_warning_[i] < timing_.warning_interval) {
        return;
    }
    const std::uint64_t since_warning = failures_since_warning_[i];
    failures_since_warning_[i] = 0;
    last_warning_[i] = now;
    warned_[i] = true;
    if (!logger_) {
        return;
    }
    try {
        log_event(*logger_, LogLevel::warn,
                  std::string("an OTLP export of ") + signal_name(signal) + " failed (" + reason +
                      "); the run goes on, and its health is not affected",
                  {{"praenora.event", "telemetry.export_failed"},
                   {"praenora.signal", signal_name(signal)},
                   {"praenora.failures", since_warning},
                   {"praenora.reason", reason}});
    } catch (...) {
        // The warning is lost; the failure is counted.
    }
}

} // namespace praenora::detail
