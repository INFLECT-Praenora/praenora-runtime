// The environment's manifest, describe()'s document and the variables' warnings
// (environment.hpp).
#include "environment.hpp"

#include "documents.hpp"
#include "environment_files.hpp"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace praenora::detail::python {

namespace {

namespace fs = std::filesystem;

constexpr const char* kManifest = "praenora-runtime-pyenv.json";

std::string quoted(const fs::path& path) { return "'" + path.u8string() + "'"; }

} // namespace

Environment read_environment(const fs::path& directory) {
    const fs::path file = directory / kManifest;
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        fail(PRAENORA_STATUS_IO_ERROR,
             "cannot read the manifest " + quoted(file) + " of the Python environment",
             json{{"path", file.u8string()}});
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json manifest = json::parse(text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             "the manifest " + quoted(file) + " of the Python environment is not a JSON object",
             json{{"path", file.u8string()}});
    }
    const auto text_at = [&](std::initializer_list<const char*> keys) {
        std::string name;
        std::optional<std::string> text = files::string_at(manifest, keys, name);
        if (!text) {
            fail(PRAENORA_STATUS_DATA_INVALID,
                 "the manifest " + quoted(file) + " of the Python environment has no " + name,
                 json{{"path", file.u8string()}, {"key", name}});
        }
        return std::move(*text);
    };

    Environment environment;
    environment.directory = directory.lexically_normal();
    environment.manifest = environment.directory / kManifest;
    environment.name = text_at({"name"});
    environment.version = text_at({"version"});
    environment.package = text_at({"package"});
    environment.lock_sha256 = text_at({"lock", "sha256"});
    environment.python_implementation = text_at({"python", "implementation"});
    environment.python_version = text_at({"python", "version"});
    environment.python_abi = text_at({"python", "abi"});
    environment.home = files::resolved(environment.directory, text_at({"python", "home"}));
    environment.executable =
        files::resolved(environment.directory, text_at({"python", "executable"}));
    const auto section = manifest.find("environment");
    if (section != manifest.end() && section->contains("paths")) {
        const json& paths = (*section)["paths"];
        if (!paths.is_object()) {
            fail(PRAENORA_STATUS_DATA_INVALID,
                 "the manifest " + quoted(file) + " gives environment.paths, not an object",
                 json{{"path", file.u8string()}, {"key", "environment.paths"}});
        }
        for (const auto& [name, value] : paths.items()) {
            if (!value.is_string()) {
                fail(PRAENORA_STATUS_DATA_INVALID,
                     "the manifest " + quoted(file) + " gives environment.paths." + name +
                         ", not a string",
                     json{{"path", file.u8string()}, {"key", "environment.paths." + name}});
            }
            environment.paths.emplace_back(
                name, files::resolved(environment.directory, value.get<std::string>()));
        }
    }
    return environment;
}

std::string describe_environment(const Environment& environment) {
    const json document{{"python",
                         {{"implementation", environment.python_implementation},
                          {"version", environment.python_version},
                          {"abi", environment.python_abi}}},
                        {"environment",
                         {{"name", environment.name},
                          {"version", environment.version},
                          {"package", environment.package},
                          {"lock_sha256", environment.lock_sha256}}}};
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::vector<std::string> variable_warnings(const Environment& environment) {
    std::vector<std::string> warnings;
    for (const auto& [name, path] : environment.paths) {
        if (name == "PYTHONHOME") {
            continue; // the core gives the interpreter its home itself
        }
        const std::optional<std::string> value = environment_variable(name);
        if (!value) {
            warnings.push_back("the environment variable " + name +
                               " is not set; the Python environment's packages read it: set it "
                               "to " +
                               path.u8string() + " before the process starts");
        } else if (!same_path(fs::u8path(*value), path)) {
            warnings.push_back("the environment variable " + name + " is '" + *value +
                               "', not the Python environment's " + path.u8string() +
                               ": set it to that path before the process starts");
        }
    }
    const std::optional<std::string> seed = environment_variable("PYTHONHASHSEED");
    const std::string why = "so that the Python processes that this one starts with its "
                            "environment and without -E or -I (subprocess with sys.executable, "
                            "for example) use the hash seed 0, as its interpreter does";
    if (!seed) {
        warnings.push_back(
            "the environment variable PYTHONHASHSEED is not set: set it to 0 before the process "
            "starts, " +
            why);
    } else if (*seed != "0") {
        warnings.push_back("the environment variable PYTHONHASHSEED is '" + *seed +
                           "', not 0: set it to 0 before the process starts, " + why);
    }
    return warnings;
}

std::optional<std::string> environment_variable(const std::string& name) {
#ifdef _WIN32
    const std::wstring wide_name = fs::u8path(name).wstring();
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    if (written == 0 || written >= size) {
        return written == 0 ? std::optional<std::string>(std::string()) : std::nullopt;
    }
    value.resize(written);
    return fs::path(value).u8string();
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool same_path(const fs::path& a, const fs::path& b) { return files::same_path(a, b); }

} // namespace praenora::detail::python
