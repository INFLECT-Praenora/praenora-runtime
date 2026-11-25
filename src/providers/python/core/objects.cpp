// Objects, the code recorded for each path, and the import isolation (objects.hpp).
//
// The digests of paths are computed and recorded without the interpreter's lock, under a mutex of
// their own, which is never held while the lock is taken: a create that hashes a large tree does
// not hold up the Python code of other threads, and the two locks cannot wait for each other.
#include "objects.hpp"

#include "digest.hpp"
#include "documents.hpp"
#include "interpreter.hpp"
#include "native_module.hpp"
#include "python_include.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// The type python.h declares: the object, and where its records and cancel flag go.
struct praenora_python_object {
    PyObject* object = nullptr;                       // owned
    const praenora_host_api* host = nullptr;          // borrowed, may be null
    const praenora_provider_host* provider = nullptr; // borrowed
};

namespace praenora::detail::python {

namespace {

namespace fs = std::filesystem;

std::string quoted(const fs::path& path) { return "'" + path.u8string() + "'"; }

// The digest of the files below each path at its first create_object() in this process, by the
// path's canonical form. Never destroyed.
struct PathRecords {
    std::mutex mutex;
    std::map<std::string, std::string> digests;
};

PathRecords& path_records() {
    static PathRecords* const records = new PathRecords();
    return *records;
}

fs::path canonical_or_normal(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

bool is_digest(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::optional<std::string> recorded_digest(const std::string& key) {
    PathRecords& records = path_records();
    std::lock_guard<std::mutex> lock(records.mutex);
    const auto found = records.digests.find(key);
    return found == records.digests.end() ? std::nullopt
                                          : std::optional<std::string>(found->second);
}

void refuse_changed(const fs::path& path, std::string_view expected, const std::string& imported) {
    fail(PRAENORA_STATUS_DATA_INVALID,
         "the Python sources below " + quoted(path) +
             " changed since this process imported them; use a new process (a module host is "
             "one per step)",
         json{{"path", path.u8string()},
              {"expected_sha256", std::string(expected)},
              {"imported_sha256", imported}});
}

// The rules of a path's code, without the interpreter's lock: the files must have the digest
// given, and a path keeps the code of its first create for the life of the process.
void check_path_code(const fs::path& path, std::string_view path_sha256) {
    const std::string key = canonical_or_normal(path).u8string();
    if (const std::optional<std::string> recorded = recorded_digest(key)) {
        if (!path_sha256.empty() && path_sha256 != *recorded) {
            refuse_changed(path, path_sha256, *recorded);
        }
        return;
    }
    std::string actual;
    try {
        actual = directory_digest(path);
    } catch (const DigestError& error) {
        fail(error.code(),
             "cannot compute the digest of the Python sources below " + quoted(path) + ": " +
                 error.what(),
             json{{"path", path.u8string()}});
    }
    if (!path_sha256.empty() && path_sha256 != actual) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             "the Python sources below " + quoted(path) +
                 " differ from those the module was built with",
             json{{"path", path.u8string()},
                  {"expected_sha256", std::string(path_sha256)},
                  {"actual_sha256", actual}});
    }
    PathRecords& records = path_records();
    std::lock_guard<std::mutex> lock(records.mutex);
    const auto [entry, inserted] = records.digests.emplace(key, actual);
    if (!inserted && !path_sha256.empty() && path_sha256 != entry->second) {
        refuse_changed(path, path_sha256, entry->second);
    }
}

// Appends `path` to sys.path, once, after the environment's own entries.
void add_to_import_path(const fs::path& path) {
    PyObject* sys_path = PySys_GetObject("path"); // borrowed
    const std::string text = path.u8string();
    const Ref entry(
        PyUnicode_DecodeFSDefaultAndSize(text.data(), static_cast<Py_ssize_t>(text.size())));
    if (sys_path == nullptr || !PyList_Check(sys_path) || !entry) {
        throw raised_failure();
    }
    const int present = PySequence_Contains(sys_path, entry.get());
    if (present < 0 || (present == 0 && PyList_Append(sys_path, entry.get()) < 0)) {
        throw raised_failure();
    }
}

fs::path path_of(PyObject* text) {
    const Ref bytes(PyUnicode_EncodeFSDefault(text));
    if (!bytes) {
        PyErr_Clear();
        return fs::path();
    }
    return fs::u8path(std::string(PyBytes_AS_STRING(bytes.get()),
                                  static_cast<std::size_t>(PyBytes_GET_SIZE(bytes.get()))));
}

// Where a loaded module comes from: its file, or the directories of a namespace package; none for
// a module built into the interpreter.
std::vector<fs::path> locations_of(PyObject* module) {
    std::vector<fs::path> locations;
    const Ref file(PyObject_GetAttrString(module, "__file__"));
    if (file && PyUnicode_Check(file.get())) {
        locations.push_back(path_of(file.get()));
        return locations;
    }
    PyErr_Clear();
    const Ref directories(PyObject_GetAttrString(module, "__path__"));
    const Ref iterator(directories ? PyObject_GetIter(directories.get()) : nullptr);
    if (!iterator) {
        PyErr_Clear();
        return locations;
    }
    while (const Ref item = Ref(PyIter_Next(iterator.get()))) {
        if (PyUnicode_Check(item.get())) {
            locations.push_back(path_of(item.get()));
        }
    }
    PyErr_Clear();
    return locations;
}

bool below(const fs::path& file, const fs::path& directory) {
    const fs::path inner = canonical_or_normal(file);
    const fs::path outer = canonical_or_normal(directory);
    auto o = outer.begin();
    auto i = inner.begin();
    for (; o != outer.end(); ++o, ++i) {
        if (o->empty() && std::next(o) == outer.end()) {
            break; // a trailing separator
        }
        if (i == inner.end() || *i != *o) {
            return false;
        }
    }
    return i != inner.end();
}

// When the top-level package `top` is loaded, it must come from below `path`.
void check_package_location(const std::string& top, const fs::path& path) {
    const Ref name = text_object(top, "the package's name");
    PyObject* module = PyDict_GetItemWithError(PyImport_GetModuleDict(), name.get()); // borrowed
    if (module == nullptr) {
        if (PyErr_Occurred()) {
            throw raised_failure();
        }
        return;
    }
    const Ref held = Ref::borrowed(module);
    const std::vector<fs::path> locations = locations_of(module);
    const auto outside = std::find_if(locations.begin(), locations.end(),
                                      [&path](const fs::path& at) { return !below(at, path); });
    if (locations.empty() || outside != locations.end()) {
        const std::string where = locations.empty() ? "the interpreter itself" : quoted(*outside);
        fail(PRAENORA_STATUS_INVALID_ARGUMENT,
             "package " + top + " is already loaded from " + where + ", not from below " +
                 quoted(path),
             json{{"package", top},
                  {"location", locations.empty() ? std::string() : outside->u8string()},
                  {"path", path.u8string()}});
    }
}

// The callable `module_name:attribute`, imported; its top-level package must come from below
// `path` when one is given.
Ref import_callable(const std::string& module_name, const std::string& attribute,
                    const fs::path& path) {
    const std::string top = module_name.substr(0, module_name.find('.'));
    if (!path.empty()) {
        check_package_location(top, path);
    }
    const Ref name = text_object(module_name, "the module's name");
    Ref value(PyImport_Import(name.get()));
    if (!value) {
        const Failure failure = raised_failure();
        if (!path.empty()) {
            check_package_location(top, path); // a clash says more than the import's error
        }
        throw failure;
    }
    if (!path.empty()) {
        check_package_location(top, path);
    }
    std::size_t start = 0;
    while (start <= attribute.size()) {
        const std::size_t end = std::min(attribute.find('.', start), attribute.size());
        const Ref part = text_object(attribute.substr(start, end - start), "the callable's name");
        value = Ref(PyObject_GetAttr(value.get(), part.get()));
        if (!value) {
            throw raised_failure();
        }
        start = end + 1;
    }
    if (!PyCallable_Check(value.get())) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT, module_name + ":" + attribute + " is not callable",
             json{{"name", module_name + ":" + attribute}});
    }
    return value;
}

} // namespace

