// What the readers of the Python environment's files share, so that they read them alike: the
// loader of the Python core (common/loader.cpp), the core itself (core/environment.cpp) and the
// runtime's own programs (src/frontend/python_environment.hpp). Header-only and internal: no
// Python and no part of the runtime, only the standard library and nlohmann/json.
#ifndef PRAENORA_PROVIDERS_PYTHON_COMMON_ENVIRONMENT_FILES_HPP
#define PRAENORA_PROVIDERS_PYTHON_COMMON_ENVIRONMENT_FILES_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace praenora::detail::python::files {

// Whether two paths name the same file or directory: equal when normalized, or the same entry.
inline bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.lexically_normal() == b.lexically_normal()) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec) && !ec;
}

// The string at `keys` of `document` (for example {"python", "home"}) when it is a string that is
// not empty, nullopt otherwise; `name` gets the keys joined by dots up to the first one that is
// missing ("python.home", or "python" when there is no python), for the message of a reader that
// refuses the document.
inline std::optional<std::string> string_at(const nlohmann::json& document,
                                            std::initializer_list<const char*> keys,
                                            std::string& name) {
    const nlohmann::json* value = &document;
    name.clear();
    for (const char* key : keys) {
        name += name.empty() ? key : std::string(".") + key;
        value = value->is_object() && value->contains(key) ? &(*value)[key] : nullptr;
        if (value == nullptr) {
            break;
        }
    }
    if (value == nullptr || !value->is_string() || value->get<std::string>().empty()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

// `text` (UTF-8) as a path below `directory` unless it is absolute, lexically normal: how a file
// of the environment names another (the environment file its environment, the manifest its
// interpreter's home).
inline std::filesystem::path resolved(const std::filesystem::path& directory,
                                      const std::string& text) {
    std::filesystem::path path = std::filesystem::u8path(text);
    if (path.is_relative()) {
        path = directory / path;
    }
    return path.lexically_normal();
}

#ifdef _WIN32
// The file of the loaded module `module` (nullptr: the process's executable), lexically normal,
// read with GetModuleFileNameW into a buffer that grows up to the longest path; an empty path when
// the system cannot tell.
inline std::filesystem::path module_file(HMODULE module) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::filesystem::path();
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).lexically_normal();
        }
        if (buffer.size() >= 32768) {
            return std::filesystem::path();
        }
        buffer.resize(buffer.size() * 2);
    }
}
#endif

} // namespace praenora::detail::python::files

#endif // PRAENORA_PROVIDERS_PYTHON_COMMON_ENVIRONMENT_FILES_HPP
