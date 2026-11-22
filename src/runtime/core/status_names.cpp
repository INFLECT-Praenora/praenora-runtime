// Names of the states in praenora/runtime/status.hpp.
#include <praenora/runtime/status.hpp>

namespace praenora {

std::string to_string(WorkflowState state) {
    switch (state) {
    case WorkflowState::created:
        return "created";
    case WorkflowState::starting:
        return "starting";
    case WorkflowState::ready:
        return "ready";
    case WorkflowState::running:
        return "running";
    case WorkflowState::stopping:
        return "stopping";
    case WorkflowState::completed:
        return "completed";
    case WorkflowState::stopped:
        return "stopped";
    case WorkflowState::failed:
        return "failed";
    }
    return "failed";
}

std::string to_string(StepState state) {
    switch (state) {
    case StepState::loaded:
        return "loaded";
    case StepState::created:
        return "created";
    case StepState::ready:
        return "ready";
    case StepState::busy:
        return "busy";
    case StepState::finalized:
        return "finalized";
    case StepState::destroyed:
        return "destroyed";
    case StepState::failed:
        return "failed";
    case StepState::unresponsive:
        return "unresponsive";
    case StepState::crashed:
        return "crashed";
    }
    return "failed";
}

std::string to_string(Health health) {
    switch (health) {
    case Health::unknown:
        return "unknown";
    case Health::healthy:
        return "healthy";
    case Health::degraded:
        return "degraded";
    case Health::unhealthy:
        return "unhealthy";
    }
    return "unknown";
}

} // namespace praenora
