// A module or provider library loaded into the process: dlopen() on POSIX, LoadLibraryExW() on
// Windows.
// Implemented in platform/posix/shared_library_posix.cpp and
// platform/windows/shared_library_windows.cpp.
#ifndef PRAENORA_RUNTIME_LOADER_SHARED_LIBRARY_HPP
#define PRAENORA_RUNTIME_LOADER_SHARED_LIBRARY_HPP

#include <atomic>
#include <filesystem>
#include <string_view>
#include <vector>

namespace praenora::detail {

class SharedLibrary {
public:
    // Loads the library at `absolute_path`. Throws praenora::Error(LIBRARY_LOAD_FAILED) with the
    // operating system's explanation (dlerror() or FormatMessageW() text) when it cannot; the
    // message starts "cannot load <what> '<path>': ", where `what` names the library (for example
    // "module library").
    // - POSIX: dlopen(RTLD_NOW | RTLD_LOCAL), so unresolved symbols fail here and modules cannot
    //   interpose on each other's symbols. `dependency_dirs` are ignored: POSIX modules find their
    //   own dependencies through $ORIGIN or @loader_path run paths.
    // - Windows: each of `dependency_dirs` is added with AddDllDirectory() for the duration of the
    //   load, then LoadLibraryExW() searches the library's own directory, the application
    //   directory, System32 and those directories, never the current directory or PATH.
    static SharedLibrary open(const std::filesystem::path& absolute_path,
                              const std::vector<std::filesystem::path>& dependency_dirs,
                              std::string_view what = "module library");

    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    // Unloads the library unless it is pinned.
    ~SharedLibrary();

    // The address of the exported symbol `name`, or nullptr.
    void* symbol(const char* name) const noexcept;

    // Keeps the library loaded for the rest of the process, whatever happens to this object: for a
    // library whose code may still be running on a detached thread. Thread-safe.
    void pin() noexcept;
    bool pinned() const noexcept;

    const std::filesystem::path& path() const noexcept;

    // The loaded image: the same for every open of one library file in the process, however the
    // file is named (a process holds one copy of each library), and another for a copy of the
    // file at another path. NULL once moved from. Identifies the library's gate
    // (loader/library_gate.hpp).
    const void* image() const noexcept;

private:
    SharedLibrary(void* handle, std::filesystem::path path) noexcept;
    void close() noexcept;

    void* handle_ = nullptr; // the dlopen() handle or the HMODULE
    std::filesystem::path path_;
    std::atomic<bool> pinned_{false};
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_SHARED_LIBRARY_HPP
