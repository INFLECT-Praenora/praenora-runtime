#include "observability/metrics.hpp"

#include "core/time.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace praenora::detail {

using nlohmann::json;

namespace {

bool is_terminal(WorkflowState state) {
    return state == WorkflowState::completed || state == WorkflowState::stopped ||
           state == WorkflowState::failed;
}

} // namespace

void MetricsRegistry::set_run_info(std::string workflow_name, std::string workflow_version,
                                   std::string fingerprint) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.workflow_name = std::move(workflow_name);
    state_.workflow_version = std::move(workflow_version);
    state_.fingerprint = std::move(fingerprint);
}

void MetricsRegistry::register_step(const std::string& step, const std::string& module_name,
                                    const std::string& module_version) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = steps_.find(step);
    if (it != steps_.end()) {
        state_.steps[it->second].module_name = module_name;
        state_.steps[it->second].module_version = module_version;
        return;
    }
    StepStatus status;
    status.step = step;
    status.module_name = module_name;
    status.module_version = module_version;
    steps_.emplace(step, state_.steps.size());
    state_.steps.push_back(std::move(status));
}

StepStatus& MetricsRegistry::step_status(const std::string& step) {
    const auto it = steps_.find(step);
    if (it == steps_.end()) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "metrics: step '" + step + "' is not registered");
    }
    return state_.steps[it->second];
}

void MetricsRegistry::set_step_state(const std::string& step, StepState state) {
    const std::lock_guard<std::mutex> lock(mutex_);
    step_status(step).state = state;
}

void MetricsRegistry::record_invocation(const std::string& step, CallOutcome outcome,
                                        std::chrono::nanoseconds duration, std::int64_t at_unix_ns,
                                        std::optional<ErrorRecord> error) {
    const std::int64_t ns = std::max<std::int64_t>(duration.count(), 0);
    const std::lock_guard<std::mutex> lock(mutex_);
    StepStatus& status = step_status(step);
    switch (outcome) {
    case CallOutcome::succeeded:
        ++status.succeeded;
        status.last_success_at_unix_ns = at_unix_ns;
        break;
    case CallOutcome::failed:
    case CallOutcome::refused: // not called: its library has an abandoned call
        ++status.failed;
        break;
    case CallOutcome::timed_out:
    case CallOutcome::unresponsive:
        ++status.timed_out;
        break;
    case CallOutcome::cancelled:
        ++status.cancelled;
        break;
    }
    status.min_duration_ns = status.invocations == 0 ? ns : std::min(status.min_duration_ns, ns);
    status.max_duration_ns = status.invocations == 0 ? ns : std::max(status.max_duration_ns, ns);
    ++status.invocations;
    status.last_duration_ns = ns;
    status.total_duration_ns += ns;
    status.mean_duration_ns =
        static_cast<double>(status.total_duration_ns) / static_cast<double>(status.invocations);
    status.last_invocation_at_unix_ns = at_unix_ns;
    if (error) {
        status.last_error = std::move(*error);
    }
}

void MetricsRegistry::set_step_error(const std::string& step, ErrorRecord error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    step_status(step).last_error = std::move(error);
}

void MetricsRegistry::set_workflow_state(WorkflowState state) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.state = state;
    if (is_terminal(state) && started_ && !ended_) {
        ended_ = std::chrono::steady_clock::now();
    }
}

void MetricsRegistry::mark_started(std::int64_t unix_ns) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.started_at_unix_ns = unix_ns;
    started_ = std::chrono::steady_clock::now();
    ended_.reset();
}

void MetricsRegistry::record_tick(std::uint64_t tick, double logical_time,
                                  std::chrono::nanoseconds duration) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.ticks_completed = tick + 1;
    state_.current_tick = tick;
    state_.logical_time = logical_time;
    state_.last_tick_duration_ns = std::max<std::int64_t>(duration.count(), 0);
}

void MetricsRegistry::set_last_error(ErrorRecord error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error = std::move(error);
}

StatusSnapshot MetricsRegistry::snapshot() const {
    StatusSnapshot copy;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        copy = state_;
        if (started_) {
            const auto end = ended_ ? *ended_ : std::chrono::steady_clock::now();
            copy.uptime_ns = std::max<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - *started_).count(), 0);
        }
    }
    copy.generated_at_unix_ns = now_unix_ns();
    copy.runtime_version = version_info().runtime;
    copy.pid = current_process_id();
    copy.health = compute_health(copy);
    return copy;
}

Health compute_health(const StatusSnapshot& snapshot) {
    const bool broken_instance =
        std::any_of(snapshot.steps.begin(), snapshot.steps.end(), [](const StepStatus& step) {
            return step.state == StepState::failed || step.state == StepState::unresponsive;
        });
    if (snapshot.state == WorkflowState::failed || broken_instance) {
        return Health::unhealthy;
    }
    if (snapshot.state == WorkflowState::created || snapshot.state == WorkflowState::starting) {
        return Health::unknown;
    }
    const bool trouble_counted =
        std::any_of(snapshot.steps.begin(), snapshot.steps.end(), [](const StepStatus& step) {
            return step.timed_out > 0 || step.cancelled > 0 || step.failed > 0;
        });
    if (trouble_counted || snapshot.state == WorkflowState::stopping ||
        snapshot.state == WorkflowState::stopped || snapshot.last_error.has_value()) {
        return Health::degraded;
    }
    return Health::healthy;
}

std::string metrics_json(const StatusSnapshot& snapshot) {
    json points = json::array();
    const auto point = [&](const char* name, const char* unit, const char* kind, json attributes) {
        return json{
            {"name", name}, {"unit", unit}, {"kind", kind}, {"attributes", std::move(attributes)}};
    };
    for (const StepStatus& step : snapshot.steps) {
        const std::string module = step.module_name + "@" + step.module_version;
        const std::pair<const char*, std::uint64_t> outcomes[] = {{"succeeded", step.succeeded},
                                                                  {"failed", step.failed},
                                                                  {"timed_out", step.timed_out},
                                                                  {"cancelled", step.cancelled}};
        for (const auto& [outcome, count] : outcomes) {
            json counter = point("praenora.step.invocations", "{invocation}", "counter",
                                 {{"step", step.step}, {"module", module}, {"outcome", outcome}});
            counter["value"] = count;
            points.push_back(std::move(counter));
        }
        json duration = point("praenora.step.invocation.duration", "ns", "summary",
                              {{"step", step.step}, {"module", module}});
        duration["summary"] = {{"count", step.invocations},
                               {"sum", step.total_duration_ns},
                               {"min", step.min_duration_ns},
                               {"max", step.max_duration_ns},
                               {"last", step.last_duration_ns}};
        points.push_back(std::move(duration));
    }
    const json workflow = {{"workflow", snapshot.workflow_name + "@" + snapshot.workflow_version}};
    json ticks = point("praenora.workflow.ticks", "{tick}", "counter", workflow);
    ticks["value"] = snapshot.ticks_completed;
    points.push_back(std::move(ticks));
    json tick_duration = point("praenora.workflow.tick.duration", "ns", "gauge", workflow);
    tick_duration["value"] = snapshot.last_tick_duration_ns;
    points.push_back(std::move(tick_duration));
    json uptime = point("praenora.workflow.uptime", "ns", "gauge", workflow);
    uptime["value"] = snapshot.uptime_ns;
    points.push_back(std::move(uptime));
    return json{{"metrics", std::move(points)}}.dump(-1, ' ', false,
                                                     json::error_handler_t::replace);
}

} // namespace praenora::detail
