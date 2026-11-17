// loader/shared_library.hpp on Windows: LoadLibraryExW(), GetProcAddress() and FreeLibrary().
//
// Paths reach this file as std::filesystem::path, whose native form on Windows is UTF-16: the
// UTF-8 text of manifests and the API was converted (MultiByteToWideChar semantics) by
// path_from_utf8() in core/paths. Messages go back out as UTF-8.
#include "loader/shared_library.hpp"

#include "core/paths.hpp"

#include <praenora/runtime/error.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

std::string utf8_from_wide(const wchar_t* data, std::size_t length) {
    if (length == 0) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(length), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(length), text.data(), size, nullptr,
                        nullptr);
    return text;
}

// "error <code>: <the system's text for it>", in UTF-8.
std::string system_message(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (length != 0 && buffer != nullptr) {
        text = utf8_from_wide(buffer, length);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return "error " + std::to_string(error) + (text.empty() ? std::string() : ": " + text);
}

std::filesystem::path with_backslashes(std::filesystem::path path) {
    path.make_preferred();
    return path;
}

// The directories added with AddDllDirectory() for one load. They are removed again when the
// object is destroyed, so they never stay in the process's search list.
class DependencyDirectories {
public:
    DependencyDirectories() = default;
    DependencyDirectories(const DependencyDirectories&) = delete;
    DependencyDirectories& operator=(const DependencyDirectories&) = delete;
    ~DependencyDirectories() {
        for (const DLL_DIRECTORY_COOKIE cookie : cookies_) {
            RemoveDllDirectory(cookie);
        }
    }

    void add(const std::filesystem::path& directory, const std::filesystem::path& library,
             std::string_view what) {
        const std::filesystem::path native = with_backslashes(directory);
        const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(native.c_str());
        if (cookie == nullptr) {
            const DWORD error = GetLastError();
            throw Error(ErrorCode::LIBRARY_LOAD_FAILED,
                        "cannot load " + std::string(what) + " '" + path_to_utf8(library) +
                            "': cannot add its dependency directory '" + path_to_utf8(directory) +
                            "': " + system_message(error));
        }
        cookies_.push_back(cookie);
    }

private:
    std::vector<DLL_DIRECTORY_COOKIE> cookies_;
};

} // namespace

SharedLibrary::SharedLibrary(void* handle, std::filesystem::path path) noexcept
    : handle_(handle), path_(std::move(path)) {}

SharedLibrary SharedLibrary::open(const std::filesystem::path& absolute_path,
                                  const std::vector<std::filesystem::path>& dependency_dirs,
                                  std::string_view what) {
    DependencyDirectories directories;
    for (const auto& directory : dependency_dirs) {
        directories.add(directory, absolute_path, what);
    }
    // A library that cannot be loaded must produce an error code, never a dialog box.
    DWORD previous_mode = 0;
    const BOOL mode_set =
        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a fully qualified path with backslashes.
    const std::filesystem::path native = with_backslashes(absolute_path);
    const HMODULE module =
        LoadLibraryExW(native.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                           LOAD_LIBRARY_SEARCH_USER_DIRS);
    const DWORD error = module == nullptr ? GetLastError() : ERROR_SUCCESS;
    if (mode_set) {
        SetThreadErrorMode(previous_mode, nullptr);
    }
    if (module == nullptr) {
        throw Error(ErrorCode::LIBRARY_LOAD_FAILED, "cannot load " + std::string(what) + " '" +
                                                        path_to_utf8(absolute_path) +
                                                        "': " + system_message(error));
    }
    return SharedLibrary(module, absolute_path);
}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)),
      pinned_(other.pinned_.load()) {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
        pinned_.store(other.pinned_.load());
    }
    return *this;
}

SharedLibrary::~SharedLibrary() { close(); }

void SharedLibrary::close() noexcept {
    if (handle_ != nullptr && !pinned_.load()) {
        FreeLibrary(static_cast<HMODULE>(handle_));
    }
    // A pinned handle is dropped without FreeLibrary(), so the library stays loaded for good.
    handle_ = nullptr;
}

void* SharedLibrary::symbol(const char* name) const noexcept {
    if (handle_ == nullptr || name == nullptr) {
        return nullptr;
    }
    const FARPROC procedure = GetProcAddress(static_cast<HMODULE>(handle_), name);
    void* address = nullptr;
    static_assert(sizeof(address) == sizeof(procedure), "code and data pointers have one size");
    std::memcpy(&address, &procedure, sizeof address);
    return address;
}

void SharedLibrary::pin() noexcept { pinned_.store(true); }

bool SharedLibrary::pinned() const noexcept { return pinned_.load(); }

const std::filesystem::path& SharedLibrary::path() const noexcept { return path_; }

} // namespace praenora::detail
