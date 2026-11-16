# The Python bindings: the extension praenora.runtime._bindings, which SWIG generates from
# praenora_runtime.i over the facade (facade.hpp, facade.cpp), and the pure-Python package
# praenora.runtime (python/praenora/runtime). The build tree holds the importable package in
# <build>/python/praenora/runtime: the extension, SWIG's proxy module _bindings.py and the
# package's __init__.py. praenora has no __init__.py: it is an implicit namespace package.
find_package(SWIG 4.0 REQUIRED COMPONENTS python)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
include(UseSWIG)

set(_praenora_package_dir "${PROJECT_BINARY_DIR}/python/praenora/runtime")
set(_praenora_package_source "${PROJECT_SOURCE_DIR}/python/praenora/runtime")

# The .i file includes facade.hpp, so editing the header regenerates the wrapper.
set_property(SOURCE praenora_runtime.i PROPERTY CPLUSPLUS ON)
set_property(SOURCE praenora_runtime.i PROPERTY DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/facade.hpp")
swig_add_library(praenora_python_bindings
    TYPE MODULE
    LANGUAGE python
    OUTPUT_DIR "${_praenora_package_dir}"
    OUTFILE_DIR "${PROJECT_BINARY_DIR}/swig"
    SOURCES praenora_runtime.i facade.hpp facade.cpp)
# -threads: the wrappers of the facade release the GIL around every call into it
# (praenora_runtime.i keeps SWIG's own string and vector code from releasing it).
set_property(TARGET praenora_python_bindings PROPERTY SWIG_COMPILE_OPTIONS -threads)
set_property(TARGET praenora_python_bindings PROPERTY SWIG_USE_TARGET_INCLUDE_DIRECTORIES ON)
target_include_directories(praenora_python_bindings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(praenora_python_bindings PRIVATE
    Praenora::Runtime
    praenora_frontend
    Python3::Module
    nlohmann_json::nlohmann_json)

# The native module is _bindings_native (UseSWIG prefixes the output name with "_" and tells SWIG
# the result, -interface), which SWIG's proxy module _bindings.py imports. It cannot be
# SWIG's default, __bindings: the proxy names it inside class bodies, where Python mangles names
# that begin with two underscores. Its suffix is the interpreter's extension suffix (for example
# .cpython-312-darwin.so), so only that interpreter loads it. On multi-config generators every
# configuration builds into the same package directory (the generator expression keeps the
# per-configuration subdirectory away), so PYTHONPATH stays <build>/python.
set(_praenora_extension_suffix ".so")
if(WIN32)
    set(_praenora_extension_suffix ".pyd")
endif()
if(Python3_SOABI)
    string(PREPEND _praenora_extension_suffix ".${Python3_SOABI}")
endif()
set_target_properties(praenora_python_bindings PROPERTIES
    OUTPUT_NAME bindings_native
    SUFFIX "${_praenora_extension_suffix}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_praenora_package_dir}>"
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
    # Debug builds use the release Python library (SWIG undefines _DEBUG around Python.h).
    target_compile_definitions(praenora_python_bindings PRIVATE SWIG_PYTHON_INTERPRETER_NO_DEBUG)
endif()
praenora_target_warnings(praenora_python_bindings)
# Windows: praenora_runtime.dll and the third-party DLLs go beside the extension, where Python
# looks for the libraries an extension needs.
praenora_copy_runtime_dlls(praenora_python_bindings)

# The package's own files, copied into the package directory at build time.
add_custom_command(
    OUTPUT "${_praenora_package_dir}/__init__.py"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${_praenora_package_source}/__init__.py" "${_praenora_package_dir}/__init__.py"
    DEPENDS "${_praenora_package_source}/__init__.py"
    COMMENT "Copying the praenora.runtime package into ${_praenora_package_dir}"
    VERBATIM)
add_custom_target(praenora_python_package ALL DEPENDS "${_praenora_package_dir}/__init__.py")
add_dependencies(praenora_python_bindings praenora_python_package)

# The wheel (pyproject.toml, scikit-build-core): praenora/runtime holds the extension, _bindings.py,
# __init__.py and the runtime library, which the extension finds beside itself ($ORIGIN or
# @loader_path; on Windows the DLL sits next to it). These rules install only what the build
# makes; the package's own files, __init__.py, come from pyproject.toml's wheel.packages
# (scikit-build-core leaves out a package file that an install rule already put in the wheel).
if(SKBUILD)
    set(_praenora_wheel_dir "praenora/runtime")
    if(APPLE)
        set_target_properties(praenora_python_bindings PROPERTIES INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(praenora_python_bindings PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    # A wheel cannot hold a symbolic link: the soname link (libpraenora_runtime.0.1.dylib, .so.0.1)
    # would be stored as a second copy of the whole library. In a wheel build the library's file
    # name is its soname, so there is one file and no link; the Mach-O current version keeps the
    # full runtime version.
    get_target_property(_praenora_runtime_version praenora_runtime VERSION)
    get_target_property(_praenora_runtime_soversion praenora_runtime SOVERSION)
    set_target_properties(praenora_runtime PROPERTIES
        VERSION "${_praenora_runtime_soversion}"
        MACHO_CURRENT_VERSION "${_praenora_runtime_version}")
    install(TARGETS praenora_python_bindings
        LIBRARY DESTINATION "${_praenora_wheel_dir}"
        RUNTIME DESTINATION "${_praenora_wheel_dir}")
    install(TARGETS praenora_runtime
        LIBRARY DESTINATION "${_praenora_wheel_dir}" NAMELINK_SKIP
        RUNTIME DESTINATION "${_praenora_wheel_dir}"
        ARCHIVE DESTINATION "${_praenora_wheel_dir}" EXCLUDE_FROM_ALL)
    install(FILES "${_praenora_package_dir}/_bindings.py" DESTINATION "${_praenora_wheel_dir}")
endif()
