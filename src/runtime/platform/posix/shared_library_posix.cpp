// loader/shared_library.hpp on POSIX systems: dlopen(), dlsym() and dlclose().
#include "loader/shared_library.hpp"

#include "core/paths.hpp"

#include <praenora/runtime/error.hpp>

#include <dlfcn.h>

#include <string>
#include <utility>

namespace praenora::detail {

SharedLibrary::SharedLibrary(void* handle, std::filesystem::path path) noexcept
    : handle_(handle), path_(std::move(path)) {}

SharedLibrary SharedLibrary::open(const std::filesystem::path& absolute_path,
                                  const std::vector<std::filesystem::path>& dependency_dirs) {
    static_cast<void>(dependency_dirs); // run paths ($ORIGIN, @loader_path) do this on POSIX
    // dlerror() reports the last error of the calling thread; clear any earlier one first.
    static_cast<void>(dlerror());
    void* handle = dlopen(absolute_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* reason = dlerror();
        throw Error(ErrorCode::LIBRARY_LOAD_FAILED,
                    "cannot load module library '" + path_to_utf8(absolute_path) + "': " +
                        (reason != nullptr ? std::string(reason) : std::string("unknown error")));
    }
    return SharedLibrary(handle, absolute_path);
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
        static_cast<void>(dlclose(handle_));
    }
    // A pinned handle is dropped without dlclose(), so the library stays loaded for good.
    handle_ = nullptr;
}

void* SharedLibrary::symbol(const char* name) const noexcept {
    if (handle_ == nullptr || name == nullptr) {
        return nullptr;
    }
    return dlsym(handle_, name);
}

void SharedLibrary::pin() noexcept { pinned_.store(true); }

bool SharedLibrary::pinned() const noexcept { return pinned_.load(); }

const std::filesystem::path& SharedLibrary::path() const noexcept { return path_; }

} // namespace praenora::detail
