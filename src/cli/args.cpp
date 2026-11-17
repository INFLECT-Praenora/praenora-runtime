#include "args.hpp"

#include <frontend.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace praenora::cli {

namespace {

const Flag* find_flag(const std::vector<Flag>& flags, std::string_view name) {
    const auto found = std::find_if(flags.begin(), flags.end(),
                                    [name](const Flag& flag) { return flag.name == name; });
    return found == flags.end() ? nullptr : &*found;
}

void set_environment(const char* name, std::string_view value) {
    const std::string text(value);
#ifdef _WIN32
    _putenv_s(name, text.c_str());
#else
    setenv(name, text.c_str(), 1);
#endif
}

} // namespace

const std::vector<Flag>& global_flags() {
    static const std::vector<Flag> flags = {
        {"--json"},
        {"--help"},
        {"-h"},
        {"--module-path", true, true},
        {"--provider-path", true, true},
        {"--catalog-path", true, true},
        {"--log-level", true},
        {"--log-format", true},
        {"--log-file", true},
    };
    return flags;
}

Arguments::Arguments(const std::vector<std::string>& args, const std::vector<Flag>& flags) {
    bool flags_ended = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (flags_ended || arg.size() < 2 || arg[0] != '-') {
            positionals_.push_back(arg);
            continue;
        }
        if (arg == "--") {
            flags_ended = true;
            continue;
        }
        std::string name = arg;
        std::optional<std::string> attached;
        if (arg.compare(0, 2, "--") == 0) {
            if (const auto equals = arg.find('='); equals != std::string::npos) {
                name = arg.substr(0, equals);
                attached = arg.substr(equals + 1);
            }
        }
        const Flag* flag = find_flag(flags, name);
        if (flag == nullptr) {
            flag = find_flag(global_flags(), name);
        }
        if (flag == nullptr) {
            throw UsageError("unknown option '" + name + "'");
        }
        std::string value;
        if (flag->takes_value) {
            if (attached) {
                value = *attached;
            } else if (i + 1 < args.size()) {
                value = args[++i];
            } else {
                throw UsageError("option " + name + " needs a value");
            }
        } else if (attached) {
            throw UsageError("option " + name + " takes no value");
        }
        auto& values = flags_[std::string(flag->name)];
        if (!values.empty() && !flag->repeatable) {
            throw UsageError("option " + name + " is given more than once");
        }
        values.push_back(std::move(value));
    }
}

bool Arguments::has(std::string_view flag) const { return flags_.find(flag) != flags_.end(); }

std::optional<std::string> Arguments::value(std::string_view flag) const {
    const auto found = flags_.find(flag);
    if (found == flags_.end()) {
        return std::nullopt;
    }
    return found->second.back();
}

std::vector<std::string> Arguments::values(std::string_view flag) const {
    const auto found = flags_.find(flag);
    return found == flags_.end() ? std::vector<std::string>() : found->second;
}

const std::vector<std::string>& Arguments::positionals() const noexcept { return positionals_; }

std::optional<std::pair<std::string, std::vector<std::string>>>
split_command(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            break;
        }
        if (arg.size() < 2 || arg[0] != '-') {
            const auto at = args.begin() + static_cast<std::ptrdiff_t>(i);
            std::vector<std::string> rest(args.begin(), at);
            rest.insert(rest.end(), at + 1, args.end());
            return std::make_pair(arg, std::move(rest));
        }
        const Flag* flag = find_flag(global_flags(), arg);
        if (flag != nullptr && flag->takes_value) {
            ++i; // its value
        }
    }
    return std::nullopt;
}

bool asks_for_json(const std::vector<std::string>& args) {
    for (const std::string& arg : args) {
        if (arg == "--") {
            return false;
        }
        if (arg == "--json") {
            return true;
        }
    }
    return false;
}

GlobalOptions global_options(const Arguments& arguments) {
    GlobalOptions options;
    options.json = arguments.has("--json");
    options.help = arguments.has("--help") || arguments.has("-h");
    options.module_paths = arguments.values("--module-path");
    options.provider_paths = arguments.values("--provider-path");
    options.catalog_paths = arguments.values("--catalog-path");
    if (const auto value = arguments.value("--log-level")) {
        options.log_level = frontend::log_level_from_name(*value);
        if (!options.log_level) {
            throw UsageError("--log-level '" + *value +
                             "' is not a log level: " + frontend::names_of(frontend::kLogLevels));
        }
    }
    if (const auto value = arguments.value("--log-format")) {
        options.log_format = frontend::log_format_from_name(*value);
        if (!options.log_format) {
            throw UsageError("--log-format '" + *value +
                             "' is not a log format: " + frontend::names_of(frontend::kLogFormats));
        }
    }
    options.log_file = arguments.value("--log-file");
    return options;
}

RuntimeOptions runtime_options(const GlobalOptions& options) {
    if (options.log_level) {
        set_environment("PRAENORA_LOG_LEVEL", frontend::log_level_name(*options.log_level));
    }
    if (options.log_format) {
        set_environment("PRAENORA_LOG_FORMAT", frontend::log_format_name(*options.log_format));
    }
    RuntimeOptions runtime;
    runtime.module_paths = options.module_paths;
    runtime.provider_paths = options.provider_paths;
    runtime.catalog_paths = options.catalog_paths;
    runtime.use_environment = true;
    if (options.log_file) {
        runtime.logging.file = *options.log_file;
    }
    return runtime;
}

std::uint64_t parse_count(const std::string& text, std::string_view flag, std::uint64_t max) {
    const auto invalid = [&] {
        return UsageError(std::string(flag) + " '" + text + "' is not a whole number from 0 to " +
                          std::to_string(max));
    };
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        throw invalid();
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (digit > max || value > (max - digit) / 10) {
            throw invalid();
        }
        value = value * 10 + digit;
    }
    return value;
}

} // namespace praenora::cli
