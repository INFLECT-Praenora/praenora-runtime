// The process's interpreter: started once, by the core's first start(), and never finalized
// (docs/specs/python-service.md, "The interpreter"); later starts check that they name the
// environment it runs.
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_INTERPRETER_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_INTERPRETER_HPP

#include "python_include.hpp"

#include "provider_abi.h"

#include <filesystem>
#include <string>

namespace praenora::detail::python {

// The Python objects the core calls, found by the first start and kept for the life of the
// process: the interpreter is never finalized, so they are never released. A null one is not
// found yet (during the first start).
struct Hooks {
    PyObject* load_document = nullptr;    // praenora.sdk._runtime.load_document
    PyObject* dump_document = nullptr;    // praenora.sdk._runtime.dump_document
    PyObject* enter_call = nullptr;       // praenora.sdk._runtime.enter_call
    PyObject* exit_call = nullptr;        // praenora.sdk._runtime.exit_call
    PyObject* call_host = nullptr;        // the praenora.sdk.Host that enter_call is given
    PyObject* sdk_error = nullptr;        // praenora.sdk.Error
    PyObject* sdk_cancelled = nullptr;    // praenora.sdk.Cancelled
    PyObject* format_exception = nullptr; // traceback.format_exception
};

// The first call of the process initializes the interpreter with the environment in
// `environment_dir` and praenora.sdk from `sdk_dir`; a later one checks that it names the same
// environment and praenora.sdk. Each logs, to `provider_host`, a warning for each of the process's
// environment variables that is unset or has another value. Throws Failure. Thread-safe.
void start_interpreter(const praenora_provider_host* provider_host,
                       const std::filesystem::path& environment_dir,
                       const std::filesystem::path& sdk_dir);

// True once a start succeeded.
bool interpreter_started() noexcept;

// After a successful start: describe()'s document, and the hooks.
const std::string& interpreter_description() noexcept;
const Hooks& hooks() noexcept;

// Brackets a call with praenora.sdk._runtime.enter_call() and exit_call(), keeping the exception
// that is being raised, if any. Needs the interpreter's lock.
class CallScope {
public:
    CallScope();
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;

private:
    bool entered_ = false;
};

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_INTERPRETER_HPP
