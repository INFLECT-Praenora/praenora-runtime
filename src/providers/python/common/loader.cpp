// The loader of the Python core (loader.hpp). The build gives it the environment it was built
// against and the core's build identity as compile definitions.
#include "loader.hpp"

#include "environment_files.hpp"
#include "service_glue.hpp"

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>

#include <cwctype>
#else
#include <dlfcn.h>
#endif

#ifndef PRAENORA_PYTHON_ENVIRONMENT_PACKAGE
#error "PRAENORA_PYTHON_ENVIRONMENT_PACKAGE is defined by the build of the providers"
#endif
#ifndef PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256
#error "PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256 is defined by the build of the providers"
#endif
#ifndef PRAENORA_PYTHON_CORE_BUILD_ID
#error "PRAENORA_PYTHON_CORE_BUILD_ID is defined by the build of the providers"
#endif

namespace praenora::detail::python {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kEnvironmentFile = "python-environment.json";
constexpr const char* kEnvironmentManifest = "praenora-runtime-pyenv.json";
constexpr const char* kModuleHostAdvice =
    "a process holds one Python interpreter: run the step in a module host (isolation mode "
    "process)";

// A failure of a step, as the error document says it.
struct LoadFailure {
    praenora_status code;
    std::string message;
    json details;
};

[[noreturn]] void fail(praenora_status code, std::string message, json details = json::object()) {
    throw LoadFailure{code, std::move(message), std::move(details)};
}

std::string utf8(const fs::path& path) { return path.u8string(); }
std::string quoted(const fs::path& path) { return "'" + utf8(path) + "'"; }

using files::resolved;
using files::same_path;
// Whether `path` exists (a failure to tell counts as no).
bool present(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

#ifdef _WIN32
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

// The path with backslashes, as LoadLibraryExW and GetModuleHandleExW need it.
fs::path native(fs::path path) {
    path.make_preferred();
    return path;
}

using files::module_file;
#endif

// An object in the provider library, whose address the system maps back to the library's file.
const char kMarker = 0;

// 1. The provider library's own file.
fs::path own_library() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&kMarker), &module)) {
        fail(PRAENORA_STATUS_INTERNAL,
             "cannot find the provider library's own file: " + system_message(GetLastError()));
    }
    const fs::path file = module_file(module);
    if (file.empty()) {
        fail(PRAENORA_STATUS_INTERNAL,
             "cannot find the provider library's own file: " + system_message(GetLastError()));
    }
    return file;
#else
    Dl_info info{};
    if (dladdr(&kMarker, &info) == 0 || info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
        fail(PRAENORA_STATUS_INTERNAL, "cannot find the provider library's own file");
    }
    std::error_code ec;
    const fs::path file = fs::absolute(fs::path(info.dli_fname), ec);
    return (ec ? fs::path(info.dli_fname) : file).lexically_normal();
#endif
}

// The text of `file`: NOT_FOUND when it does not exist, IO_ERROR when it cannot be read.
std::string read_file(const fs::path& file, const std::string& missing, json details) {
    if (!present(file)) {
        fail(PRAENORA_STATUS_NOT_FOUND, missing, std::move(details));
    }
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        fail(PRAENORA_STATUS_IO_ERROR, "cannot read " + quoted(file), std::move(details));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        fail(PRAENORA_STATUS_IO_ERROR, "cannot read " + quoted(file), std::move(details));
    }
    return text;
}

// The text of the string at `keys` in `document`, read from `file`: DATA_INVALID when it is not
// a string that is not empty.
std::string text_at(const json& document, std::initializer_list<const char*> keys,
                    const fs::path& file, const std::string& what) {
    std::string name;
    std::optional<std::string> text = files::string_at(document, keys, name);
    if (!text) {
        fail(PRAENORA_STATUS_DATA_INVALID, what + " " + quoted(file) + " has no " + name,
             json{{"file", utf8(file)}, {"key", name}});
    }
    return std::move(*text);
}

// What the environment file beside the provider library names, as absolute paths.
struct EnvironmentFile {
    fs::path file;        // the environment file
    fs::path environment; // the environment's directory
    fs::path core;        // the core library
};

