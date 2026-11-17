// Small operating-system services the runtime needs besides loading libraries
// (loader/shared_library.hpp). Implemented in platform/posix/ and platform/windows/.
#ifndef PRAENORA_RUNTIME_PLATFORM_PLATFORM_HPP
#define PRAENORA_RUNTIME_PLATFORM_PLATFORM_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace praenora::detail {

// The value of the environment variable `name` as UTF-8, or nullopt when it is not set. (Windows
// reads the wide environment, so values outside the ANSI code page survive.)
std::optional<std::string> environment_variable(const char* name);

// The user's home directory: on POSIX systems HOME, or, when it is unset or empty, the home
// directory of the user's entry in the password database (getpwuid_r()); on Windows USERPROFILE.
// nullopt when there is none. The value is returned as it is found, so it may be relative or a
// root directory (PathAnchors skips such a home).
std::optional<std::filesystem::path> home_directory();

// The absolute path of the binary that holds the runtime's code: the shared library
// praenora_runtime, or the executable when the runtime's objects are linked into it. Found with
// dladdr() or GetModuleHandleExW(); nullopt when the platform cannot tell.
std::optional<std::filesystem::path> runtime_binary_path();

// Names the calling thread for debuggers and profilers, where the platform allows it; Linux keeps
// only the first 15 bytes. Best effort: failures are ignored.
void set_current_thread_name(const std::string& name) noexcept;

// The identifier of the current process (getpid() or GetCurrentProcessId()).
std::int64_t current_process_id() noexcept;

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_PLATFORM_PLATFORM_HPP
