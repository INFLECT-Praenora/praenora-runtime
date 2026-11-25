// What the runtime's own programs, the praenora tool (src/cli) and the module host
// praenora-module-host (src/module_host), do first thing in main(), before any thread starts: when
// their tree has the provider python, they set the variables of the process's environment that
// the Python environment's packages and child processes read (docs/specs/python-service.md, the
// process's environment). No provider and no Python core sets a variable: they run where other
// threads may read the environment.
//
// The tree has the provider python when the provider's environment file,
// <libdir>/praenora/providers/python/python-environment.json, exists. A program finds an
// installation's at the path relative to its own directory that it was built with (the compile
// definition PRAENORA_PYTHON_ENVIRONMENT_FILE, from the installation's GNUInstallDirs directories:
// ../lib/... from bin for the tool, ../../lib/... from libexec/praenora for the module host, with
// lib64 or another library directory in its place), and otherwise a build tree's, below lib/: the
// tool, in <tree>/bin, at <program dir>/../lib/praenora/providers/python/, the module host, in
// <tree>/libexec/praenora, at <program dir>/../../lib/praenora/providers/python/. The file names
// the environment's directory ({"environment": <dir>, ...}, relative to the file's directory or
// absolute), whose manifest praenora-runtime-pyenv.json lists in environment.paths the variables
// that name directories of the environment. Each of them but PYTHONHOME (the Python core gives the
// interpreter its home itself) is set to its absolute path when it is unset, and PYTHONHASHSEED to
// 0 when it is unset: a value set before is the caller's and stays. The code names no variable of
// the manifest.
//
// Header-only and internal; the module host, which links neither the shared runtime nor
// praenora_frontend, includes it by its directory. It uses only the standard library and
// nlohmann/json, and reads the environment's files with the helpers that the Python core and its
// loader read them with (src/providers/python/common/environment_files.hpp, header-only too).
#ifndef PRAENORA_FRONTEND_PYTHON_ENVIRONMENT_HPP
#define PRAENORA_FRONTEND_PYTHON_ENVIRONMENT_HPP

#include "../providers/python/common/environment_files.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdlib.h>
#else
#include <cstdlib>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace praenora::frontend {

// Where a program of the runtime lies in its tree.
enum class ProgramDirectory {
    bin,     // <tree>/bin: the praenora tool
    libexec, // <tree>/libexec/praenora: the module host praenora-module-host
};

namespace python_environment_detail {

inline std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A name a variable can have: not empty, and without '=' or NUL.
inline bool valid_name(const std::string& name) {
    return !name.empty() && name.find('=') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

// Sets the variable `name` to `value` (UTF-8) unless it is set already, empty or not.
inline void set_if_unset(const std::string& name, const std::string& value) {
    if (!valid_name(name) || value.find('\0') != std::string::npos) {
        return;
    }
#ifdef _WIN32
    // GetEnvironmentVariableW is where the runtime and the Python core read the environment;
    // _wputenv_s changes the C runtime's copy too, which Python's os.environ and C code read, and
    // the process's environment block, which child processes inherit.
    const std::wstring wide_name = std::filesystem::u8path(name).wstring();
    const std::wstring wide_value = std::filesystem::u8path(value).wstring();
    SetLastError(ERROR_SUCCESS);
    if (GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0) != 0 ||
        GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        return;
    }
    if (_wputenv_s(wide_name.c_str(), wide_value.c_str()) == 0) {
        SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str());
    }
#else
    static_cast<void>(setenv(name.c_str(), value.c_str(), 0));
#endif
}

} // namespace python_environment_detail

// The environment file of the provider python in the tree of the program at `program_path`, lying
// in `directory` of its tree: the installation's, at `installed` (UTF-8, relative to the program's
// directory, '/' separating its parts) when that is given and names a regular file, and otherwise
// the build tree's, below lib/, whether or not it exists.
inline std::filesystem::path python_environment_file(const std::filesystem::path& program_path,
                                                     ProgramDirectory directory,
                                                     const std::string& installed = std::string()) {
    namespace fs = std::filesystem;
    if (!installed.empty()) {
        const fs::path file =
            (program_path.parent_path() / fs::u8path(installed)).lexically_normal();
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) {
            return file;
        }
    }
    fs::path tree = program_path.parent_path() / "..";
    if (directory == ProgramDirectory::libexec) {
        tree /= "..";
    }
    return (tree / "lib" / "praenora" / "providers" / "python" / "python-environment.json")
        .lexically_normal();
}

// Sets the Python environment's variables, as the header says, for the program at `program_path`
// (absolute) lying in `directory` of its tree, which finds an installation's environment file at
// `installed` (python_environment_file()). Does nothing when the tree has no environment file;
// with one, PYTHONHASHSEED is set, and the paths too when the file and the environment's manifest
// can be read. Never throws: a file that cannot be read is ignored, since the provider's start
// reports it. Call it before any thread starts.
inline void apply_python_environment(const std::filesystem::path& program_path,
                                     ProgramDirectory directory,
                                     const std::string& installed = std::string()) noexcept {
    namespace fs = std::filesystem;
    using nlohmann::json;
    using python_environment_detail::read_text;
    using python_environment_detail::set_if_unset;
    try {
        if (program_path.empty()) {
            return;
        }
        const fs::path file = python_environment_file(program_path, directory, installed);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            return;
        }
        set_if_unset("PYTHONHASHSEED", "0");

        const json environment_file = json::parse(read_text(file), nullptr, false);
        if (!environment_file.is_object() || !environment_file.contains("environment") ||
            !environment_file.at("environment").is_string()) {
            return;
        }
        const std::string named = environment_file.at("environment").get<std::string>();
        if (named.empty()) {
            return;
        }
        fs::path environment = detail::python::files::resolved(file.parent_path(), named);
        environment = fs::absolute(environment, ec).lexically_normal();
        if (ec) {
            return;
        }
        const json manifest =
            json::parse(read_text(environment / "praenora-runtime-pyenv.json"), nullptr, false);
        if (!manifest.is_object() || !manifest.contains("environment")) {
            return;
        }
        const json& section = manifest.at("environment");
        if (!section.is_object() || !section.contains("paths") ||
            !section.at("paths").is_object()) {
            return;
        }
        for (const auto& [name, value] : section.at("paths").items()) {
            if (name == "PYTHONHOME" || !value.is_string()) {
                continue;
            }
            const fs::path path =
                detail::python::files::resolved(environment, value.get<std::string>());
            set_if_unset(name, path.u8string());
        }
    } catch (...) {
        // Ignored: the provider's start reports what is wrong.
    }
}

// The absolute path of the running program's executable file, or an empty path when the system
// cannot tell. On macOS its symbolic links are resolved; Linux reports the file itself
// (/proc/self/exe). Never throws.
inline std::filesystem::path current_program_path() noexcept {
    namespace fs = std::filesystem;
    try {
#if defined(_WIN32)
        return detail::python::files::module_file(nullptr);
#elif defined(__APPLE__)
        std::uint32_t size = 0;
        static_cast<void>(_NSGetExecutablePath(nullptr, &size));
        std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return fs::path();
        }
        std::error_code ec;
        const fs::path path = fs::weakly_canonical(fs::path(buffer.data()), ec);
        return ec ? fs::path() : path;
#elif defined(__linux__)
        std::error_code ec;
        const fs::path path = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::path() : path;
#else
        return fs::path();
#endif
    } catch (...) {
        return fs::path();
    }
}

} // namespace praenora::frontend

#endif // PRAENORA_FRONTEND_PYTHON_ENVIRONMENT_HPP
