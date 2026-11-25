// praenora.sdk._native, the module the core creates for praenora.sdk (log, log_enabled and
// cancel_requested), and where they act: the host of the call that runs on the calling thread, or
// a provider host outside calls.
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_NATIVE_MODULE_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_NATIVE_MODULE_HPP

#include "python_include.hpp"

#include "provider_abi.h"

#include <cstdint>
#include <string>

namespace praenora::detail::python {

// Where the records and the cancel flag of Python code go: a module's host API when there is one,
// otherwise a provider host. Both are borrowed; the core checked their struct_size.
struct CallTarget {
    const praenora_host_api* host = nullptr;
    const praenora_provider_host* provider = nullptr;
};

// The target of the calls on this thread while the object lives; the innermost one is used. The
// targets of a thread are a chain of these objects, which live on its stack: the thread keeps only
// a pointer to the innermost, which needs no destruction when the thread ends (Python code that a
// thread's end runs, as its thread state is cleared, still finds no target).
class ScopedCallTarget {
public:
    explicit ScopedCallTarget(CallTarget target) noexcept;
    ~ScopedCallTarget();
    ScopedCallTarget(const ScopedCallTarget&) = delete;
    ScopedCallTarget& operator=(const ScopedCallTarget&) = delete;

    const CallTarget& target() const noexcept { return target_; }

private:
    CallTarget target_;
    const ScopedCallTarget* outer_;
};

// The provider host of the records written outside calls (by a thread that Python code started,
// for example): the first start's. Set once.
void set_default_provider(const praenora_provider_host* provider) noexcept;

// A record at `level` to the provider host itself, as the core's own warnings are.
void log_to_provider(const praenora_provider_host* provider, std::int32_t level,
                     const std::string& message) noexcept;

// Creates praenora.sdk._native and puts it in sys.modules: a new reference, or null with a Python
// exception set. Needs the interpreter's lock.
PyObject* create_native_module();

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_NATIVE_MODULE_HPP
