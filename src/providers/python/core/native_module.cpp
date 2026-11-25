// praenora.sdk._native (native_module.hpp). Its functions find their host in a per-thread stack of
// call targets, and call it with the interpreter's lock released, so that a host that blocks
// (a module host writing to its channel, for example) never holds up other Python threads.
#include "native_module.hpp"

#include "documents.hpp"

#include <praenora/sdk/abi.h>

#include <atomic>
#include <exception>
#include <string>

namespace praenora::detail::python {

namespace {

thread_local const ScopedCallTarget* t_innermost = nullptr;
std::atomic<const praenora_provider_host*> g_default_provider{nullptr};

CallTarget current_target() noexcept {
    if (t_innermost != nullptr) {
        return t_innermost->target();
    }
    return CallTarget{nullptr, g_default_provider.load(std::memory_order_acquire)};
}

void write_record(const CallTarget& target, std::int32_t level,
                  const std::string& message) noexcept {
    if (target.host != nullptr && target.host->log != nullptr) {
        target.host->log(target.host->context, level, message.data(), message.size());
    } else if (target.host == nullptr) {
        log_to_provider(target.provider, level, message);
    }
}

bool record_enabled(const CallTarget& target, std::int32_t level) noexcept {
    if (target.host != nullptr) {
        return target.host->log_enabled == nullptr ||
               target.host->log_enabled(target.host->context, level) != 0;
    }
    if (target.provider != nullptr) {
        return target.provider->log_enabled == nullptr ||
               target.provider->log_enabled(target.provider->context, level) != 0;
    }
    return false;
}

bool cancel_flag(const CallTarget& target) noexcept {
    return target.host != nullptr && target.host->cancel_requested != nullptr &&
           target.host->cancel_requested(target.host->context) != 0;
}

bool valid_level(int level) {
    if (level < PRAENORA_LOG_TRACE || level > PRAENORA_LOG_CRITICAL) {
        PyErr_Format(PyExc_ValueError, "%d is not a log level (0 to 5)", level);
        return false;
    }
    return true;
}

PyObject* native_log(PyObject*, PyObject* args) {
    int level = 0;
    PyObject* message = nullptr;
    if (!PyArg_ParseTuple(args, "iU:log", &level, &message) || !valid_level(level)) {
        return nullptr;
    }
    std::string text;
    try {
        text = utf8_of(message);
    } catch (const std::exception&) {
        return PyErr_NoMemory();
    }
    const CallTarget target = current_target();
    PyThreadState* saved = PyEval_SaveThread();
    write_record(target, level, text);
    PyEval_RestoreThread(saved);
    Py_RETURN_NONE;
}

PyObject* native_log_enabled(PyObject*, PyObject* args) {
    int level = 0;
    if (!PyArg_ParseTuple(args, "i:log_enabled", &level) || !valid_level(level)) {
        return nullptr;
    }
    const CallTarget target = current_target();
    PyThreadState* saved = PyEval_SaveThread();
    const bool enabled = record_enabled(target, level);
    PyEval_RestoreThread(saved);
    return PyBool_FromLong(enabled ? 1 : 0);
}

PyObject* native_cancel_requested(PyObject*, PyObject*) {
    const CallTarget target = current_target();
    PyThreadState* saved = PyEval_SaveThread();
    const bool requested = cancel_flag(target);
    PyEval_RestoreThread(saved);
    return PyBool_FromLong(requested ? 1 : 0);
}

PyMethodDef kMethods[] = {
    {"log", native_log, METH_VARARGS,
     "log(level, message): a log record of the call that runs on this thread, or of the "
     "provider outside calls."},
    {"log_enabled", native_log_enabled, METH_VARARGS,
     "log_enabled(level): whether a record at that level would be kept."},
    {"cancel_requested", native_cancel_requested, METH_NOARGS,
     "cancel_requested(): whether the call that runs on this thread was asked to end."},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT,
                       "praenora.sdk._native",
                       "The runtime's side of praenora.sdk: records and the cancel flag of the "
                       "call that runs on the calling thread.",
                       -1,
                       kMethods,
                       nullptr,
                       nullptr,
                       nullptr,
                       nullptr};

} // namespace

ScopedCallTarget::ScopedCallTarget(CallTarget target) noexcept
    : target_(target), outer_(t_innermost) {
    t_innermost = this;
}

ScopedCallTarget::~ScopedCallTarget() { t_innermost = outer_; }

void set_default_provider(const praenora_provider_host* provider) noexcept {
    const praenora_provider_host* none = nullptr;
    g_default_provider.compare_exchange_strong(none, provider, std::memory_order_acq_rel);
}

void log_to_provider(const praenora_provider_host* provider, std::int32_t level,
                     const std::string& message) noexcept {
    if (provider != nullptr && provider->log != nullptr) {
        provider->log(provider->context, level, message.data(), message.size());
    }
}

PyObject* create_native_module() {
    PyObject* module = PyModule_Create(&kModule);
    if (module == nullptr) {
        return nullptr;
    }
    if (PyDict_SetItemString(PyImport_GetModuleDict(), "praenora.sdk._native", module) < 0) {
        Py_DECREF(module);
        return nullptr;
    }
    return module;
}

} // namespace praenora::detail::python