EnvironmentFile read_environment_file(const fs::path& library) {
    EnvironmentFile names;
    const fs::path directory = library.parent_path();
    names.file = directory / kEnvironmentFile;
    const std::string text =
        read_file(names.file,
                  "the provider library " + quoted(library) + " has no environment file " +
                      quoted(names.file) +
                      ", which names the Python environment and the Python core it "
                      "starts",
                  json{{"file", utf8(names.file)}, {"library", utf8(library)}});
    const json document = json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             "the environment file " + quoted(names.file) + " is not a JSON object",
             json{{"file", utf8(names.file)}});
    }
    names.environment =
        resolved(directory, text_at(document, {"environment"}, names.file, "the environment file"));
    names.core =
        resolved(directory, text_at(document, {"core"}, names.file, "the environment file"));
    return names;
}

// What the environment's manifest says, and where its libpython is.
struct Environment {
    fs::path manifest;
    std::string package, lock_sha256;
    fs::path python_home; // absolute
    fs::path libpython;   // absolute: the shared library (on Windows the DLL) to load
};

Environment read_environment(const EnvironmentFile& names) {
    Environment environment;
    environment.manifest = names.environment / kEnvironmentManifest;
    const json details{{"file", utf8(names.file)},
                       {"environment", utf8(names.environment)},
                       {"manifest", utf8(environment.manifest)}};
    const std::string text = read_file(environment.manifest,
                                       "the environment file " + quoted(names.file) +
                                           " names the directory " + quoted(names.environment) +
                                           ", which holds no Python environment: it has no " +
                                           quoted(environment.manifest),
                                       details);
    const json manifest = json::parse(text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             "the manifest " + quoted(environment.manifest) +
                 " of the Python environment is not a JSON object",
             details);
    }
    const std::string what = "the Python environment's manifest";
    environment.package = text_at(manifest, {"package"}, environment.manifest, what);
    environment.lock_sha256 = text_at(manifest, {"lock", "sha256"}, environment.manifest, what);
    environment.python_home = resolved(
        names.environment, text_at(manifest, {"python", "home"}, environment.manifest, what));
    const fs::path library = resolved(
        names.environment, text_at(manifest, {"python", "library"}, environment.manifest, what));
#ifdef _WIN32
    // python.library is the import library (python/libs/python312.lib); the DLL of the same name
    // is in the Python directory.
    environment.libpython = environment.python_home / library.stem();
    environment.libpython += ".dll";
#else
    environment.libpython = library;
#endif
    return environment;
}

// 1. The environment must be the one the provider was built against, and the files it names must
// be there.
void check_environment(const EnvironmentFile& names, const Environment& environment) {
    const std::string package = PRAENORA_PYTHON_ENVIRONMENT_PACKAGE;
    const std::string lock = PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256;
    if (environment.package != package || environment.lock_sha256 != lock) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the Python environment " + quoted(names.environment) + " that " + quoted(names.file) +
                 " names is " + environment.package + " with the lock " + environment.lock_sha256 +
                 ", and the provider was built for " + package + " with the lock " + lock,
             json{{"file", utf8(names.file)},
                  {"environment", utf8(names.environment)},
                  {"package", environment.package},
                  {"lock_sha256", environment.lock_sha256},
                  {"expected_package", package},
                  {"expected_lock_sha256", lock}});
    }
    if (!present(names.core)) {
        fail(PRAENORA_STATUS_NOT_FOUND,
             "the environment file " + quoted(names.file) + " names the Python core " +
                 quoted(names.core) + ", which does not exist",
             json{{"file", utf8(names.file)}, {"core", utf8(names.core)}});
    }
    if (!present(environment.libpython)) {
        fail(PRAENORA_STATUS_NOT_FOUND,
             "the Python environment " + quoted(names.environment) + " lacks its library " +
                 quoted(environment.libpython),
             json{{"environment", utf8(names.environment)},
                  {"library", utf8(environment.libpython)}});
    }
}

// Whether the core library is in the process already (another provider loaded it).
bool core_loaded(const fs::path& core) {
#ifdef _WIN32
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, native(core).c_str(),
                              &module) != 0;
#else
    void* handle = dlopen(core.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle); // RTLD_NOLOAD counted a reference
    return true;
#endif
}

