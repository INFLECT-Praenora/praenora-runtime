// The process's interpreter (interpreter.hpp): its configuration, what the core prepares in it
// before it serves calls, and the checks of later starts.
//
// The core's build gives it, as compile definitions, the environment it was built against
// (PRAENORA_PYTHON_ENVIRONMENT_PACKAGE, PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256) and the digest
// of the praenora.sdk sources (PRAENORA_PYTHON_SDK_DIGEST).
#include "interpreter.hpp"

#include "digest.hpp"
#include "documents.hpp"
#include "environment.hpp"
#include "native_module.hpp"

#include <praenora/sdk/abi.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifndef PRAENORA_PYTHON_ENVIRONMENT_PACKAGE
#error "PRAENORA_PYTHON_ENVIRONMENT_PACKAGE is defined by the core's build"
#endif
#ifndef PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256
#error "PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256 is defined by the core's build"
#endif
#ifndef PRAENORA_PYTHON_SDK_DIGEST
#error "PRAENORA_PYTHON_SDK_DIGEST is defined by the core's build"
#endif

namespace praenora::detail::python {

namespace {

namespace fs = std::filesystem;

struct State {
    std::mutex mutex; // start() calls, one at a time
    std::atomic<bool> started{false};
    // A failure after the interpreter was initialized: the process cannot try again.
    std::optional<Failure> broken;
    Environment environment; // the running one
    fs::path sdk_dir;
    std::string description;
    Hooks hooks;
};

// Never destroyed: nothing that touches Python may run while the process exits.
State& state() {
    static State* const instance = new State();
    return *instance;
}

std::string quoted(const fs::path& path) { return "'" + path.u8string() + "'"; }

// The environment must be the one the core was built against: its libpython is the one linked.
void check_built_for(const Environment& environment) {
    const std::string package = PRAENORA_PYTHON_ENVIRONMENT_PACKAGE;
    const std::string lock = PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256;
    if (environment.lock_sha256 != lock || environment.package != package) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "the Python environment " + quoted(environment.directory) + " is " +
                 environment.package + " with the lock " + environment.lock_sha256 +
                 ", and the Python core was built for " + package + " with the lock " + lock,
             json{{"environment", environment.directory.u8string()},
                  {"package", environment.package},
                  {"lock_sha256", environment.lock_sha256},
                  {"expected_package", package},
                  {"expected_lock_sha256", lock}});
    }
}

// A later start must name the environment and the praenora.sdk that the interpreter runs.
void check_same(const State& s, const Environment& environment, const fs::path& sdk_dir) {
    if (!same_path(s.environment.directory, environment.directory)) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "this process runs the Python environment " + quoted(s.environment.directory) +
                 ", not " + quoted(environment.directory) + ": a process holds one interpreter",
             json{{"environment", environment.directory.u8string()},
                  {"running", s.environment.directory.u8string()}});
    }
    if (!same_path(s.sdk_dir, sdk_dir)) {
        fail(PRAENORA_STATUS_UNSUPPORTED,
             "this process imports praenora.sdk from " + quoted(s.sdk_dir) + ", not from " +
                 quoted(sdk_dir),
             json{{"sdk_dir", sdk_dir.u8string()}, {"running", s.sdk_dir.u8string()}});
    }
}

// The directory of praenora.sdk comes first on the import path: it may hold praenora/sdk and
// nothing else, which would come before the standard library and which no digest covers.
// Directories named __pycache__ are left out, as the digest leaves them out.
void check_sdk_directory(const fs::path& sdk_dir) {
    const auto check = [&sdk_dir](const fs::path& directory, const char* allowed) {
        std::error_code ec;
        fs::directory_iterator it(directory, ec);
        for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().u8string();
            std::error_code status_error;
            const bool is_directory = it->is_directory(status_error) && !status_error;
            if (is_directory && (name == allowed || name == "__pycache__")) {
                continue;
            }
            fail(PRAENORA_STATUS_DATA_INVALID,
                 "the directory of praenora.sdk " + quoted(sdk_dir) + " holds " +
                     quoted(it->path()) +
                     ", which is not part of praenora.sdk: it may hold praenora/sdk only, since it "
                     "comes first on the import path",
                 json{{"sdk_dir", sdk_dir.u8string()}, {"path", it->path().u8string()}});
        }
        if (ec) {
            fail(PRAENORA_STATUS_IO_ERROR,
                 "cannot list the directory " + quoted(directory) + ": " + ec.message(),
                 json{{"path", directory.u8string()}});
        }
    };
    check(sdk_dir, "praenora");
    check(sdk_dir / "praenora", "sdk");
}

