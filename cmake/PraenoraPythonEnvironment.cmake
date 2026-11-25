# The Python environment of the runtime, built only with PRAENORA_WITH_PYTHON_ENVIRONMENT=ON
# (cmake/PraenoraOptions.cmake): the bundle of praenora-runtime-pyenv (CPython and the packages it
# pins), found as a CMake package and described by its manifest. The bundle is never downloaded
# by the build: it comes from CMAKE_PREFIX_PATH or PraenoraRuntimePyenv_DIR (a developer's presets
# or the CI presets name it).
#
# include(PraenoraPythonEnvironment) stops the configuration when the bundle is not found or does
# not suit this build (find_package() says why: another platform, a macOS deployment target older
# than the bundle's, an older MSVC toolset), and otherwise sets:
#
#   PRAENORA_PYTHON_ENVIRONMENT_DIR         the bundle's directory
#   PRAENORA_PYTHON_ENVIRONMENT_PACKAGE     the bundle's package name
#   PRAENORA_PYTHON_ENVIRONMENT_VERSION     the environment's version
#   PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256 the SHA-256 of the lock that pins its packages
#   PRAENORA_PYTHON_VERSION                 the interpreter's version
#   PRAENORA_PYTHON_ENVIRONMENT_NAME        the environment's name
#   PRAENORA_PYTHON_ENVIRONMENT_MANIFEST    the manifest's file
#   PRAENORA_PYTHON_ENVIRONMENT_EXECUTABLE  the environment's interpreter
#   PRAENORA_PYTHON_ENVIRONMENT_JSON        the environment's object, compact JSON:
#       {"lock_sha256","name","package","version"}
#   PRAENORA_PYTHON_ENGINE_JSON             the engine object of the provider python, compact JSON:
#       {"environment":<PRAENORA_PYTHON_ENVIRONMENT_JSON>,"name":"cpython","version":<python>}
#
# and the imported target praenora_libpython: the environment's libpython, with its include
# directory. It is made from the bundle's variables, never with find_package(Python3).
# One status line says which package was found and where.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraPythonDigest.cmake")

find_package(PraenoraRuntimePyenv 0.1 CONFIG REQUIRED)

set(PRAENORA_PYTHON_ENVIRONMENT_DIR "${PraenoraRuntimePyenv_ROOT}")
set(PRAENORA_PYTHON_ENVIRONMENT_MANIFEST "${PraenoraRuntimePyenv_MANIFEST}")
set(PRAENORA_PYTHON_ENVIRONMENT_EXECUTABLE "${PraenoraRuntimePyenv_PYTHON_EXECUTABLE}")

if(NOT EXISTS "${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST}")
    message(FATAL_ERROR "The Python environment ${PRAENORA_PYTHON_ENVIRONMENT_DIR} has no manifest "
        "(${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST}).")
endif()
file(READ "${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST}" _praenora_pyenv_manifest)
# Every value the engine object and the checks need must be in the manifest.
foreach(_praenora_pyenv_key IN ITEMS
        "name;name" "version;version" "package;package" "lock_sha256;lock;sha256"
        "python_version;python;version")
    list(POP_FRONT _praenora_pyenv_key _praenora_pyenv_variable)
    string(JSON _praenora_pyenv_value ERROR_VARIABLE _praenora_pyenv_error
        GET "${_praenora_pyenv_manifest}" ${_praenora_pyenv_key})
    if(_praenora_pyenv_error OR _praenora_pyenv_value STREQUAL "")
        message(FATAL_ERROR "The manifest ${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST} has no "
            "'${_praenora_pyenv_key}' (${_praenora_pyenv_error}).")
    endif()
    set(_praenora_pyenv_${_praenora_pyenv_variable} "${_praenora_pyenv_value}")
endforeach()
set(PRAENORA_PYTHON_ENVIRONMENT_NAME "${_praenora_pyenv_name}")
set(PRAENORA_PYTHON_ENVIRONMENT_VERSION "${_praenora_pyenv_version}")
set(PRAENORA_PYTHON_ENVIRONMENT_PACKAGE "${_praenora_pyenv_package}")
set(PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256 "${_praenora_pyenv_lock_sha256}")
set(PRAENORA_PYTHON_VERSION "${_praenora_pyenv_python_version}")
string(LENGTH "${PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256}" _praenora_pyenv_lock_length)
if(NOT PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256 MATCHES "^[0-9a-f]+$" OR
   NOT _praenora_pyenv_lock_length EQUAL 64)
    message(FATAL_ERROR "The manifest ${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST} gives the lock "
        "'${PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256}', not 64 lowercase hexadecimal digits.")
