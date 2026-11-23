// praenora status: renders the status file that a run keeps (status-snapshot document), or, with
// --server, what a server of the control API holds: its runs, or one run's status.
#include "commands/commands.hpp"

#include "output.hpp"
#include "serving.hpp"

#include <address.hpp>
#include <client.hpp>
#include <tokens.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

constexpr std::string_view kUsage =
    "usage: praenora status <status.json>\n"
    "       praenora status --server ADDR [--run ID] [--token-file F]\n"
    "\n"
    "Prints the status file that a run keeps (its status_file): the workflow's state, health and\n"
    "ticks, and each step's state and invocation counts.\n"
    "With --server, asks the control API at ADDR (praenora run --listen, praenora serve): without\n"
    "--run, for its runs (id, workflow, state, health and ticks); with it, for that run's status,\n"
    "printed as a status file is. --json prints the documents. The server's errors keep their\n"
    "codes and exit codes; a connection that fails is IO_ERROR (exit 74).\n"
    "\n"
    "  --server ADDR          the listener's address: unix:<path>, <IPv4>:<port> or\n"
    "                         [<IPv6>]:<port>, as the server printed it\n"
    "  --run ID               one run's status, by its run id\n"
    "  --token-file F         send the token on the first line of F (a TCP listener with tokens;\n"
    "                         F readable by its owner only)\n";

const std::vector<Flag> kFlags = {{"--server", true}, {"--run", true}, {"--token-file", true}};

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

// The run list of GET /v0/runs as a table: id, workflow, state, health and ticks.
std::string runs_text(const json& list) {
    std::vector<std::vector<std::string>> rows = {{"id", "workflow", "state", "health", "ticks"}};
    for (const json& entry : list.value("runs", json::array())) {
        if (!entry.is_object()) {
            continue;
        }
        const json workflow = entry.value("workflow", json::object());
        rows.push_back(
            {shown(entry, "id"), shown(workflow, "name") + "@" + shown(workflow, "version"),
             shown(entry, "state"), shown(entry, "health"), shown(entry, "ticks_completed")});
    }
    return table(rows);
}

// Whether `id` is a run id: decimal, without leading zeros.
bool is_run_id(const std::string& id) {
    if (id.empty() || id.front() == '0') {
        return false;
    }
    for (const char c : id) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// What a server of the control API holds: its runs, or the status of `run`.
int server_status(const Arguments& arguments, const Output& output) {
    if (!arguments.positionals().empty()) {
        throw UsageError("status takes a status file or --server, not both");
    }
    const server::ListenAddress address =
        as_usage([&] { return server::parse_listen_address(*arguments.value("--server")); });
    std::optional<std::string> token;
    if (const auto file = arguments.value("--token-file")) {
        token = as_usage([&] { return server::read_token_file(*file); });
        if (address.kind() == server::ListenerKind::unix_socket) {
            throw UsageError("--token-file is refused with a Unix socket (" + address.to_string() +
                             "): the socket's mode is the credential");
        }
    }
    const std::optional<std::string> run = arguments.value("--run");
    if (run && !is_run_id(*run)) {
        throw UsageError("--run '" + *run +
                         "' is not a run id: a decimal number without leading zeros");
    }
    const server::ClientAnswer answer =
        server::get(address, run ? "/v0/runs/" + *run : std::string("/v0/runs"), token);
    if (answer.status != 200) {
        throw server::answer_error(address, answer);
    }
    const json document = json::parse(answer.body, nullptr, false);
    if (!document.is_object()) {
        throw Error(ErrorCode::IO_ERROR, "the control API at " + address.to_string() +
                                             " answered a document that is not a JSON object");
    }
    return output.result(document, run ? status_text(document) : runs_text(document));
}

} // namespace

int command_status(const std::vector<std::string>& args) {
    const std::string usage = std::string(kUsage) + "\n" + global_options_usage();
    return run_command(args, usage, kFlags,
                       [](const Arguments& arguments, const GlobalOptions&, const Output& output) {
                           if (arguments.has("--server")) {
                               return server_status(arguments, output);
                           }
                           for (const char* flag : {"--run", "--token-file"}) {
                               if (arguments.has(flag)) {
                                   throw UsageError(std::string(flag) + " needs --server");
                               }
                           }
                           if (arguments.positionals().size() != 1) {
                               throw UsageError("status takes one status file");
                           }
                           const json snapshot = json::parse(
                               Runtime::read_status_file(arguments.positionals().front()));
                           return output.result(snapshot, status_text(snapshot));
                       });
}

} // namespace praenora::cli
