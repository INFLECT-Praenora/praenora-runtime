// praenora modules: the modules on the module search paths (list, inspect) and the contract
// checks of one module (check).
#include "commands/commands.hpp"

#include "output.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

constexpr std::string_view kUsage =
    "usage: praenora modules list\n"
    "       praenora modules inspect <name>[@<version>] [--load]\n"
    "       praenora modules check <manifest.pmm.json> [--vectors F]\n"
    "\n"
    "list     every module manifest on the module search paths (--module-path, then\n"
    "         PRAENORA_MODULE_PATH, then the installation's module directory)\n"
    "inspect  one module as discovery resolves it, by default its highest version for this\n"
    "         platform; --load also loads its library and reports its describe() document and\n"
    "         the library's SHA-256\n"
    "check    the contract checks of a module, each passed, failed or skipped; the exit code is 1\n"
    "         when one fails. --vectors F runs the test vectors in F instead of the ones the\n"
    "         manifest names (tests.vectors)\n";

std::string usage_text() { return std::string(kUsage) + "\n" + std::string(kGlobalOptionsUsage); }

int list(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (!arguments.positionals().empty()) {
                throw UsageError("modules list takes no arguments");
            }
            const Runtime runtime(runtime_options(globals));
            json result = json::array();
            std::vector<std::vector<std::string>> rows;
            for (const ModuleInfo& module : runtime.list_modules()) {
                result.push_back(frontend::module_json(module));
                rows.push_back({module.name, module.version, module.os + "-" + module.arch,
                                module.manifest_path});
            }
            if (rows.empty()) {
                output.note("no module manifests on the module search paths");
            }
            return output.result(result, table(rows));
        });
}

int inspect(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {{"--load"}},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (arguments.positionals().size() != 1) {
                throw UsageError("modules inspect takes one module, <name>[@<version>]");
            }
            const std::string& text = arguments.positionals().front();
            const std::size_t at = text.find('@');
            const std::string name = text.substr(0, at);
            std::optional<std::string> version;
            if (at != std::string::npos) {
                version = text.substr(at + 1);
            }
            const Runtime runtime(runtime_options(globals));
            const ModuleInfo module =
                runtime.inspect_module(name, version, arguments.has("--load"));
            std::vector<std::vector<std::string>> rows = {
                {"name", module.name},
                {"version", module.version},
                {"platform", module.os + "-" + module.arch},
                {"manifest", module.manifest_path},
                {"library", module.library_path},
                {"capabilities", joined(module.capabilities)}};
            if (!module.describe_json.empty()) {
                rows.push_back({"library sha256", module.library_sha256});
                rows.push_back({"describe", module.describe_json});
            }
            return output.result(frontend::module_json(module), table(rows));
        });
}

int check(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {{"--vectors", true}},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (arguments.positionals().size() != 1) {
                throw UsageError("modules check takes one module manifest");
            }
            const Runtime runtime(runtime_options(globals));
            const ModuleCheckReport report =
                runtime.check_module(arguments.positionals().front(), arguments.value("--vectors"));
            std::vector<std::vector<std::string>> rows;
            std::size_t passed = 0, failed_count = 0, skipped = 0;
            for (const CheckResult& check : report.checks) {
                rows.push_back({check.status, check.id, check.message});
                if (check.status == "pass") {
                    ++passed;
                } else if (check.status == "fail") {
                    ++failed_count;
                } else {
                    ++skipped;
                }
            }
            const std::string summary = std::to_string(report.checks.size()) +
                                        " checks: " + std::to_string(passed) + " passed, " +
                                        std::to_string(failed_count) + " failed, " +
                                        std::to_string(skipped) + " skipped\n";
            if (report.passed()) {
                return output.result(json::parse(report.to_json()), table(rows) + summary);
            }
            if (!output.json()) {
                std::cout << table(rows) << summary;
            }
            // FAILED (exit code 1), with the report in the details.
            return failed(output, frontend::check_failure(report));
        });
}

} // namespace

int command_modules(const std::vector<std::string>& args) {
    return run_subcommand(args, "modules", usage_text(),
                          {{"list", &list}, {"inspect", &inspect}, {"check", &check}});
}

} // namespace praenora::cli
