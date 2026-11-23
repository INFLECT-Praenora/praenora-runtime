// praenora serve: a long-running server of the control API that opens runs on request, from the
// configurations under a configuration root, into directories of an output root
// (docs/specs/control-api.md). SIGINT or SIGTERM shuts it down: it stops listening, asks every
// run to stop, waits for each one to end, closes them and exits with 0; a second signal ends it
// at once with 130 (signals.hpp).
#include "commands/commands.hpp"

#include "output.hpp"
#include "serving.hpp"
#include "signals.hpp"

#include <run_registry.hpp>

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

// The names of the isolation modes come from the table that the tool shares with the Python
// extension (src/frontend).
const std::string kUsage =
    "usage: praenora serve --listen ADDR --config-root D --output-root D\n"
    "                      [--token-file F] [--read-token-file F] [--allow-host H]...\n"
    "                      [--address-file F] [--max-runs N] [--isolation " +
    frontend::names_of(frontend::kIsolationModes, "|", "|") +
    "]\n"
    "\n"
    "Serves the control API on ADDR until SIGINT or SIGTERM (such as Ctrl+C): POST /v0/runs opens\n"
    "a run of a configuration under the configuration root, which writes to\n"
    "<output-root>/run-<k>/ and logs to <output-root>/run-<k>.log; the routes of /v0/runs/{id}\n"
    "watch and stop it, and DELETE forgets it once it is over. On a signal the server stops\n"
    "listening, asks every run to stop, waits for each one to end (a run in a call that does not\n"
    "return ends at its own deadline) and exits with 0; a second signal ends it at once with 130.\n"
    "One server per output root.\n"
    "\n"
    "  --config-root D        the directory whose configurations runs are opened from\n"
    "  --output-root D        the directory of the runs' outputs and logs, created when missing\n"
    "  --max-runs N           the runs held at once, running and finished (default 16, at least\n"
    "                         1); a POST beyond them answers 503\n"
    "  --isolation M          " +
    frontend::names_of(frontend::kIsolationModes) +
    ": every run's isolation; process (POSIX) is\n"
    "                         recommended, since a crash in an in-process module ends every run\n"
    "                         of the server\n" +
    listen_flags_usage() +
    "  --log-level, --log-format\n"
    "                         the runs' logging, each run to its own log file\n";

std::vector<Flag> flags() {
    std::vector<Flag> result = {{"--config-root", true},
                                {"--output-root", true},
                                {"--max-runs", true},
                                {"--isolation", true}};
    const std::vector<Flag>& listening = listen_flags();
    result.insert(result.end(), listening.begin(), listening.end());
    return result;
}

// The value of a flag that serve needs.
std::string required(const Arguments& arguments, const std::string& flag,
                     const std::string& value) {
    const std::optional<std::string> given = arguments.value(flag);
    if (!given) {
        throw UsageError("serve needs " + flag + " " + value);
    }
    return *given;
}

} // namespace

int command_serve(const std::vector<std::string>& args) {
    const std::string usage = kUsage + "\n" + global_options_usage();
    return run_command(
        args, usage, flags(),
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (!arguments.positionals().empty()) {
                throw UsageError("serve takes no positional argument");
            }
            required(arguments, "--listen", "ADDR");
            server::RegistrySettings settings;
            settings.configuration_root = required(arguments, "--config-root", "D");
            settings.output_root = required(arguments, "--output-root", "D");
            const std::optional<ListenSettings> listen = listen_settings(arguments);
            if (const auto max_runs = arguments.value("--max-runs")) {
                settings.max_runs = static_cast<std::size_t>(
                    parse_count(*max_runs, "--max-runs", frontend::kMaxCount));
                if (settings.max_runs == 0) {
                    throw UsageError("--max-runs must be at least 1");
                }
            }
            if (const auto mode = arguments.value("--isolation")) {
                settings.isolation = frontend::isolation_mode(*mode);
                if (!settings.isolation) {
                    throw UsageError("--isolation '" + *mode + "' is not " +
                                     frontend::names_of(frontend::kIsolationModes));
                }
            }
            settings.log_level = globals.log_level;
            settings.log_format = globals.log_format;

            // The environment is written before the Runtime reads it and before the watcher thread
            // starts.
            const RuntimeOptions runtime_settings = runtime_options(globals);
            StopOnSignal stop;
            const Runtime runtime(runtime_settings);
            server::RunRegistry registry(runtime, settings);
            Server serving(*listen, output);
            serving.start(runtime, registry);
            stop.wait_for_signal();
            output.note("shutting down: no new connection; waiting for every run to end (a "
                        "second signal ends the server at once)");
            // The listener first: once it stopped, no handler holds a run of the registry.
            serving.stop();
            registry.shutdown();
            return output.result(json{{"address", serving.address()}}, "");
        });
}

} // namespace praenora::cli
