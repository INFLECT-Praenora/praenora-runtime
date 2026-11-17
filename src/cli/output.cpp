#include "output.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <utility>

namespace praenora::cli {

using nlohmann::json;

namespace {

// The diagnostic of one item of a validation report's JSON (ValidationReport::to_json()), its
// details included.
Diagnostic diagnostic_from(const json& item) {
    Diagnostic diagnostic;
    const std::string severity = item.value("severity", std::string("error"));
    diagnostic.severity = severity == "warning" ? Severity::warning
                          : severity == "note"  ? Severity::note
                                                : Severity::error;
    const auto code = error_code_from_name(item.value("name", std::string()));
    diagnostic.code = code ? *code : ErrorCode::UNKNOWN;
    diagnostic.message = item.value("message", std::string());
    const json location = item.value("location", json::object());
    diagnostic.location.file = location.value("file", std::string());
    diagnostic.location.line = location.value("line", std::int64_t{0});
    diagnostic.location.column = location.value("column", std::int64_t{0});
    diagnostic.location.pointer = location.value("pointer", std::string());
    diagnostic.step = item.value("step", std::string());
    diagnostic.module = item.value("module", std::string());
    if (const auto details = item.find("details"); details != item.end()) {
        diagnostic.details_json = dump(*details);
    }
    return diagnostic;
}

// The error as one line in the compiler-style format, "praenora: " first when it has no file.
std::string error_line(const Error& error) {
    ValidationReport single;
    Diagnostic diagnostic;
    diagnostic.severity = Severity::error;
    diagnostic.code = error.code();
    diagnostic.message = error.what();
    diagnostic.location = error.location();
    single.add(std::move(diagnostic));
    return (error.location().file.empty() ? "praenora: " : "") + single.to_text();
}

} // namespace

std::string dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) {
        text += (text.empty() ? "" : ", ") + item;
    }
    return text;
}

int Output::result(const nlohmann::json& result, const std::string& text) const {
    return result_text(json_ ? dump(result) : std::string(), text);
}

int Output::result_text(const std::string& result_json, const std::string& text) const {
    if (json_) {
        std::cout << "{\"ok\":true,\"result\":" << result_json << "}\n";
    } else {
        std::cout << text;
    }
    return finish(kExitOk);
}

int Output::failure(const Error& error, int exit_code, const ValidationReport& report) const {
    if (report.diagnostics.empty()) {
        std::cerr << error_line(error);
    } else {
        std::cerr << report.to_text() << "praenora: " << error.what() << "\n";
    }
    std::cerr.flush();
    if (json_) {
        // (Within Output, `json` names the member function.)
        nlohmann::json diagnostics = nlohmann::json::array();
        if (!report.diagnostics.empty()) {
            diagnostics = nlohmann::json::parse(report.to_json()).at("diagnostics");
        }
        std::cout << "{\"ok\":false,\"error\":" << error.to_json()
                  << ",\"diagnostics\":" << dump(diagnostics) << "}\n";
    }
    return finish(exit_code);
}

void Output::note(const std::string& text) const {
    std::cerr << "praenora: " << text << "\n";
    std::cerr.flush();
}

int Output::finish(int exit_code) const {
    std::cout.flush();
    return std::cout ? exit_code : kExitIo;
}

ValidationReport report_in(const Error& error) {
    ValidationReport report;
    const json details = json::parse(error.details_json(), nullptr, false);
    if (!details.is_object() || !details.contains("ok") || !details.at("ok").is_boolean()) {
        return report;
    }
    const auto diagnostics = details.find("diagnostics");
    if (diagnostics == details.end() || !diagnostics->is_array()) {
        return report;
    }
    for (const json& item : *diagnostics) {
        if (item.is_object()) {
            report.add(diagnostic_from(item));
        }
    }
    return report;
}

int exit_code_of(const ValidationReport& report) {
    bool all_io = false;
    for (const Diagnostic& diagnostic : report.diagnostics) {
        if (diagnostic.severity == Severity::error) {
            if (diagnostic.code != ErrorCode::IO_ERROR) {
                return kExitValidation;
            }
            all_io = true;
        }
    }
    return all_io ? kExitIo : kExitValidation;
}

int exit_code_of(const Error& error) {
    const ValidationReport report = report_in(error);
    if (!report.diagnostics.empty()) {
        return exit_code_of(report);
    }
    return error_code_info(error.code()).exit_code;
}

int failed(const Output& output, const Error& error) {
    return output.failure(error, exit_code_of(error), report_in(error));
}

int usage_failure(const Output& output, const std::string& message, std::string_view usage) {
    std::cerr << "praenora: " << message << "\n\n" << usage;
    std::cerr.flush();
    if (output.json()) {
        const Error error(ErrorCode::INVALID_ARGUMENT, message);
        std::cout << "{\"ok\":false,\"error\":" << error.to_json() << ",\"diagnostics\":[]}\n";
    }
    return output.finish(kExitUsage);
}

int run_subcommand(const std::vector<std::string>& args, std::string_view command,
                   std::string_view usage, const std::vector<Subcommand>& subcommands) {
    const Output output(asks_for_json(args));
    std::vector<std::string> names;
    for (const Subcommand& subcommand : subcommands) {
        names.emplace_back(subcommand.name);
    }
    const auto found = split_command(args);
    if (!found) {
        if (std::find(args.begin(), args.end(), "--help") != args.end() ||
            std::find(args.begin(), args.end(), "-h") != args.end()) {
            return output.result(nlohmann::json(std::string(usage)), std::string(usage));
        }
        return usage_failure(output, std::string(command) + " needs a subcommand: " + joined(names),
                             usage);
    }
    for (const Subcommand& subcommand : subcommands) {
        if (found->first == subcommand.name) {
            return subcommand.run(found->second);
        }
    }
    return usage_failure(output,
                         "unknown " + std::string(command) + " subcommand '" + found->first +
                             "'; the subcommands are " + joined(names),
                         usage);
}

int run_command(const std::vector<std::string>& args, std::string_view usage,
                const std::vector<Flag>& flags, const CommandBody& body) {
    const Output output(asks_for_json(args));
    try {
        const Arguments arguments(args, flags);
        const GlobalOptions globals = global_options(arguments);
        if (globals.help) {
            return output.result(nlohmann::json(std::string(usage)), std::string(usage));
        }
        return body(arguments, globals, output);
    } catch (const UsageError& error) {
        return usage_failure(output, error.what(), usage);
    } catch (const Error& error) {
        return failed(output, error);
    } catch (const std::exception& error) {
        return output.failure(
            Error(ErrorCode::INTERNAL, std::string("internal error: ") + error.what()),
            kExitInternal);
    }
}

} // namespace praenora::cli
