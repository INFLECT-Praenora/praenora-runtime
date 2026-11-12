// praenora status: renders the status file that a run keeps (status-snapshot document).
#include "commands/commands.hpp"

#include "output.hpp"

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

constexpr std::string_view kUsage =
    "usage: praenora status <status.json>\n"
    "\n"
    "Prints the status file that a run keeps (its status_file): the workflow's state, health and\n"
    "ticks, and each step's state and invocation counts.\n";

// A value of the snapshot as text: strings as they are, null as "-", anything else as JSON.
std::string shown(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        return "-";
    }
    return found->is_string() ? found->get<std::string>() : dump(*found);
}

// An error record of the snapshot: "<NAME>: <message> (tick <n>)".
std::string error_text(const json& error) {
    std::string text = shown(error, "name") + ": " + shown(error, "message");
    if (error.contains("tick") && !error.at("tick").is_null()) {
        text += " (tick " + dump(error.at("tick")) + ")";
    }
    return text;
}

std::string status_text(const json& snapshot) {
    const json workflow = snapshot.value("workflow", json::object());
    std::vector<std::vector<std::string>> rows = {
        {"workflow", shown(workflow, "name") + "@" + shown(workflow, "version")},
        {"state", shown(workflow, "state") + " (health " + shown(workflow, "health") + ")"},
        {"ticks completed", shown(workflow, "ticks_completed")},
        {"fingerprint", shown(workflow, "fingerprint")},
        {"started at", shown(workflow, "started_at")},
        {"generated at", shown(snapshot, "generated_at")},
        {"process", shown(snapshot, "pid")}};
    if (workflow.contains("last_error") && workflow.at("last_error").is_object()) {
        rows.push_back({"last error", error_text(workflow.at("last_error"))});
    }
    std::string text = table(rows);
    std::vector<std::vector<std::string>> steps;
    for (const json& step : snapshot.value("steps", json::array())) {
        std::vector<std::string> row = {
            "  " + shown(step, "step"),
            shown(step, "module_name") + "@" + shown(step, "module_version"), shown(step, "state"),
            shown(step, "invocations") + " invocations (" + shown(step, "succeeded") +
                " succeeded, " + shown(step, "failed") + " failed, " + shown(step, "timed_out") +
                " timed out, " + shown(step, "cancelled") + " cancelled)"};
        if (step.contains("last_error") && step.at("last_error").is_object()) {
            row.back() += "; last error " + error_text(step.at("last_error"));
        }
        steps.push_back(std::move(row));
    }
    if (!steps.empty()) {
        text += "steps:\n" + table(steps);
    }
    return text;
}

} // namespace

int command_status(const std::vector<std::string>& args) {
    const std::string usage = std::string(kUsage) + "\n" + std::string(kGlobalOptionsUsage);
    return run_command(args, usage, {},
                       [](const Arguments& arguments, const GlobalOptions&, const Output& output) {
                           if (arguments.positionals().size() != 1) {
                               throw UsageError("status takes one status file");
                           }
                           const json snapshot = json::parse(
                               Runtime::read_status_file(arguments.positionals().front()));
                           return output.result(snapshot, status_text(snapshot));
                       });
}

} // namespace praenora::cli