// The praenora.sdk sources must be those the core was built with.
void check_sdk(const fs::path& sdk_dir) {
    check_sdk_directory(sdk_dir);
    const fs::path sources = sdk_dir / "praenora" / "sdk";
    std::string actual;
    try {
        actual = directory_digest(sources);
    } catch (const DigestError& error) {
        fail(error.code(),
             "cannot check the praenora.sdk sources below " + quoted(sources) + ": " + error.what(),
             json{{"path", sources.u8string()}});
    }
    const std::string expected = PRAENORA_PYTHON_SDK_DIGEST;
    if (actual != expected) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             "the praenora.sdk sources below " + quoted(sources) +
                 " differ from those the Python core was built with",
             json{{"path", sources.u8string()},
                  {"expected_sha256", expected},
                  {"actual_sha256", actual}});
    }
}

void check_status(const PyStatus& status, const std::string& what) {
    if (PyStatus_Exception(status)) {
        std::string message = "the Python interpreter could not be started: " + what;
        if (status.func != nullptr) {
            message += std::string(" (") + status.func + ")";
        }
        if (status.err_msg != nullptr) {
            message += std::string(": ") + status.err_msg;
        }
        if (PyStatus_IsExit(status)) {
            message += ": it exited with code " + std::to_string(status.exitcode);
        }
        fail(PRAENORA_STATUS_INTERNAL, message);
    }
}

void set_string(PyConfig& config, wchar_t** field, const fs::path& path, const char* what) {
    check_status(PyConfig_SetBytesString(&config, field, path.u8string().c_str()), what);
}

// A failure of the Python code that prepares the interpreter.
[[noreturn]] void preparation_failed(const std::string& what) {
    Failure failure = raised_failure();
    failure.code = PRAENORA_STATUS_INTERNAL;
    failure.message = "the Python core could not " + what + ": " + failure.message;
    throw failure;
}

Ref import(const char* name) {
    Ref module(PyImport_ImportModule(name));
    if (!module) {
        preparation_failed(std::string("import ") + name);
    }
    return module;
}

// A new reference to module.name, kept by the core for the life of the process.
PyObject* kept(PyObject* module, const char* name) {
    PyObject* value = PyObject_GetAttrString(module, name);
    if (value == nullptr) {
        preparation_failed(std::string("find ") + name);
    }
    return value;
}

void call_hook(PyObject* module, const char* name, PyObject* argument) {
    const Ref function(kept(module, name));
    const Ref result(argument != nullptr ? PyObject_CallOneArg(function.get(), argument)
                                         : PyObject_CallNoArgs(function.get()));
    if (!result) {
        preparation_failed(std::string("run praenora.sdk._runtime.") + name + "()");
    }
}

