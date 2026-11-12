// platform/platform.hpp on POSIX systems (Linux, macOS).
#include "platform/platform.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace praenora::detail {

namespace {

// An object inside the runtime's binary, whose address dladdr() maps back to that binary.
const char kRuntimeMarker = 0;

} // namespace

std::optional<std::string> environment_variable(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<std::filesystem::path> runtime_binary_path() {
    Dl_info info{};
    if (dladdr(&kRuntimeMarker, &info) == 0 || info.dli_fname == nullptr ||
        info.dli_fname[0] == '\0') {
        return std::nullopt;
    }
    std::error_code ec;
    const auto path = std::filesystem::absolute(std::filesystem::path(info.dli_fname), ec);
    if (ec) {
        return std::nullopt;
    }
    return path.lexically_normal();
}

void set_current_thread_name(const std::string& name) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    // macOS takes up to 63 bytes, Linux up to 15, each plus the terminating NUL.
#if defined(__APPLE__)
    constexpr std::size_t kMaxBytes = 63;
#else
    constexpr std::size_t kMaxBytes = 15;
#endif
    char buffer[kMaxBytes + 1];
    const std::size_t size = std::min(name.size(), kMaxBytes);
    std::memcpy(buffer, name.data(), size);
    buffer[size] = '\0';
#if defined(__APPLE__)
    // macOS can name only the calling thread.
    static_cast<void>(pthread_setname_np(buffer));
#else
    static_cast<void>(pthread_setname_np(pthread_self(), buffer));
#endif
#else
    static_cast<void>(name);
#endif
}

std::int64_t current_process_id() noexcept { return static_cast<std::int64_t>(getpid()); }

} // namespace praenora::detail