praenora_python_object* create_object(const praenora_provider_host* provider_host,
                                      const praenora_host_api* module_host, std::string_view path,
                                      std::string_view path_sha256, std::string_view name,
                                      std::string_view args_json) {
    const std::string what = "the arguments of " + std::string(name);
    check_size(args_json.size(), what);
    const std::size_t colon = name.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == name.size()) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT,
             "the callable '" + std::string(name) + "' is not written package.module:attribute",
             json{{"name", std::string(name)}});
    }
    const std::string module_name(name.substr(0, colon));
    const std::string attribute(name.substr(colon + 1));
    fs::path directory;
    if (!path.empty()) {
        directory = fs::u8path(path.begin(), path.end()).lexically_normal();
        if (!directory.is_absolute()) {
            fail(PRAENORA_STATUS_INVALID_ARGUMENT,
                 "the path " + quoted(directory) + " of the Python sources is not absolute",
                 json{{"path", directory.u8string()}});
        }
    }
    if (!path_sha256.empty() && !is_digest(path_sha256)) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT, "path_sha256 '" + std::string(path_sha256) +
                                                   "' is not 64 lowercase hexadecimal digits");
    }
    if (!path_sha256.empty() && directory.empty()) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT, "path_sha256 is given without a path");
    }
    if (!directory.empty()) {
        check_path_code(directory, path_sha256);
    }

    auto object = std::make_unique<praenora_python_object>();
    object->host = module_host;
    object->provider = provider_host;
    {
        const Gil gil;
        const ScopedCallTarget target({module_host, provider_host});
        const CallScope scope;
        if (!directory.empty()) {
            add_to_import_path(directory);
        }
        const Ref document = load_document(args_json, what);
        const Ref callable = import_callable(module_name, attribute, directory);
        Ref result(PyObject_CallOneArg(callable.get(), document.get()));
        if (!result) {
            throw raised_failure();
        }
        object->object = result.release();
    }
    return object.release();
}

