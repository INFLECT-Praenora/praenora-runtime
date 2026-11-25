// How the Python core reaches CPython's C API: the one include of Python.h in the repository, and
// the two guards every Python-facing source of the core uses, for the interpreter's lock and for
// object references.
//
// Python.h is included with PY_SSIZE_T_CLEAN and, in an MSVC Debug build, without _DEBUG: the
// environment has only its release library, which pyconfig.h then links, and _DEBUG would make
// the headers describe a debug interpreter's objects. The C runtime's headers are included first
// with _DEBUG, so that their settings stay those of the rest of the build.
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_PYTHON_INCLUDE_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_PYTHON_INCLUDE_HPP

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdefs.h>
#include <yvals.h>
#define PRAENORA_PYTHON_RESTORE_DEBUG
#undef _DEBUG
#endif

#include <Python.h>

#ifdef PRAENORA_PYTHON_RESTORE_DEBUG
#define _DEBUG 1
#undef PRAENORA_PYTHON_RESTORE_DEBUG
#endif

namespace praenora::detail::python {

// Gives the calling thread a Python thread state of its own, made at the thread's first call and
// kept as long as the thread (interpreter.cpp), so that what Python keeps per thread (context
// variables, such as numpy's error state and decimal's context, and threading.local values) lasts
// from one call on the thread to the next. Only after the interpreter was started, the lock not
// held.
void keep_thread_state() noexcept;

// Holds the interpreter's lock for a scope, on any thread (PyGILState_Ensure), with the thread's
// kept thread state (keep_thread_state()). Only after the interpreter was started. Declare it
// before the references of the scope, so that they are released while the lock is still held.
class Gil {
public:
    Gil() noexcept : state_(ensure()) {}
    ~Gil() { PyGILState_Release(state_); }
    Gil(const Gil&) = delete;
    Gil& operator=(const Gil&) = delete;

private:
    static PyGILState_STATE ensure() noexcept {
        keep_thread_state();
        return PyGILState_Ensure();
    }

    PyGILState_STATE state_;
};

// An owned reference (a new one, or null), released when it goes out of scope; the interpreter's
// lock must be held then.
class Ref {
public:
    Ref() noexcept = default;
    explicit Ref(PyObject* owned) noexcept : object_(owned) {}
    ~Ref() { Py_XDECREF(object_); }
    Ref(Ref&& other) noexcept : object_(other.release()) {}
    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            Py_XDECREF(object_);
            object_ = other.release();
        }
        return *this;
    }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;

    // A new reference to a borrowed object.
    static Ref borrowed(PyObject* object) noexcept {
        Py_XINCREF(object);
        return Ref(object);
    }

    PyObject* get() const noexcept { return object_; }
    explicit operator bool() const noexcept { return object_ != nullptr; }
    PyObject* release() noexcept {
        PyObject* object = object_;
        object_ = nullptr;
        return object;
    }

private:
    PyObject* object_ = nullptr;
};

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_PYTHON_INCLUDE_HPP