endif()
if(NOT PRAENORA_PYTHON_ENVIRONMENT_PACKAGE STREQUAL "${PraenoraRuntimePyenv_PACKAGE_NAME}")
    message(FATAL_ERROR "The manifest ${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST} names the package "
        "${PRAENORA_PYTHON_ENVIRONMENT_PACKAGE}, its CMake package "
        "${PraenoraRuntimePyenv_PACKAGE_NAME}.")
endif()
foreach(_praenora_pyenv_file IN ITEMS "${PraenoraRuntimePyenv_PYTHON_EXECUTABLE}"
        "${PraenoraRuntimePyenv_PYTHON_LIBRARY}")
    if(NOT EXISTS "${_praenora_pyenv_file}")
        message(FATAL_ERROR "The Python environment lacks ${_praenora_pyenv_file}.")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${PraenoraRuntimePyenv_PYTHON_INCLUDE_DIR}")
    message(FATAL_ERROR
        "The Python environment lacks ${PraenoraRuntimePyenv_PYTHON_INCLUDE_DIR}.")
endif()

# The engine object of the provider python, what the fingerprint names, and the environment's
# object in it, which the other providers that run Python add to their own engine objects
# (cmake/PraenoraPythonProvider.cmake): compact JSON, its keys in name order, written as text, so
# that every provider names the environment with the same bytes. Its values are names, versions
# and a digest; one that holds another character than a letter, a digit, '.', '_', '+' or '-'
# is refused, so the text needs no escape.
foreach(_praenora_pyenv_value IN ITEMS "${PRAENORA_PYTHON_ENVIRONMENT_NAME}"
        "${PRAENORA_PYTHON_ENVIRONMENT_VERSION}" "${PRAENORA_PYTHON_ENVIRONMENT_PACKAGE}"
        "${PRAENORA_PYTHON_VERSION}")
    if(NOT _praenora_pyenv_value MATCHES "^[A-Za-z0-9._+-]+$")
        message(FATAL_ERROR "The manifest ${PRAENORA_PYTHON_ENVIRONMENT_MANIFEST} gives "
            "'${_praenora_pyenv_value}', which holds another character than a letter, a digit, "
            "'.', '_', '+' or '-'.")
    endif()
endforeach()
string(CONCAT PRAENORA_PYTHON_ENVIRONMENT_JSON
    "{\"lock_sha256\":\"${PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256}\","
    "\"name\":\"${PRAENORA_PYTHON_ENVIRONMENT_NAME}\","
    "\"package\":\"${PRAENORA_PYTHON_ENVIRONMENT_PACKAGE}\","
    "\"version\":\"${PRAENORA_PYTHON_ENVIRONMENT_VERSION}\"}")
string(CONCAT PRAENORA_PYTHON_ENGINE_JSON
    "{\"environment\":${PRAENORA_PYTHON_ENVIRONMENT_JSON},"
    "\"name\":\"cpython\",\"version\":\"${PRAENORA_PYTHON_VERSION}\"}")

if(NOT TARGET praenora_libpython)
    add_library(praenora_libpython SHARED IMPORTED GLOBAL)
    set_target_properties(praenora_libpython PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PraenoraRuntimePyenv_PYTHON_INCLUDE_DIR}")
    if(WIN32)
        # The import library is python/libs/python312.lib and the DLL python/python312.dll.
        get_filename_component(_praenora_pyenv_dll_name "${PraenoraRuntimePyenv_PYTHON_LIBRARY}"
            NAME_WE)
        set_target_properties(praenora_libpython PROPERTIES
            IMPORTED_IMPLIB "${PraenoraRuntimePyenv_PYTHON_LIBRARY}"
            IMPORTED_LOCATION
                "${PraenoraRuntimePyenv_PYTHON_HOME}/${_praenora_pyenv_dll_name}.dll")
    else()
        set_target_properties(praenora_libpython PROPERTIES
            IMPORTED_LOCATION "${PraenoraRuntimePyenv_PYTHON_LIBRARY}")
    endif()
endif()

message(STATUS
    "Praenora: Python environment ${PRAENORA_PYTHON_ENVIRONMENT_PACKAGE} at "
    "${PRAENORA_PYTHON_ENVIRONMENT_DIR}")

unset(_praenora_pyenv_manifest)
unset(_praenora_pyenv_key)
unset(_praenora_pyenv_variable)
unset(_praenora_pyenv_value)
unset(_praenora_pyenv_error)
unset(_praenora_pyenv_name)
unset(_praenora_pyenv_version)
unset(_praenora_pyenv_package)
unset(_praenora_pyenv_lock_sha256)
unset(_praenora_pyenv_python_version)
unset(_praenora_pyenv_file)
unset(_praenora_pyenv_lock_length)
unset(_praenora_pyenv_dll_name)
