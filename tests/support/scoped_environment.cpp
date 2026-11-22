#include "scoped_environment.hpp"

#include "test_support.hpp"

#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <vector>
#else
#include <cstdlib>
#endif

namespace praenora::test {

namespace {

// The variable's value as UTF-8, or nullopt when it is not set.
std::optional<std::string> get(const std::string& name) {
#ifdef _WIN32
    const std::wstring wide_name = utf8_path(name).wstring();
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
            return std::filesystem::path(std::wstring(buffer.data(), length)).u8string();
        }
        buffer.resize(static_cast<std::size_t>(length));
    }
#else
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

void put(const std::string& name, const std::optional<std::string>& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(utf8_path(name).c_str(), value ? utf8_path(*value).c_str() : nullptr);
#else
    if (value) {
        setenv(name.c_str(), value->c_str(), 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

} // namespace

ScopedEnvironment::ScopedEnvironment(std::string name, const std::optional<std::string>& value)
    : name_(std::move(name)), previous_(get(name_)) {
    put(name_, value);
}

ScopedEnvironment::~ScopedEnvironment() { put(name_, previous_); }

} // namespace praenora::test