// 2. The file that holds another Python interpreter in the process, if one does.
std::optional<fs::path> another_interpreter(const Environment& environment) {
#ifdef _WIN32
    // Every loaded python3*.dll (python3.dll and python3<minor>.dll) that is not in the
    // environment's Python directory.
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) {
            break;
        }
    }
    if (snapshot == INVALID_HANDLE_VALUE) {
        fail(PRAENORA_STATUS_INTERNAL,
             "cannot list the modules of the process, to look for another Python interpreter: " +
                 system_message(GetLastError()));
    }
    std::optional<fs::path> found;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Module32FirstW(snapshot, &entry); more && !found;
         more = Module32NextW(snapshot, &entry)) {
        std::wstring name = entry.szModule;
        for (wchar_t& c : name) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        const bool python_dll = name.size() >= 11 && name.compare(0, 7, L"python3") == 0 &&
                                name.compare(name.size() - 4, 4, L".dll") == 0;
        if (python_dll) {
            const fs::path file = module_file(entry.hModule);
            if (!file.empty() && !same_path(file.parent_path(), environment.python_home)) {
                found = file;
            }
        }
    }
    CloseHandle(snapshot);
    return found;
#else
    // Py_IsInitialized in the process's global scope: an interpreter linked into the program (the
    // environment's own python3.12 links CPython statically) or loaded globally.
    void* symbol = dlsym(RTLD_DEFAULT, "Py_IsInitialized");
    if (symbol == nullptr) {
        return std::nullopt;
    }
    Dl_info info{};
    if (dladdr(symbol, &info) == 0 || info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
        // The program itself (glibc names the main program's file with an empty string).
#if defined(__linux__)
        std::error_code ec;
        const fs::path program = fs::read_symlink("/proc/self/exe", ec);
        if (!ec && !program.empty()) {
            return program;
        }
#endif
        return fs::path("the program of the process");
    }
    const fs::path file = fs::path(info.dli_fname).lexically_normal();
    if (same_path(file, environment.libpython)) {
        return std::nullopt; // the environment's own, loaded by an earlier start of this process
    }
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(file, ec);
    return ec ? file : canonical;
#endif
}

// Loads `library` for the rest of the process. INTERNAL when it does not load.
void* load_library(const fs::path& library, bool global, const std::string& what) {
#ifdef _WIN32
    static_cast<void>(global);
    // A library that cannot be loaded must produce an error code, never a dialog box.
    DWORD previous_mode = 0;
    const BOOL mode_set =
        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    const HMODULE module =
        LoadLibraryExW(native(library).c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD error = GetLastError();
    if (mode_set) {
        SetThreadErrorMode(previous_mode, nullptr);
    }
    if (module == nullptr) {
        fail(PRAENORA_STATUS_INTERNAL,
             "cannot load " + what + " " + quoted(library) + ": " + system_message(error),
             json{{"library", utf8(library)}});
    }
    return reinterpret_cast<void*>(module);
#else
    void* handle = dlopen(library.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
    if (handle == nullptr) {
        const char* error = dlerror();
        fail(PRAENORA_STATUS_INTERNAL,
             "cannot load " + what + " " + quoted(library) + ": " +
                 (error != nullptr ? error : "unknown error"),
             json{{"library", utf8(library)}});
    }
    return handle;
#endif
}

// The core's table, checked: UNSUPPORTED for a library that is not a core of this interface.
const praenora_python_core_v0* core_table(void* handle, const fs::path& core) {
    const json details{{"core", utf8(core)}};
#ifdef _WIN32
    const FARPROC symbol =
        GetProcAddress(reinterpret_cast<HMODULE>(handle), PRAENORA_PYTHON_CORE_ENTRY_POINT);
#else
    void* const symbol = dlsym(handle, PRAENORA_PYTHON_CORE_ENTRY_POINT);
#endif
    if (symbol == nullptr) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the Python core " + quoted(core) + " does not export " +
                 PRAENORA_PYTHON_CORE_ENTRY_POINT,
             details);
    }
    praenora_python_core_get_api_fn entry = nullptr;
    static_assert(sizeof entry == sizeof symbol, "function and object pointers have one size");
    std::memcpy(&entry, &symbol, sizeof entry);
    const praenora_python_core_v0* table = entry();
    if (table == nullptr || table->header.abi_major != PRAENORA_PYTHON_CORE_MAJOR ||
        table->header.reserved != 0 ||
        table->header.struct_size < sizeof(praenora_python_core_v0) || table->start == nullptr ||
        table->describe == nullptr || table->create_object == nullptr || table->call == nullptr ||
        table->release_object == nullptr || table->build_id == nullptr) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the Python core " + quoted(core) +
                 " gives no table of the interface the provider was built with (major " +
                 std::to_string(PRAENORA_PYTHON_CORE_MAJOR) + ", every function given)",
             details);
    }
    return table;
}

