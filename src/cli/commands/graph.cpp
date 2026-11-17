// praenora graph: the graph of a workflow definition, as text, JSON, DOT or AsyncAPI.
#include "commands/commands.hpp"

#include "output.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace praenora::cli {

namespace {

// The names of the formats come from the table that the tool shares with the Python extension
// (src/frontend); the paragraph that describes each format names them itself, and changes with
// that table.
const std::string kUsage =
    "usage: praenora graph (<config.yaml> | --workflow <name>@<version>)\n"
    "                      [--format " +
    frontend::names_of(frontend::kGraphFormats, "|", "|") +
    "]\n"
    "\n"
    "Prints the graph of a workflow definition of the catalog, or of the one a configuration\n"
    "pins: text (the schedule and the connections; the default), json, dot (Graphviz; delayed\n"
    "connections dashed) or asyncapi (an AsyncAPI 3.0.0 document of its messages).\n"
    "\n"
    "  --workflow <name>@<version>  the workflow definition\n"
    "  --format F             " +
    frontend::names_of(frontend::kGraphFormats) + "\n";

const std::vector<Flag> kFlags = {{"--workflow", true}, {"--format", true}};

} // namespace

int command_graph(const std::vector<std::string>& args) {
    const std::string usage = kUsage + "\n" + global_options_usage();
    return run_command(
        args, usage, kFlags,
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            GraphRequest request;
            request.workflow = arguments.value("--workflow");
            const auto& positionals = arguments.positionals();
            if (request.workflow ? !positionals.empty() : positionals.size() != 1) {
                throw UsageError(
                    "graph takes one configuration file, or --workflow <name>@<version>");
            }
            if (!request.workflow) {
                request.config_path = positionals.front();
            }
            const std::string format = arguments.value("--format").value_or("text");
            const std::optional<GraphFormat> graph_format = frontend::graph_format(format);
            if (!graph_format) {
                throw UsageError("--format '" + format + "' is not " +
                                 frontend::names_of(frontend::kGraphFormats));
            }
            request.format = *graph_format;
            const Runtime runtime(runtime_options(globals));
            const std::string text = runtime.graph(request);
            // In a JSON document, the JSON formats are values, the others strings.
            const bool is_json =
                request.format == GraphFormat::json || request.format == GraphFormat::asyncapi;
            return output.result(is_json ? nlohmann::json::parse(text) : nlohmann::json(text),
                                 text);
        });
}

} // namespace praenora::cli