// What the core prepares in the interpreter, the lock held: praenora.sdk first on sys.path, its
// native module, the hooks, the captured output and the children's start method.
void prepare(State& s, const fs::path& sdk_dir) {
    PyObject* path = PySys_GetObject("path"); // borrowed
    const std::string sdk_text = sdk_dir.u8string();
    const Ref entry(PyUnicode_DecodeFSDefaultAndSize(sdk_text.data(),
                                                     static_cast<Py_ssize_t>(sdk_text.size())));
    if (path == nullptr || !PyList_Check(path) || !entry ||
        PyList_Insert(path, 0, entry.get()) < 0) {
        preparation_failed("put praenora.sdk on sys.path");
    }
    const Ref native(create_native_module());
    if (!native) {
        preparation_failed("create praenora.sdk._native");
    }
    const Ref sdk = import("praenora.sdk");
    // The package must be the one below sdk_dir, which comes first on sys.path: a package
    // praenora of the environment would hide it.
    const Ref file(PyObject_GetAttrString(sdk.get(), "__file__"));
    if (!file || !PyUnicode_Check(file.get())) {
        preparation_failed("find praenora.sdk's file");
    }
    const fs::path sdk_file = fs::u8path(utf8_of(file.get()));
    if (!same_path(sdk_file.parent_path(), sdk_dir / "praenora" / "sdk")) {
        fail(PRAENORA_STATUS_DATA_INVALID,
             "praenora.sdk was imported from " + quoted(sdk_file) + ", not from below " +
                 quoted(sdk_dir),
             json{{"sdk_dir", sdk_dir.u8string()}, {"file", sdk_file.u8string()}});
    }
    const Ref runtime = import("praenora.sdk._runtime");
    const Ref traceback = import("traceback");
    s.hooks.format_exception = kept(traceback.get(), "format_exception");
    s.hooks.sdk_error = kept(sdk.get(), "Error");
    s.hooks.sdk_cancelled = kept(sdk.get(), "Cancelled");
    s.hooks.load_document = kept(runtime.get(), "load_document");
    s.hooks.dump_document = kept(runtime.get(), "dump_document");
    s.hooks.enter_call = kept(runtime.get(), "enter_call");
    s.hooks.exit_call = kept(runtime.get(), "exit_call");

    call_hook(runtime.get(), "install", native.get());
    const Ref backend_function(kept(runtime.get(), "backend"));
    const Ref backend(PyObject_CallNoArgs(backend_function.get()));
    if (!backend) {
        preparation_failed("make the backend of praenora.sdk.Host");
    }
    const Ref host_class(kept(sdk.get(), "Host"));
    s.hooks.call_host = PyObject_CallOneArg(host_class.get(), backend.get());
    if (s.hooks.call_host == nullptr) {
        preparation_failed("make a praenora.sdk.Host");
    }
    call_hook(runtime.get(), "capture_output", nullptr);
    call_hook(runtime.get(), "configure_children", nullptr);
}

// The directory of cached bytecode that the interpreter is given (sys.pycache_prefix), which can
// hold no file: a path below the environment's manifest, which read_environment() read as a
// regular file, and no system lets a file be a directory. The interpreter then reads no cached
// bytecode, neither below the prefix nor in a __pycache__ directory beside a source, and compiles
// every module it imports from its source, the code that the digests cover. It writes none
// either (write_bytecode).
fs::path no_bytecode_directory(const Environment& environment) {
    return environment.manifest / "no-bytecode";
}

// Set once the process exits (an exit handler that the first start registers): from then on no
// thread state is released, since other threads may still run Python code.
std::atomic<bool> g_exiting{false};

void mark_exiting() { g_exiting.store(true, std::memory_order_release); }

// Whether the end of the calling thread may release its kept thread state, which takes the
// interpreter's lock: not while the process exits, not on the process's main thread (whose
// thread-local objects are destroyed as the process exits), and not on Windows, where a thread's
// thread-local objects are destroyed under the loader's lock, which a thread that holds the
// interpreter's lock may be waiting for. A state that is not released stays with the
// interpreter, which is never finalized.
bool thread_end_may_release() noexcept {
#if defined(_WIN32)
    return false;
#else
    if (g_exiting.load(std::memory_order_acquire)) {
        return false;
    }
#if defined(__APPLE__)
    return pthread_main_np() == 0;
#elif defined(__linux__)
    return static_cast<pid_t>(::syscall(SYS_gettid)) != ::getpid();
#else
    return false;
#endif
#endif
}

// The thread state that a thread keeps (keep_thread_state()): made with PyGILState_Ensure(), which
// binds it to the thread, and held with the lock released, so that the PyGILState_Ensure() of each
// later call on the thread finds it; released, as CPython ends the threads it starts, when the
// thread ends (thread_end_may_release()). A thread that has a thread state already (the one that
// started the interpreter) keeps its own.
class ThreadState {
public:
    ThreadState() noexcept {
        if (PyGILState_GetThisThreadState() != nullptr) {
            return;
        }
        static_cast<void>(PyGILState_Ensure());
        state_ = PyEval_SaveThread();
    }
    ~ThreadState() {
        if (state_ == nullptr || !thread_end_may_release()) {
            return;
        }
        PyEval_RestoreThread(state_);
        PyThreadState_Clear(state_);
        PyThreadState_DeleteCurrent(); // releases the lock
    }
    ThreadState(const ThreadState&) = delete;
    ThreadState& operator=(const ThreadState&) = delete;

private:
    PyThreadState* state_ = nullptr;
};

