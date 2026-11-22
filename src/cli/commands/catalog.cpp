// praenora catalog: the protocol catalog's entries (list) and one entry's document (show).
#include "commands/commands.hpp"

#include "output.hpp"

#include <frontend.hpp>

#include <praenora/runtime/runtime.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace praenora::cli {

namespace {

using nlohmann::json;

// The names of the kinds come from the table that the tool shares with the Python extension
// (src/frontend).
const std::string kUsage =
    "usage: praenora catalog list [--kind " +
    frontend::names_of(frontend::kCatalogKinds, "|", "|") +
    "]\n"
    "       praenora catalog show <id>@<version>\n"
    "\n"
    "list  every entry of the protocol catalog (the built-in entries and those of the catalog\n"
    "      paths): its kind, id, version, status and summary\n"
    "show  the document of one entry\n";

std::string usage_text() { return kUsage + "\n" + global_options_usage(); }

int list(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {{"--kind", true}},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            if (!arguments.positionals().empty()) {
                throw UsageError("catalog list takes no arguments");
            }
            std::string kind;
            if (const auto value = arguments.value("--kind")) {
                const std::optional<std::string> entry_kind = frontend::catalog_kind(*value);
                if (!entry_kind) {
                    throw UsageError("--kind '" + *value + "' is not " +
                                     frontend::names_of(frontend::kCatalogKinds));
                }
                kind = *entry_kind;
            }
            const Runtime runtime(runtime_options(globals));
            json result = json::array();
            std::vector<std::vector<std::string>> rows;
            for (const CatalogEntryInfo& entry : runtime.list_catalog()) {
                if (!kind.empty() && entry.kind != kind) {
                    continue;
                }
                result.push_back(frontend::catalog_entry_json(entry));
                rows.push_back({entry.kind, entry.id, entry.version, entry.status, entry.summary});
            }
            return output.result(result, table(rows));
        });
}

int show(const std::vector<std::string>& args) {
    return run_command(
        args, usage_text(), {},
        [](const Arguments& arguments, const GlobalOptions& globals, const Output& output) {
            const auto& positionals = arguments.positionals();
            const std::size_t at =
                positionals.size() == 1 ? positionals.front().find('@') : std::string::npos;
            if (at == std::string::npos || at == 0 || at + 1 == positionals.front().size()) {
                throw UsageError("catalog show takes one entry, <id>@<version>");
            }
            const Runtime runtime(runtime_options(globals));
            const json entry = json::parse(runtime.catalog_entry(
                positionals.front().substr(0, at), positionals.front().substr(at + 1)));
            return output.result(entry,
                                 entry.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
        });
}

} // namespace

int command_catalog(const std::vector<std::string>& args) {
    return run_subcommand(args, "catalog", usage_text(), {{"list", &list}, {"show", &show}});
}

} // namespace praenora::cli