std::string call_object(praenora_python_object& object, const praenora_host_api* module_host,
                        std::string_view method, std::string_view args_json) {
    const std::string name(method);
    if (name.empty()) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT, "call() needs the name of a method");
    }
    check_size(args_json.size(), "the arguments of " + name);
    const Gil gil;
    const ScopedCallTarget target(
        {module_host != nullptr ? module_host : object.host, object.provider});
    const CallScope scope;
    const Ref method_name = text_object(name, "the method's name");
    const Ref function(PyObject_GetAttr(object.object, method_name.get()));
    if (!function) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            throw raised_failure();
        }
        PyErr_Clear();
    }
    if (!function || !PyCallable_Check(function.get())) {
        fail(PRAENORA_STATUS_UNSUPPORTED, "the Python object has no method '" + name + "'",
             json{{"method", name}});
    }
    const Ref document = load_document(args_json, "the arguments of " + name);
    const Ref result(PyObject_CallOneArg(function.get(), document.get()));
    if (!result) {
        throw raised_failure();
    }
    return dump_document(result.get(), "the result of " + name);
}

void release_object(praenora_python_object* object) noexcept {
    if (object == nullptr) {
        return;
    }
    if (object->object != nullptr) {
        const Gil gil;
        try {
            // What the object prints as it goes is a record of its own host.
            const ScopedCallTarget target({object->host, object->provider});
            const CallScope scope;
            Py_CLEAR(object->object);
        } catch (...) {
            PyErr_Clear();
        }
        Py_XDECREF(object->object); // when the scope could not be made
    }
    delete object;
}

} // namespace praenora::detail::python
