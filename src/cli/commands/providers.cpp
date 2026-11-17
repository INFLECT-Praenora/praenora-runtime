// praenora providers: the service providers on the provider search paths (list).
#include "commands/commands.hpp"

#include "output.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

constexpr std::string_view kUsage =
    "usage: praenora providers list\n"
    "\n"
    "list  every service provider manifest on the provider search paths (--provider-path, then\n"
    "      PRAENORA_PROVIDER_PATH, then the installation's provider directory), in search order:\n"
    "      its name, version, platform, services and manifest, and whether this runtime can use\n"
    "      it or why not. A manifest shadowed by an earlier one of the same name and platform is\n"
    "      never used: it is listed unusable, naming the manifest that shadows it. Nothing is\n"
    "      loaded.\n";

std::string usage_text() { return std::string(kUsage) + "\n" + std::string(kGlobalOptionsUsage); }

int list(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (!arguments.positionals().empty()) {
                throw UsageError("providers list takes no arguments");
            }
            const Runtime runtime(runtime_options(globals));
            json result = json::array();
            std::vector<std::vector<std::string>> rows;
            for (const ProviderInfo& provider : runtime.list_providers()) {
                result.push_back(frontend::provider_json(provider));
                rows.push_back({provider.name, provider.version, provider.os + "-" + provider.arch,
                                joined(provider.services), provider.manifest_path,
                                provider.usable ? std::string("usable")
                                                : "unusable: " + provider.problem});
            }
            if (rows.empty()) {
                output.note("no provider manifests on the provider search paths");
            }
            return output.result(result, table(rows));
        });
}

} // namespace

int command_providers(const std::vector<std::string>& args) {
    return run_subcommand(args, "providers", usage_text(), {{"list", &list}});
}

} // namespace praenora::cli