// The first start of the process: initializes the interpreter on this thread, prepares it, and
// releases its lock, which every call then takes with PyGILState_Ensure().
void initialize(State& s, const Environment& environment, const fs::path& sdk_dir,
                const praenora_provider_host* provider_host) {
    PyPreConfig preconfig;
    PyPreConfig_InitIsolatedConfig(&preconfig);
    preconfig.utf8_mode = 1;
    check_status(Py_PreInitialize(&preconfig), "pre-initialization");

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    struct Clear {
        PyConfig* config;
        ~Clear() { PyConfig_Clear(config); }
    } clear{&config};
    set_string(config, &config.home, environment.home, "the home");
    set_string(config, &config.executable, environment.executable, "the executable");
    set_string(config, &config.pycache_prefix, no_bytecode_directory(environment), "the cache");
    config.write_bytecode = 0;
    config.install_signal_handlers = 0;
    config.use_hash_seed = 1;
    config.hash_seed = 0;
    config.parse_argv = 0;
    char empty[] = "";
    char* argv[] = {empty};
    check_status(PyConfig_SetBytesArgv(&config, 1, argv), "the arguments");

    // What the state keeps is made now: once the interpreter runs, nothing may throw while this
    // thread holds its lock but prepare(), whose failures release it.
    Environment running = environment;
    fs::path running_sdk = sdk_dir;
    std::string description = describe_environment(environment);
    Failure unprepared{PRAENORA_STATUS_INTERNAL, "the Python core could not be prepared"};

    // Records written while the interpreter is prepared go to this start's provider host.
    set_default_provider(provider_host);
    const PyStatus status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        try {
            check_status(status, "initialization");
        } catch (const Failure& failure) {
            s.broken = failure;
            throw;
        }
    }
    // The interpreter runs, and this thread holds its lock.
    try {
        prepare(s, sdk_dir);
    } catch (const Failure& failure) {
        PyEval_SaveThread();
        s.broken = failure;
        throw;
    } catch (...) {
        PyEval_SaveThread();
        s.broken = std::move(unprepared);
        throw *s.broken;
    }
    PyEval_SaveThread();
    static_cast<void>(std::atexit(&mark_exiting));
    s.environment = std::move(running);
    s.sdk_dir = std::move(running_sdk);
    s.description = std::move(description);
    s.started.store(true, std::memory_order_release);
}

} // namespace

void start_interpreter(const praenora_provider_host* provider_host, const fs::path& environment_dir,
                       const fs::path& sdk_dir) {
    if (!environment_dir.is_absolute()) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT,
             "the Python environment's directory " + quoted(environment_dir) + " is not absolute");
    }
    if (!sdk_dir.is_absolute()) {
        fail(PRAENORA_STATUS_INVALID_ARGUMENT,
             "the directory of praenora.sdk " + quoted(sdk_dir) + " is not absolute");
    }
    const Environment environment = read_environment(environment_dir.lexically_normal());
    check_built_for(environment);
    const fs::path sdk = sdk_dir.lexically_normal();

    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.broken) {
            throw *s.broken;
        }
        if (s.started.load(std::memory_order_acquire)) {
            check_same(s, environment, sdk);
        } else {
            if (Py_IsInitialized()) {
                fail(PRAENORA_STATUS_UNSUPPORTED,
                     "the process runs another Python interpreter, which the Python core "
                     "cannot share: run the step in a module host (isolation mode process)");
            }
            check_sdk(sdk);
            initialize(s, environment, sdk, provider_host);
        }
    }
    for (const std::string& warning : variable_warnings(environment)) {
        log_to_provider(provider_host, PRAENORA_LOG_WARN, warning);
    }
}

void keep_thread_state() noexcept {
    thread_local ThreadState kept;
    static_cast<void>(kept);
}

bool interpreter_started() noexcept { return state().started.load(std::memory_order_acquire); }

const std::string& interpreter_description() noexcept { return state().description; }

const Hooks& hooks() noexcept { return state().hooks; }

CallScope::CallScope() {
    PyObject* enter = hooks().enter_call;
    if (enter == nullptr) {
        return;
    }
    const Ref result(PyObject_CallOneArg(enter, hooks().call_host));
    if (!result) {
        throw raised_failure();
    }
    entered_ = true;
}

CallScope::~CallScope() {
    if (!entered_) {
        return;
    }
    // exit_call() runs with the exception that is being raised, if any, put aside.
    PyObject* raised = PyErr_GetRaisedException();
    const Ref result(PyObject_CallNoArgs(hooks().exit_call));
    if (!result) {
        PyErr_Clear();
    }
    PyErr_SetRaisedException(raised);
}

} // namespace praenora::detail::python
