// SWIG interface of the Python extension praenora.runtime._bindings. It wraps the facade of
// facade.hpp (namespace praenora::python), whose parameters and results are strings, integers and
// booleans, and nothing else. The pure-Python package praenora.runtime is its only client.
//
// SWIG runs with -threads, and every call into the facade releases the GIL while the C++ code
// runs, so other Python threads go on (for example one that calls status() or request_stop()
// during run()). SWIG's own code for strings and string vectors keeps the GIL: it calls the
// Python C API (StringVector's iterators create Python strings, its slices read slice objects).
%module(package="praenora.runtime") _bindings

%{
#include "facade.hpp"
%}

%nothreadallow;

%include <std_string.i>
%include <std_vector.i>

// The vector's destructor too, which SWIG adds to the class later.
%feature("nothreadallow") std::vector<std::string>::~vector;
%template(StringVector) std::vector<std::string>;

%clearnothreadallow;

// C++ exceptions become Python exceptions: std::bad_alloc MemoryError, any other exception
// RuntimeError with its message (the facade's errors carry the prefix PRAENORA_ERROR_JSON:, which
// the package turns into PraenoraError). With -threads, the GIL is released only inside $action
// (whose scope ends, taking the GIL back, before a handler runs); each handler still takes it
// explicitly, so the Python error is always set while it is held.
%exception {
    try {
        $action
    } catch (const std::bad_alloc&) {
        SWIG_PYTHON_THREAD_BEGIN_BLOCK;
        PyErr_NoMemory();
        SWIG_PYTHON_THREAD_END_BLOCK;
        SWIG_fail;
    } catch (const std::exception& error) {
        SWIG_PYTHON_THREAD_BEGIN_BLOCK;
        PyErr_SetString(PyExc_RuntimeError, error.what());
        SWIG_PYTHON_THREAD_END_BLOCK;
        SWIG_fail;
    } catch (...) {
        SWIG_PYTHON_THREAD_BEGIN_BLOCK;
        PyErr_SetString(PyExc_RuntimeError, "unknown C++ exception");
        SWIG_PYTHON_THREAD_END_BLOCK;
        SWIG_fail;
    }
}

%include "facade.hpp"
