// praenora validate: checks a workflow configuration (everything a run checks before tick 0) or
// any Praenora document, without running anything.
#include "commands/commands.hpp"

#include "output.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::cli {

namespace {

constexpr std::string_view kUsage =
    "usage: praenora validate <config.yaml> [--load]\n"
    "       praenora validate --document <file>\n"
    "\n"
    "Checks a workflow configuration: the file, the catalog, the graph, the bindings, the modules\n"
    "and the runtime services they require (without loading anything), the parameters and the\n"
    "inputs; with --load the service providers of those services are loaded and started, and the\n"
    "module libraries loaded and their identities checked, too. With --document, checks any\n"
    "Praenora document (JSON or YAML) against the schema of its kind. Every problem is reported,\n"
    "located in its file; the exit code is 3 when there are errors (SERVICE_UNAVAILABLE among\n"
    "them: no provider on the provider search paths serves a service a module requires), or 74\n"
    "when every error is IO_ERROR, such as a configuration, document or manifest file that is\n"
    "missing or cannot be read (but a data file that is missing or cannot be read is\n"
    "TARGET_DATA_UNAVAILABLE, exit 3).\n"
    "\n"
    "  --load                 also start the service providers, and load each module library\n"
    "                         and check its identity\n"
    "  --document <file>      the document to check\n";

const std::vector<Flag> kFlags = {{"--load"}, {"--document", true}};

// Reports `report` about `what`: valid, or the failure with its first error's code and location.
int report_result(const Output& output, const ValidationReport& report, const std::string& what) {
    if (report.first_error() == nullptr) {
        if (!report.diagnostics.empty()) {
            std::cerr << report.to_text(); // the warnings and notes
        }
        const auto warnings =
            std::count_if(report.diagnostics.begin(), report.diagnostics.end(),
                          [](const Diagnostic& d) { return d.severity == Severity::warning; });
        return output.result(
            nlohmann::json::parse(report.to_json()),
            what + ": valid" +
                (warnings > 0 ? " (" + std::to_string(warnings) + " warning(s))" : std::string()) +
                "\n");
    }
    return output.failure(frontend::validation_failure(report, what), exit_code_of(report), report);
}

} // namespace

int command_validate(const std::vector<std::string>& args) {
    const std::string usage = std::string(kUsage) + "\n" + global_options_usage();
    return run_command(
        args, usage, kFlags,
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            const auto& positionals = arguments.positionals();
            const std::optional<std::string> document = arguments.value("--document");
            if (document ? !positionals.empty() || arguments.has("--load")
                         : positionals.size() != 1) {
                throw UsageError("validate takes one configuration file, or --document <file>");
            }
            const Runtime runtime(runtime_options(globals));
            if (document) {
                return report_result(output, runtime.validate_document(*document), *document);
            }
            ValidateOptions options;
            options.load = arguments.has("--load");
            return report_result(output, runtime.validate(positionals.front(), options),
                                 positionals.front());
        });
}

} // namespace praenora::cli
