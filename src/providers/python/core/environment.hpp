// The Python environment as the core reads it from its manifest (praenora-runtime-pyenv.json),
// what describe() reports of it, and the checks of the process's environment variables that the
// core warns about (docs/specs/python-service.md). No Python.
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_ENVIRONMENT_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_ENVIRONMENT_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace praenora::detail::python {

struct Environment {
    std::filesystem::path directory; // absolute, lexically normal
    std::filesystem::path manifest;  // directory / praenora-runtime-pyenv.json, a regular file
    std::string name, version, package, lock_sha256;
    std::string python_implementation, python_version, python_abi;
    std::filesystem::path home, executable; // absolute
    // environment.paths: each variable the manifest names, with the absolute path it should
    // hold, sorted by name.
    std::vector<std::pair<std::string, std::filesystem::path>> paths;
};

// Reads the manifest of the environment in `directory` (absolute). Throws Failure: IO_ERROR when
// it cannot be read, DATA_INVALID when it is not JSON or lacks a value the core needs.
Environment read_environment(const std::filesystem::path& directory);

// describe()'s document: canonical JSON {"environment": {"lock_sha256", "name", "package",
// "version"}, "python": {"abi", "implementation", "version"}}.
std::string describe_environment(const Environment& environment);

// One warning for each variable of environment.paths but PYTHONHOME that is unset or names
// another path, and one for PYTHONHASHSEED when it is unset or not 0.
std::vector<std::string> variable_warnings(const Environment& environment);

// The value of a variable of the process's environment, in UTF-8; nothing when it is unset.
std::optional<std::string> environment_variable(const std::string& name);

// Whether two paths name the same file or directory: equal when normalized, or the same entry
// (files::same_path() of common/environment_files.hpp, which the loader uses too).
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_ENVIRONMENT_HPP