// 4. The core must be the one built with the provider.
void check_build_id(const praenora_python_core_v0& table, const fs::path& core) {
    const std::string expected = PRAENORA_PYTHON_CORE_BUILD_ID;
    TextSink id;
    if (table.build_id(id.get()) != PRAENORA_STATUS_OK) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the Python core " + quoted(core) + " does not report its build identity",
             json{{"core", utf8(core)}, {"expected_build_id", expected}});
    }
    if (id.text() != expected) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the Python core " + quoted(core) + " has the build identity " + id.text() +
                 ", and the provider was built with the core " + expected +
                 ": a core of another build is not started",
             json{{"core", utf8(core)}, {"build_id", id.text()}, {"expected_build_id", expected}});
    }
}

const praenora_python_core_v0* load_and_start(const praenora_provider_host* host,
                                              praenora_sink* error_json, praenora_status* status) {
    // 1. The files.
    const fs::path library = own_library();
    const EnvironmentFile names = read_environment_file(library);
    const Environment environment = read_environment(names);
    check_environment(names, environment);
    const bool loaded = core_loaded(names.core);
    if (!loaded) {
        // 2. Another interpreter.
        if (const std::optional<fs::path> other = another_interpreter(environment)) {
            fail(PRAENORA_STATUS_UNSUPPORTED,
                 "this process runs another Python interpreter, from " + quoted(*other) + ": " +
                     kModuleHostAdvice,
                 json{{"interpreter", utf8(*other)}});
        }
        // 3. libpython, globally.
        static_cast<void>(load_library(environment.libpython, true, "the Python library"));
    }
    // 4. The core: its table, its identity, its start.
    void* const handle = load_library(names.core, false, "the Python core");
    const praenora_python_core_v0* table = core_table(handle, names.core);
    check_build_id(*table, names.core);
    const std::string environment_dir = utf8(names.environment);
    const std::string sdk_dir = utf8(names.core.parent_path() / "python");
    *status = table->start(host, bytes(environment_dir), bytes(sdk_dir), error_json);
    return *status == PRAENORA_STATUS_OK ? table : nullptr;
}

} // namespace

const praenora_python_core_v0* Loader::start(const praenora_provider_host* host,
                                             praenora_sink* error_json,
                                             praenora_status* status) noexcept {
    praenora_status ignored = PRAENORA_STATUS_OK;
    if (status == nullptr) {
        status = &ignored;
    }
    static std::mutex mutex;
    static const praenora_python_core_v0* started = nullptr;
    try {
        const std::lock_guard<std::mutex> lock(mutex);
        if (started == nullptr) {
            started = load_and_start(host, error_json, status);
        } else {
            *status = PRAENORA_STATUS_OK;
        }
        return started;
    } catch (const LoadFailure& failure) {
        *status = failure.code;
        write_error(error_json, failure.code, failure.message,
                    failure.details.dump(-1, ' ', false, json::error_handler_t::replace));
    } catch (const std::bad_alloc&) {
        *status = PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        write_error(error_json, *status, "the provider ran out of memory loading the Python core");
    } catch (const std::exception& error) {
        *status = PRAENORA_STATUS_INTERNAL;
        write_error(error_json, *status,
                    std::string("the provider could not load the Python core: ") + error.what());
    } catch (...) {
        *status = PRAENORA_STATUS_INTERNAL;
        write_error(error_json, *status, "the provider could not load the Python core");
    }
    return nullptr;
}

std::string Loader::library_directory() {
    try {
        return utf8(own_library().parent_path());
    } catch (const LoadFailure& failure) {
        throw std::runtime_error(failure.message);
    }
}

} // namespace praenora::detail::python
