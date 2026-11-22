// What the praenora tool prints, and its exit codes.
//
// With --json, standard output carries exactly one JSON document: {"ok": true, "result": ...} or
// {"ok": false, "error": <Error::to_json()>, "diagnostics": [<the validation report's
// diagnostics>]}; everything meant for people goes to standard error. Without --json, results go
// to standard output, and diagnostics and errors to standard error, in the compiler-style format
// of ValidationReport::to_text().
#ifndef PRAENORA_CLI_OUTPUT_HPP
#define PRAENORA_CLI_OUTPUT_HPP

#include "args.hpp"
#include "table.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::cli {

// Exit codes. A failure exits with its error code's exit code (error_code_info()); these are the
// codes the tool sets itself.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitValidation = 3;
inline constexpr int kExitInternal = 70;
inline constexpr int kExitIo = 74;
inline constexpr int kExitStopped = 130;

class Output {
public:
    explicit Output(bool json) : json_(json) {}

    bool json() const noexcept { return json_; }

    // A command's result: with --json, `result` in the document; otherwise `text` on standard
    // output, as it is. Returns 0, or 74 when standard output cannot be written.
    int result(const nlohmann::json& result, const std::string& text) const;
    // The same, with the result given as JSON text, which goes into the document as it is.
    int result_text(const std::string& result_json, const std::string& text) const;

    // A failure: `error` and, when not empty, the diagnostics of `report`. People get the
    // diagnostics, then the error, on standard error. Returns `exit_code` (74 when standard
    // output cannot be written).
    int failure(const Error& error, int exit_code, const ValidationReport& report = {}) const;

    // A message for people, on standard error: "praenora: <text>".
    void note(const std::string& text) const;

    // Flushes standard output: `exit_code`, or 74 when it cannot be written.
    int finish(int exit_code) const;

private:
    bool json_;
};

// The validation report that an error's details hold ({"ok", "diagnostics"}: Runtime::open() and
// Runtime::graph() put one there); empty when they hold none.
ValidationReport report_in(const Error& error);

// The exit code of a validation report with errors: 74 when every error is IO_ERROR (such as a
// configuration, document or manifest file that is missing or cannot be read; a data file is
// TARGET_DATA_UNAVAILABLE), otherwise 3, whatever the codes.
int exit_code_of(const ValidationReport& report);

// The exit code of a command that failed with `error`: that of the validation report it carries,
// if any (exit_code_of() above), otherwise its code's.
int exit_code_of(const Error& error);

// Reports `error` with its exit code (exit_code_of()) and the report it carries.
int failed(const Output& output, const Error& error);

// Reports a command-line mistake: "praenora: <message>" and `usage` on standard error (with
// --json, also the error document INVALID_ARGUMENT on standard output). Returns 2.
int usage_failure(const Output& output, const std::string& message, std::string_view usage);

// A command's work, given its parsed arguments and global options.
using CommandBody = std::function<int(const Arguments&, const GlobalOptions&, const Output&)>;

// Runs one command: parses `args` against `flags` and the global flags, prints `usage` to standard
// output for --help (with --json as the result of the success document), and otherwise runs
// `body`. A UsageError is reported with `usage` (exit 2), a
// praenora::Error with failed(), anything else as an internal error (exit 70), each the way
// --json asks.
int run_command(const std::vector<std::string>& args, std::string_view usage,
                const std::vector<Flag>& flags, const CommandBody& body);

// A subcommand (for example `modules list`) and the function that runs it, given the arguments
// around its name.
struct Subcommand {
    std::string_view name;
    int (*run)(const std::vector<std::string>& args);
};

// Runs the subcommand of `command` that the first positional argument of `args` names. Without
// one, --help prints `usage` (exit 0; with --json as the result of the success document) and
// anything else is a usage error, as is a name that is not one of `subcommands`.
int run_subcommand(const std::vector<std::string>& args, std::string_view command,
                   std::string_view usage, const std::vector<Subcommand>& subcommands);

// Compact JSON, with bytes that are not UTF-8 replaced.
std::string dump(const nlohmann::json& value);

// The items, separated by ", ".
std::string joined(const std::vector<std::string>& items);

} // namespace praenora::cli

#endif // PRAENORA_CLI_OUTPUT_HPP
