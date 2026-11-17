// The praenora tool's command line: flags with and without values and positional arguments,
// parsed by hand; the options every command accepts; and the runtime options they make.
#ifndef PRAENORA_CLI_ARGS_HPP
#define PRAENORA_CLI_ARGS_HPP

#include <praenora/runtime/options.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::cli {

// A mistake on the command line. The tool reports it with the command's usage and exits with 2.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A flag: "--name" (or a short "-h"), with a value ("--name V" or "--name=V") or without one. A
// repeatable flag may be given more than once.
struct Flag {
    std::string_view name;
    bool takes_value = false;
    bool repeatable = false;
};

// The flags every command accepts: --json, --module-path P, --provider-path P, --catalog-path P
// (the three repeatable), --log-level L, --log-format F, --log-file F and --help (or -h).
const std::vector<Flag>& global_flags();

// One command's arguments, parsed against its own flags and the global flags. Flags may come
// before, between and after the positional arguments; "--" ends the flags.
class Arguments {
public:
    // Throws UsageError for an unknown flag, a flag without its value, a value given to a flag
    // that takes none, and a flag that is not repeatable given twice.
    Arguments(const std::vector<std::string>& args, const std::vector<Flag>& flags);

    bool has(std::string_view flag) const;
    // The value of a flag that was given, or nullopt.
    std::optional<std::string> value(std::string_view flag) const;
    // Every value of a repeatable flag, in the order given.
    std::vector<std::string> values(std::string_view flag) const;
    const std::vector<std::string>& positionals() const noexcept;

private:
    std::map<std::string, std::vector<std::string>, std::less<>> flags_;
    std::vector<std::string> positionals_;
};

// The first positional argument of `args`, skipping the global flags (and their values), and the
// arguments without it: how the tool finds a command or a subcommand. nullopt when there is none.
std::optional<std::pair<std::string, std::vector<std::string>>>
split_command(const std::vector<std::string>& args);

// Whether `args` holds --json (before any "--"); known even when the rest does not parse, so
// that a usage error can be reported as JSON too.
bool asks_for_json(const std::vector<std::string>& args);

// The global options, checked.
struct GlobalOptions {
    bool json = false;
    bool help = false;
    std::vector<std::string> module_paths, provider_paths, catalog_paths;
    std::optional<LogLevel> log_level;
    std::optional<LogFormat> log_format;
    std::optional<std::string> log_file;
};

// The global options of `arguments`. --log-level takes trace, debug, info, warn, error, critical
// or off, --log-format text or json, in any case; anything else is a UsageError.
GlobalOptions global_options(const Arguments& arguments);

// The runtime options of a command: its module, provider and catalog paths, the PRAENORA_*
// environment variables honored, and --log-file as the runtime's log file. Before it returns, it
// writes the --log-level and --log-format given, by their canonical names, into the process's
// environment (PRAENORA_LOG_LEVEL, PRAENORA_LOG_FORMAT), which a Runtime reads when it is
// constructed, so that they win over the environment's own values for the runtime's logging too.
// Call it before constructing the Runtime and before any thread starts.
RuntimeOptions runtime_options(const GlobalOptions& options);

// A decimal integer from 0 to `max`; otherwise UsageError naming `flag`.
std::uint64_t parse_count(const std::string& text, std::string_view flag, std::uint64_t max);

} // namespace praenora::cli

#endif // PRAENORA_CLI_ARGS_HPP
