// Praenora module SDK: the native library of a Python module (docs/specs/python-service.md).
//
// A Python module is a class derived from praenora.sdk.Module plus a module library that forwards
// the ABI v0 table (abi.h) to that class through the service praenora.python@0 (python.h).
// praenora_add_python_module() generates the library's one source file, which includes this header
// and expands, once, at namespace scope,
//     PRAENORA_SDK_DEFINE_PYTHON_MODULE("package.module:Class", "<sources digest>", <0 or 1>)
// with the class, the directory digest of the module's Python sources and whether the table has a
// self_test. The library needs neither Python's headers nor libpython: any C++17 compiler builds
// it. Its sources are in <library directory>/<name>.python/, <name> being PRAENORA_MODULE_NAME.
//
// The table:
//   - describe() writes the identity of PRAENORA_MODULE_NAME, PRAENORA_MODULE_VERSION and
//     PRAENORA_MODULE_CAPABILITIES (string literals where the macro is expanded) with module.hpp's
//     writer, the C++ adapter's own, and no call into Python; this header uses module.hpp's other
//     helpers too.
//   - create() gets praenora.python@0 from the host. Without it, or with a table it cannot use
//     (another major, a nonzero reserved field, too short, a NULL function), it fails with
//     UNSUPPORTED: "the service praenora.python@0 is not available". It then finds the directory of
//     its own library file and calls
//         create_object(host, <directory>/<name>.python, <digest>,
//                       "<package.module:Class>._praenora_adapter", {"create": <create document>})
//     whose status and error document are create()'s. The class method _praenora_adapter, which
//     praenora.sdk.Module defines, returns the object that the other functions call; the class's
//     own top-level package is the one the service imports, so the service's import isolation
//     applies to it. An empty create document is INVALID_ARGUMENT.
//   - initialize(), invoke(), finalize() and diagnostics() call the object's method of that name
//     with the call's document (finalize and diagnostics with null); invoke's result goes to the
//     host's result sink as the service writes it, and diagnostics' to its output sink.
//   - self_test(), only when the macro's last argument is nonzero (NULL otherwise), creates a
//     class-only object ({"create": null}), calls its self_test, which calls the class method
//     self_test(host), and releases it. The ABI's self_test has no error sink: a failure's error
//     document becomes a log record of level error.
//   - destroy() releases the object.
// Errors of the adapter itself are those of module.hpp's: INVALID_ARGUMENT for a NULL argument,
// UNSUPPORTED for a host API of another ABI major, RESOURCE_EXHAUSTED when memory runs out,
// INTERNAL when the library's own file cannot be found.
//
// praenora::sdk::library_directory() is public: a C or C++ module that uses praenora.python@0
// directly finds the Python sources it installs beside its library with it.
//
// On Windows this header includes <windows.h> (with NOMINMAX and WIN32_LEAN_AND_MEAN, unless the
// module defined them), for GetModuleHandleExW() and GetModuleFileNameW(); elsewhere <dlfcn.h>, for
// dladdr() (on Linux before glibc 2.34 the library links libdl: praenora_add_python_module() links
// ${CMAKE_DL_LIBS}).
#ifndef PRAENORA_SDK_PYTHON_MODULE_HPP
#define PRAENORA_SDK_PYTHON_MODULE_HPP

#include <praenora/sdk/abi.h>
#include <praenora/sdk/module.hpp>
#include <praenora/sdk/python.h>
#include <praenora/sdk/status.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#define PRAENORA_SDK_PYTHON_MODULE_UNDEF_NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define PRAENORA_SDK_PYTHON_MODULE_UNDEF_LEAN
#endif
#include <windows.h>
#ifdef PRAENORA_SDK_PYTHON_MODULE_UNDEF_NOMINMAX
#undef NOMINMAX
#undef PRAENORA_SDK_PYTHON_MODULE_UNDEF_NOMINMAX
#endif
#ifdef PRAENORA_SDK_PYTHON_MODULE_UNDEF_LEAN
#undef WIN32_LEAN_AND_MEAN
#undef PRAENORA_SDK_PYTHON_MODULE_UNDEF_LEAN
#endif
#else
#include <cerrno>
#include <dlfcn.h>
#include <unistd.h>
#endif

static_assert(PRAENORA_PYTHON_SERVICE_MAJOR == 0u,
              "praenora/sdk/python_module.hpp uses the praenora.python@0 table");

