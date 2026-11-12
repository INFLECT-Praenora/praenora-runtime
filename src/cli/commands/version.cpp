// praenora version: the runtime, SDK, protocol and ABI versions, the document format versions
// and the build facts.
#include "commands/commands.hpp"

#include "output.hpp"

#include <praenora/runtime/version.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace praenora::cli {

namespace {

constexpr std::string_view kUsage =
    "usage: praenora version [--json]\n"
    "\n"
    "Prints the runtime, SDK, protocol and ABI versions, the format version of each document\n"
    "kind, and the platform, compiler and build type.\n";

std::string version_text(const VersionInfo& info) {
    std::string text = "praenora " + info.runtime + "\n" + "  sdk         " + info.sdk + "\n" +
                       "  pmp         " + info.pmp + "\n" + "  abi         " +
                       std::to_string(info.abi_major) + "." + std::to_string(info.abi_minor) +
                       "\n" + "  platform    " + info.platform + "\n" + "  compiler    " +
                       info.compiler + "\n" + "  build type  " + info.build_type + "\n" +
                       "  formats\n";
    std::size_t width = 0;
    for (const auto& entry : info.formats) {
        width = std::max(width, entry.first.size());
    }
    for (const auto& [kind, format_version] : info.formats) {
        text += "    " + kind + std::string(width - kind.size() + 2, ' ') + format_version + "\n";
    }
    return text;
}

} // namespace

int command_version(const std::vector<std::string>& args) {
    const std::string usage = std::string(kUsage) + "\n" + std::string(kGlobalOptionsUsage);
    return run_command(args, usage, {},
                       [](const Arguments& arguments, const GlobalOptions&, const Output& output) {
                           if (!arguments.positionals().empty()) {
                               throw UsageError("version takes no arguments");
                           }
                           return output.result_text(version_info_json(),
                                                     version_text(version_info()));
                       });
}

} // namespace praenora::cli
