// Small operating-system services the runtime needs besides loading libraries
// (loader/shared_library.hpp). Implemented in platform/posix/ and platform/windows/.
#ifndef PRAENORA_RUNTIME_PLATFORM_PLATFORM_HPP
#define PRAENORA_RUNTIME_PLATFORM_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

// The stack of the threads that run a module's code (its worker threads, and the threads of a
// module host): a program's main thread's, which native code and Python expect (a thread's default
// stack is 512 KiB on macOS, where Python code that recurses far below its recursion limit
// overflows it). On POSIX systems the soft limit of RLIMIT_STACK, at least 1 MiB and at most
// 1 GiB, in whole pages, or 8 MiB when the limit is unlimited or cannot be read; on Windows 8 MiB,
// reserved (address space until it is used).
std::size_t module_thread_stack_size() noexcept;

// A thread with a stack of module_thread_stack_size() that runs `body`, joined or detached as a
// std::thread is: destroying one that is still joinable ends the process, and so does an
// exception that escapes `body`.
class ModuleThread {
public:
    ModuleThread() noexcept;
    // Starts the thread. Throws std::system_error when the system starts no thread.
    explicit ModuleThread(std::function<void()> body);
    ModuleThread(ModuleThread&& other) noexcept;
    ModuleThread& operator=(ModuleThread&& other) noexcept;
    ~ModuleThread();
    ModuleThread(const ModuleThread&) = delete;
    ModuleThread& operator=(const ModuleThread&) = delete;

    bool joinable() const noexcept;
    // Waits for the thread to end. Throws std::system_error (resource_deadlock_would_occur) on
    // the thread itself.
    void join();
    // Lets the thread run on its own.
    void detach() noexcept;

private:
    struct Native;
    std::unique_ptr<Native> native_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_PLATFORM_PLATFORM_HPP