namespace praenora::sdk {

// The absolute directory, in UTF-8, of the library file (or the program) whose code or data holds
// `address`, without a separator at its end; empty when the system cannot tell. Pass the address
// of an object or a function of the library itself.
inline std::string library_directory(const void* address) {
    std::string file;
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module)) {
        return std::string();
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    for (;;) {
        length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::string();
        }
        if (length < buffer.size()) {
            break;
        }
        if (buffer.size() >= 32768) {
            return std::string(); // longer than any path the system gives
        }
        buffer.resize(buffer.size() * 2);
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    file.assign(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length), &file[0], size,
                        nullptr, nullptr);
    const std::size_t separator = file.find_last_of("\\/");
    return separator == std::string::npos ? std::string() : file.substr(0, separator);
#else
    Dl_info info{};
    if (dladdr(address, &info) != 0 && info.dli_fname != nullptr) {
        file = info.dli_fname;
    }
#if defined(__linux__)
    if (file.empty()) {
        // glibc names the program's own file with an empty string.
        std::vector<char> buffer(4096);
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length > 0 && static_cast<std::size_t>(length) < buffer.size()) {
            file.assign(buffer.data(), static_cast<std::size_t>(length));
        }
    }
#endif
    if (file.empty()) {
        return std::string();
    }
    if (file.front() != '/') {
        // The path the library was loaded by, relative to the working directory.
        std::vector<char> directory(4096);
        while (getcwd(directory.data(), directory.size()) == nullptr) {
            if (errno != ERANGE || directory.size() >= (std::size_t{1} << 20)) {
                return std::string();
            }
            directory.resize(directory.size() * 2);
        }
        file = std::string(directory.data()) + "/" + file;
    }
    const std::size_t separator = file.find_last_of('/');
    return separator == 0 ? std::string("/") : file.substr(0, separator);
#endif
}

namespace detail {

inline praenora_status PRAENORA_CALL python_discard(void*, const char*, std::size_t) noexcept {
    return PRAENORA_STATUS_OK;
}

inline praenora_bytes python_bytes(std::string_view text) noexcept {
    return praenora_bytes{text.data(), text.size()};
}

// A sink that keeps what is written to it, for an error document that becomes a log record.
struct PythonErrorText {
    std::string text;
    praenora_sink sink{this, &PythonErrorText::write};

    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        if (data == nullptr && size != 0) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        try {
            static_cast<PythonErrorText*>(context)->text.append(data == nullptr ? "" : data, size);
            return PRAENORA_STATUS_OK;
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
    }
};

// The table of praenora.python@0 that `host` serves, when it serves one this adapter can use:
// major 0, reserved 0, the v0.0 functions there and set. NULL otherwise.
inline const praenora_python_v0* python_service(const praenora_host_api* host) noexcept {
    const auto* table = static_cast<const praenora_python_v0*>(
        Host(host).service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR));
    if (table == nullptr || table->header.abi_major != PRAENORA_PYTHON_SERVICE_MAJOR ||
        table->header.reserved != 0u || table->header.struct_size < PRAENORA_PYTHON_V0_BASE_SIZE ||
        table->create_object == nullptr || table->call == nullptr ||
        table->release_object == nullptr) {
        return nullptr;
    }
    return table;
}

