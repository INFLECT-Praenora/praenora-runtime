// The commands of the praenora tool. Each takes the arguments that follow its name (global flags
// included) and returns the process's exit code; main.cpp dispatches to them.
#ifndef PRAENORA_CLI_COMMANDS_COMMANDS_HPP
#define PRAENORA_CLI_COMMANDS_COMMANDS_HPP

#include <frontend.hpp>

#include <string>
#include <vector>

namespace praenora::cli {

int command_version(const std::vector<std::string>& args);
int command_validate(const std::vector<std::string>& args);
int command_modules(const std::vector<std::string>& args);
int command_providers(const std::vector<std::string>& args);
int command_graph(const std::vector<std::string>& args);
int command_run(const std::vector<std::string>& args);
int command_serve(const std::vector<std::string>& args);
int command_status(const std::vector<std::string>& args);
int command_catalog(const std::vector<std::string>& args);
int command_schema(const std::vector<std::string>& args);

// The options every command accepts, for the usage texts; the names of the log levels and formats
// come from the tables that the tool shares with the Python extension (src/frontend).
inline std::string global_options_usage() {
    return "options of every command:\n"
           "  --json                 print exactly one JSON document on standard output:\n"
           "                         {\"ok\": true, \"result\": ...} or\n"
           "                         {\"ok\": false, \"error\": ..., \"diagnostics\": [...]}\n"
           "  --module-path P        search P for modules first (repeatable)\n"
           "  --provider-path P      search P for service providers first (repeatable)\n"
           "  --catalog-path P       add the catalog entries in P (repeatable)\n"
           "  --log-level L          " +
           frontend::names_of(frontend::kLogLevels) +
           "\n"
           "  --log-format F         " +
           frontend::names_of(frontend::kLogFormats) +
           "\n"
           "  --log-file F           also log to file F\n"
           "  -h, --help             print the usage\n";
}

} // namespace praenora::cli

#endif // PRAENORA_CLI_COMMANDS_COMMANDS_HPP
