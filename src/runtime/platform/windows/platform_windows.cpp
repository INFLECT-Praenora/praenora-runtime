// platform/platform.hpp on Windows. Text crosses the Win32 API as UTF-16 and is converted from and
// to UTF-8 here.
#include "platform/platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <process.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

// An object inside the runtime's binary, whose address GetModuleHandleExW() maps back to it.
const char kRuntimeMarker = 0;

// The longest path GetModuleFileNameW() can report, in UTF-16 code units.
constexpr std::size_t kMaxPathUnits = 32768;

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string narrow(const wchar_t* data, std::size_t length) {
    if (length == 0) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(length), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(length), utf8.data(), size, nullptr,
                        nullptr);
    return utf8;
}

// SetThreadDescription exists from Windows 10 version 1607 on; it is looked up at run time so the
// runtime still loads on earlier versions.
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

struct StackTask {
    const std::function<void()>* task = nullptr;
    std::exception_ptr error;
};

DWORD WINAPI run_stack_task(LPVOID argument) {
    auto* task = static_cast<StackTask*>(argument);
    try {
        (*task->task)();
    } catch (...) {
        task->error = std::current_exception();
    }
    return 0;
}

} // namespace

void run_on_thread_with_stack(std::size_t stack_bytes, const std::function<void()>& task) {
    StackTask state;
    state.task = &task;
    // The size is a reservation: the stack is committed as it is used.
    HANDLE thread = CreateThread(nullptr, stack_bytes, run_stack_task, &state,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) {
        throw ThreadStartError("cannot start a thread with a stack of " +
                               std::to_string(stack_bytes) + " bytes (error " +
                               std::to_string(GetLastError()) + ")");
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (state.error) {
        std::rethrow_exception(state.error);
    }
}

std::optional<std::string> environment_variable(const char* name) {
    const std::wstring wide_name = widen(name);
    std::vector<wchar_t> buffer(256);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string(); // set, but empty
        }
        if (length < buffer.size()) {
            return narrow(buffer.data(), length);
        }
        // Too small: `length` is the size needed, terminating NUL included.
        buffer.resize(static_cast<std::size_t>(length));
    }
}

std::optional<std::filesystem::path> home_directory() {
    const auto profile = environment_variable("USERPROFILE");
    if (!profile || profile->empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(widen(*profile));
}

std::optional<std::filesystem::path> runtime_binary_path() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&kRuntimeMarker), &module)) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).lexically_normal();
        }
        // The name was truncated: retry with a larger buffer.
        if (buffer.size() >= kMaxPathUnits) {
            return std::nullopt;
        }
        buffer.resize(buffer.size() * 2);
    }
}

void set_current_thread_name(const std::string& name) noexcept {
    try {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel == nullptr) {
            return;
        }
        const FARPROC procedure = GetProcAddress(kernel, "SetThreadDescription");
        if (procedure == nullptr) {
            return;
        }
        SetThreadDescriptionFn set_description = nullptr;
        static_assert(sizeof(set_description) == sizeof(procedure),
                      "function pointers have one size");
        std::memcpy(&set_description, &procedure, sizeof set_description);
        const std::wstring wide = widen(name);
        static_cast<void>(set_description(GetCurrentThread(), wide.c_str()));
    } catch (...) {
        // Naming is best effort.
    }
}

std::int64_t current_process_id() noexcept {
    return static_cast<std::int64_t>(GetCurrentProcessId());
}

std::size_t module_thread_stack_size() noexcept { return std::size_t{8} << 20; }

namespace {

// The start routine of a ModuleThread: runs and frees the body it is given.
unsigned __stdcall run_module_thread(void* raw) noexcept {
    const std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(raw));
    (*body)();
    return 0;
}

} // namespace

struct ModuleThread::Native {
    HANDLE handle = nullptr;
    unsigned id = 0;
};

ModuleThread::ModuleThread() noexcept = default;

ModuleThread::ModuleThread(std::function<void()> body) {
    auto start = std::make_unique<std::function<void()>>(std::move(body));
    auto native = std::make_unique<Native>();
    // The size is the stack's reservation; the system commits its pages as they are used.
    const std::uintptr_t handle = _beginthreadex(
        nullptr, static_cast<unsigned>(module_thread_stack_size()), &run_module_thread, start.get(),
        STACK_SIZE_PARAM_IS_A_RESERVATION, &native->id);
    if (handle == 0) {
        throw std::system_error(errno, std::generic_category(), "_beginthreadex");
    }
    static_cast<void>(start.release()); // the thread frees it
    native->handle = reinterpret_cast<HANDLE>(handle);
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
    if (native_->id == GetCurrentThreadId()) {
        throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
    }
    if (WaitForSingleObject(native_->handle, INFINITE) != WAIT_OBJECT_0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "WaitForSingleObject");
    }
    CloseHandle(native_->handle);
    native_.reset();
}

void ModuleThread::detach() noexcept {
    if (joinable()) {
        CloseHandle(native_->handle);
        native_.reset();
    }
}

} // namespace praenora::detail
