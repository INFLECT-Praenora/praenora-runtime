// praenora: the command-line interface to the Praenora runtime, a thin layer over the C++ API
// (praenora::Runtime and praenora::Workflow). Arguments are parsed by hand (args.hpp); the output
// rules and exit codes are those of output.hpp.
#include "args.hpp"
#include "commands/commands.hpp"
#include "output.hpp"

#include <frontend.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

namespace {

using namespace praenora::cli;
namespace frontend = praenora::frontend;

// The names of the graph formats come from the table that the tool shares with the Python
// extension (src/frontend).
const std::string kUsage =
    "usage: praenora <command> [arguments] [options]\n"
    "\n"
    "commands:\n"
    "  version                versions of the runtime, SDK, protocol, ABI and document formats\n"
    "  validate <config.yaml> check a workflow configuration without running it\n"
    "  validate --document F  check any Praenora document against the schema of its kind\n"
    "  modules list           the modules on the module search paths\n"
    "  modules inspect <name>[@<version>]\n"
    "                         one module, as discovery resolves it\n"
    "  modules check <manifest.pmm.json>\n"
    "                         the contract checks of a module\n"
    "  providers list         the service providers on the provider search paths\n"
    "  graph <config.yaml> | --workflow <name>@<version>\n"
    "                         a workflow definition's graph: " +
    frontend::names_of(frontend::kGraphFormats) +
    "\n"
    "  run <config.yaml>      run a workflow\n"
    "  status <status.json>   print the status file of a run\n"
    "  catalog list | catalog show <id>@<version>\n"
    "                         the protocol catalog's entries\n"
    "  schema list | schema show <kind>\n"
    "                         the embedded document schemas\n"
    "\n"
    "Run 'praenora <command> --help' for the arguments and options of a command.\n"
    "Exit codes: 0 ok, 1 execution failed, 2 usage, 3 validation, 4 load, 5 timeout,\n"
    "70 internal, 74 I/O, 130 interrupted or stopped.\n";

std::string usage_text() { return kUsage + "\n" + global_options_usage(); }

struct Command {
    std::string_view name;
    int (*run)(const std::vector<std::string>& args);
};

const Command kCommands[] = {
    {"version", &command_version},
    {"validate", &command_validate},
    {"modules", &command_modules},
    {"providers", &command_providers},
    {"graph", &command_graph},
    {"run", &command_run},
    {"status", &command_status},
    {"catalog", &command_catalog},
    {"schema", &command_schema},
};

const Command* find_command(std::string_view name) {
    for (const Command& command : kCommands) {
        if (command.name == name) {
            return &command;
        }
    }
    return nullptr;
}

int run_tool(const std::vector<std::string>& args) {
    const Output output(asks_for_json(args));
    const auto found = split_command(args);
    const bool help = std::find(args.begin(), args.end(), "--help") != args.end() ||
                      std::find(args.begin(), args.end(), "-h") != args.end();
    if (!found) {
        if (help) {
            return output.result(nlohmann::json(usage_text()), usage_text());
        }
        return usage_failure(output, "no command given", usage_text());
    }
    const auto& [name, rest] = *found;
    if (name == "help") {
        // `praenora help` and `praenora help <command>`.
        const auto topic = split_command(rest);
        if (!topic) {
            return output.result(nlohmann::json(usage_text()), usage_text());
        }
        if (const Command* command = find_command(topic->first)) {
            std::vector<std::string> help_args = {"--help"};
            if (output.json()) {
                help_args.emplace_back("--json");
            }
            return command->run(help_args);
        }
        return usage_failure(output, "unknown command '" + topic->first + "'", usage_text());
    }
    if (const Command* command = find_command(name)) {
        return command->run(rest);
    }
    return usage_failure(output, "unknown command '" + name + "'", usage_text());
}

int guarded(const std::vector<std::string>& args) {
    try {
        return run_tool(args);
    } catch (const std::exception& error) {
        std::cerr << "praenora: internal error: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "praenora: internal error\n";
    }
    return kExitInternal;
}

} // namespace

#ifdef _WIN32
// The arguments arrive as UTF-16 and become UTF-8, the encoding of every path the API takes. The
// console shows UTF-8, and standard output and error are binary, so lines end in "\n" only.
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size > 0 ? static_cast<std::size_t>(size) : 1u, '\0');
        if (size > 0) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), size, nullptr, nullptr);
        }
        arg.resize(arg.size() - 1); // the terminating NUL
        args.push_back(std::move(arg));
    }
    return guarded(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return guarded(args);
}
#endif