// The v0 table of a Python module. Identity provides, as static functions: name(), version() and
// capabilities() (as for ModuleAdapter), python_class() ("package.module:Class"),
// sources_sha256() (the digest of <library directory>/<name>.python) and address() (an object of
// the library, which locates its file). Used through the macros below.
template <class Identity, bool HasSelfTest> class PythonModuleAdapter {
public:
    // The table for ABI major 0, or NULL for any other major.
    static const praenora_api_header* get_api(std::uint32_t abi_major) noexcept {
        static constexpr praenora_module_api_v0 table = {
            {PRAENORA_ABI_MAJOR, PRAENORA_ABI_MINOR,
             static_cast<std::uint32_t>(sizeof(praenora_module_api_v0)), 0u},
            &describe,
            &create,
            &initialize,
            &invoke,
            &finalize,
            &destroy,
            &diagnostics,
            HasSelfTest ? &self_test : nullptr,
        };
        return abi_major == 0u ? &table.header : nullptr;
    }

private:
    // What an instance handle points to: the service's table and the module's object.
    struct Instance {
        const praenora_python_v0* python = nullptr;
        praenora_python_object* object = nullptr;
    };

    static constexpr std::string_view kUnavailable =
        "the service praenora.python@0 is not available";

    static Instance* from(praenora_instance* handle) noexcept {
        return instance_from<Instance>(handle);
    }

    // The callable that creates the module's object: the class method of praenora.sdk.Module.
    static std::string callable() {
        return std::string(Identity::python_class()) + "._praenora_adapter";
    }

    // <library directory>/<name>.python; Error(INTERNAL) when the library's file is unknown.
    static std::string sources_directory() {
        const std::string directory = library_directory(Identity::address());
        if (directory.empty()) {
            throw Error(PRAENORA_STATUS_INTERNAL,
                        "cannot find the file of the module library, beside which its Python "
                        "sources are");
        }
#if defined(_WIN32)
        const char* separator = "\\";
#else
        const char* separator = directory == "/" ? "" : "/";
#endif
        return directory + separator + Identity::name() + ".python";
    }

    // Creates the module's object from the arguments document `args`.
    static praenora_status create_object(const praenora_python_v0* python,
                                         const praenora_host_api* host, const std::string& args,
                                         praenora_python_object** object,
                                         praenora_sink* error_json) {
        const std::string directory = sources_directory();
        const std::string name = callable();
        return python->create_object(python->context, host, python_bytes(directory),
                                     python_bytes(Identity::sources_sha256()), python_bytes(name),
                                     python_bytes(args), object, error_json);
    }

    static praenora_status PRAENORA_CALL describe(praenora_sink* out_json) noexcept {
        return write_identity<Identity>(out_json);
    }

    static praenora_status PRAENORA_CALL create(const praenora_host_api* host,
                                                praenora_bytes create_json,
                                                praenora_instance** out_instance,
                                                praenora_sink* error_json) noexcept {
        if (out_instance == nullptr) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "create: out_instance is NULL");
        }
        *out_instance = nullptr;
        if (host == nullptr) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "create: the host API is NULL");
        }
        if (!is_v0_host(host)) {
            return fail(error_json, PRAENORA_STATUS_UNSUPPORTED,
                        "create: the host API is not an ABI v0 host API");
        }
        const std::string_view document =
            !readable(create_json) || create_json.size == 0
                ? std::string_view()
                : std::string_view(create_json.data, create_json.size);
        if (document.find_first_not_of(" \t\r\n") == std::string_view::npos) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "create: the create document is NULL or empty");
        }
        const praenora_python_v0* python = python_service(host);
        if (python == nullptr) {
            return fail(error_json, PRAENORA_STATUS_UNSUPPORTED, kUnavailable);
        }
        praenora_sink discard{nullptr, &python_discard};
        praenora_sink* errors = usable(error_json) ? error_json : &discard;
        praenora_status created = PRAENORA_STATUS_OK;
        const praenora_status status = guarded(error_json, [&] {
            auto instance = std::make_unique<Instance>();
            instance->python = python;
            std::string args = "{\"create\":";
            args.append(document);
            args += "}";
            created = create_object(python, host, args, &instance->object, errors);
            if (created == PRAENORA_STATUS_OK && instance->object == nullptr) {
                throw Error(PRAENORA_STATUS_INTERNAL,
                            "create: the service praenora.python@0 returned OK without an object");
            }
            if (created == PRAENORA_STATUS_OK) {
                *out_instance = reinterpret_cast<praenora_instance*>(instance.release());
            }
        });
        return status != PRAENORA_STATUS_OK ? status : created;
    }

    // Calls the object's `method` with `document`, its result to `result` (NULL: dropped).
    static praenora_status call(Instance& instance, std::string_view method,
                                praenora_bytes document, praenora_sink* result,
                                praenora_sink* error_json) noexcept {
        praenora_sink discard{nullptr, &python_discard};
        return instance.python->call(instance.object, python_bytes(method), document,
                                     result != nullptr ? result : &discard,
                                     usable(error_json) ? error_json : &discard);
    }

    static praenora_status PRAENORA_CALL initialize(praenora_instance* handle,
                                                    praenora_bytes initialize_json,
                                                    praenora_sink* error_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr || !readable(initialize_json)) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "initialize: the instance or the initialize document is NULL");
        }
        return call(*instance, "initialize", initialize_json, nullptr, error_json);
    }

    static praenora_status PRAENORA_CALL invoke(praenora_instance* handle,
                                                praenora_bytes invocation_json,
                                                praenora_sink* result_json,
                                                praenora_sink* error_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr || !usable(result_json) || !readable(invocation_json)) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "invoke: the instance, the invocation document or the result sink is NULL");
        }
        return call(*instance, "invoke", invocation_json, result_json, error_json);
    }

    static praenora_status PRAENORA_CALL finalize(praenora_instance* handle,
                                                  praenora_sink* error_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr) {
            return fail(error_json, PRAENORA_STATUS_INVALID_ARGUMENT,
                        "finalize: the instance is NULL");
        }
        return call(*instance, "finalize", praenora_bytes{nullptr, 0}, nullptr, error_json);
    }

    static void PRAENORA_CALL destroy(praenora_instance* handle) noexcept {
        Instance* instance = from(handle);
        if (instance != nullptr) {
            instance->python->release_object(instance->object);
            delete instance;
        }
    }

    static praenora_status PRAENORA_CALL diagnostics(praenora_instance* handle,
                                                     praenora_sink* out_json) noexcept {
        Instance* instance = from(handle);
        if (instance == nullptr || !usable(out_json)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        return call(*instance, "diagnostics", praenora_bytes{nullptr, 0}, out_json, nullptr);
    }

    // Logs `error` (an error document, or the adapter's own text) at level error.
    static void log_failure(const praenora_host_api* host, std::string_view error) noexcept {
        try {
            Host(host).log(LogLevel::error, "self_test: " + std::string(error));
        } catch (...) {
            Host(host).log(LogLevel::error, "self_test failed");
        }
    }

    static praenora_status PRAENORA_CALL self_test(const praenora_host_api* host,
                                                   praenora_sink* report_json) noexcept {
        if (host == nullptr || !usable(report_json)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        if (!is_v0_host(host)) {
            return PRAENORA_STATUS_UNSUPPORTED;
        }
        const praenora_python_v0* python = python_service(host);
        if (python == nullptr) {
            log_failure(host, kUnavailable);
            return PRAENORA_STATUS_UNSUPPORTED;
        }
        // One error document at most reaches `error`: the service's, or the adapter's own when it
        // throws before it calls the service.
        PythonErrorText error;
        praenora_python_object* object = nullptr;
        praenora_status created = PRAENORA_STATUS_OK;
        praenora_status status = guarded(&error.sink, [&] {
            created = create_object(python, host, "{\"create\":null}", &object, &error.sink);
        });
        if (status == PRAENORA_STATUS_OK) {
            status = created;
        }
        if (status == PRAENORA_STATUS_OK && object == nullptr) {
            status = PRAENORA_STATUS_INTERNAL;
        }
        if (status == PRAENORA_STATUS_OK) {
            status = python->call(object, python_bytes("self_test"), praenora_bytes{nullptr, 0},
                                  report_json, &error.sink);
        }
        python->release_object(object); // NULL does nothing
        if (status != PRAENORA_STATUS_OK) {
            log_failure(host, error.text.empty() ? std::string_view("the class-only object of "
                                                                    "the module was not created")
                                                 : std::string_view(error.text));
        }
        return status;
    }
};

} // namespace detail
} // namespace praenora::sdk

