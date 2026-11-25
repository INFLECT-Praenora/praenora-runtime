// platform/platform.hpp on POSIX systems (Linux, macOS).
#include "platform/platform.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

// An object inside the runtime's binary, whose address dladdr() maps back to that binary.
const char kRuntimeMarker = 0;

// The largest buffer home_directory() offers getpwuid_r() for one password database entry.
constexpr std::size_t kMaxPasswordEntryBytes = std::size_t(1) << 20;

// The start routine of a ModuleThread: runs and frees the body it is given.
void* run_module_thread(void* raw) noexcept {
    const std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(raw));
    (*body)();
    return nullptr;
}

} // namespace

std::optional<std::string> environment_variable(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<std::filesystem::path> home_directory() {
    if (const auto home = environment_variable("HOME"); home && !home->empty()) {
        return std::filesystem::path(*home);
    }
    // The user's entry in the password database. The buffer grows until the entry fits.
    const long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggested > 0 ? static_cast<std::size_t>(suggested) : 1024);
    passwd entry{};
    passwd* found = nullptr;
    int result = 0;
    while ((result = getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found)) ==
               ERANGE &&
           buffer.size() < kMaxPasswordEntryBytes) {
        buffer.resize(buffer.size() * 2);
    }
    if (result != 0 || found == nullptr || found->pw_dir == nullptr || found->pw_dir[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(found->pw_dir);
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

std::size_t module_thread_stack_size() noexcept {
    constexpr rlim_t kFallback = rlim_t{8} << 20;
    constexpr rlim_t kLeast = rlim_t{1} << 20;
    constexpr rlim_t kMost = rlim_t{1} << 30;
    rlim_t size = kFallback;
    rlimit limit{};
    if (::getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
        size = std::clamp(limit.rlim_cur, kLeast, kMost);
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page > 0) {
        const auto unit = static_cast<rlim_t>(page);
        size = (size + unit - 1) / unit * unit;
    }
    return static_cast<std::size_t>(size);
}

struct ModuleThread::Native {
    pthread_t thread{};
};

ModuleThread::ModuleThread() noexcept = default;

ModuleThread::ModuleThread(std::function<void()> body) {
    auto start = std::make_unique<std::function<void()>>(std::move(body));
    auto native = std::make_unique<Native>();
    pthread_attr_t attributes;
    int result = ::pthread_attr_init(&attributes);
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), "pthread_attr_init");
    }
    // A size the system refuses leaves the default stack.
    static_cast<void>(::pthread_attr_setstacksize(&attributes, module_thread_stack_size()));
    result = ::pthread_create(&native->thread, &attributes, &run_module_thread, start.get());
    static_cast<void>(::pthread_attr_destroy(&attributes));
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), "pthread_create");
    }
    static_cast<void>(start.release()); // the thread frees it
    native_ = std::move(native);
}

ModuleThread::ModuleThread(ModuleThread&& other) noexcept = default;

ModuleThread& ModuleThread::operator=(ModuleThread&& other) noexcept {
    if (this != &other) {
        if (joinable()) {
            std::terminate();
        }
        native_ = std::move(other.native_);
    }
    return *this;
}

ModuleThread::~ModuleThread() {
    if (joinable()) {
        std::terminate();
    }
}

bool ModuleThread::joinable() const noexcept { return native_ != nullptr; }

void ModuleThread::join() {
    if (!joinable()) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument));
    }
    if (::pthread_equal(native_->thread, ::pthread_self()) != 0) {
        throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
    }
    const int result = ::pthread_join(native_->thread, nullptr);
    if (result != 0) {
        throw std::system_error(result, std::generic_category(), "pthread_join");
    }
    native_.reset();
}

void ModuleThread::detach() noexcept {
    if (joinable()) {
        static_cast<void>(::pthread_detach(native_->thread));
        native_.reset();
    }
}

} // namespace praenora::detail