// Defines `getter_fn`, a function with the praenora_module_get_api_fn signature that returns the
// static v0 table of the Python module of class `class_literal` ("package.module:Class"), whose
// sources have the directory digest `digest_literal` (64 lowercase hexadecimal digits), with a
// self_test when `has_self_test` is nonzero; NULL for another ABI major. It is not exported, so a
// test executable may drive the module in process (praenora/sdk/testing.hpp); its sources are then
// beside the program. The identity is taken from PRAENORA_MODULE_NAME, PRAENORA_MODULE_VERSION and
// PRAENORA_MODULE_CAPABILITIES as defined where the macro is expanded. Prefix the expansion with
// `static`, or put it in an unnamed namespace, for internal linkage.
#define PRAENORA_SDK_DEFINE_PYTHON_MODULE_GETTER(class_literal, digest_literal, has_self_test,     \
                                                 getter_fn)                                        \
    const praenora_api_header* PRAENORA_CALL getter_fn(uint32_t praenora_sdk_abi_major) {          \
        struct praenora_sdk_python_module_identity {                                               \
            static const char* name() noexcept { return PRAENORA_MODULE_NAME; }                    \
            static const char* version() noexcept { return PRAENORA_MODULE_VERSION; }              \
            static const char* capabilities() noexcept { return PRAENORA_MODULE_CAPABILITIES; }    \
            static const char* python_class() noexcept { return class_literal; }                   \
            static const char* sources_sha256() noexcept { return digest_literal; }                \
            static const void* address() noexcept {                                                \
                static const char marker = 0;                                                      \
                return &marker;                                                                    \
            }                                                                                      \
        };                                                                                         \
        return ::praenora::sdk::detail::PythonModuleAdapter<praenora_sdk_python_module_identity,   \
                                                            ((has_self_test) !=                    \
                                                             0)>::get_api(praenora_sdk_abi_major); \
    }

// Defines the Python module: a getter as above, with internal linkage, and the library's single
// exported entry point, praenora_module_get_api, which returns its table. Expand it once per
// library, at namespace scope; praenora_add_python_module() generates the source that does.
#define PRAENORA_SDK_DEFINE_PYTHON_MODULE(class_literal, digest_literal, has_self_test)            \
    namespace {                                                                                    \
    PRAENORA_SDK_DEFINE_PYTHON_MODULE_GETTER(class_literal, digest_literal, has_self_test,         \
                                             praenora_sdk_python_module_get_api)                   \
    }                                                                                              \
    PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL              \
    praenora_module_get_api(uint32_t abi_major) {                                                  \
        return praenora_sdk_python_module_get_api(abi_major);                                      \
    }

#endif // PRAENORA_SDK_PYTHON_MODULE_HPP
