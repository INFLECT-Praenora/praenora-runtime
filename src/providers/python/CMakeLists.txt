# What runs Python (docs/specs/python-service.md), built only with
# PRAENORA_WITH_PYTHON_ENVIRONMENT, after include(PraenoraPythonEnvironment) found the environment
# (the imported target praenora_libpython and the PRAENORA_PYTHON_* variables):
#   - the Python core, praenora_python_core (core/): the one library of the runtime that embeds
#     CPython and links the environment's libpython;
#   - praenora_python_provider_support (common/), STATIC: the loader of the core and the service
#     glue, which every provider that runs Python links (no Python header, no Python link);
#   - the provider python, praenora_provider_python (provider/), which offers praenora.python@0.0,
#     built with praenora_add_python_provider() (cmake/PraenoraPythonProvider.cmake).
#
# The core is a SHARED library, praenora-python-core, whose only exported symbol is its entry
# point praenora_python_core_get_api (python_core.h). It links neither the runtime nor its objects:
# a module host holds the runtime's objects itself, and the core must not bring a second copy into
# its process. It compiles the runtime's SHA-256 (src/runtime/core/sha256.cpp), which needs no other
# part of the runtime, with hidden symbols like the rest of its code.
#
# It goes where the build tree's runtime library looks for the provider python
# (<directory of the runtime library>/praenora/providers/python; on Windows
# <runtime DLL directory>/../lib/praenora/providers/python), with the praenora.sdk sources beside it
# in python/praenora/sdk/, and so do the provider python's library, its manifest and its
# environment file python-environment.json: the layout of an installation, whose core finds
# libpython in ../../python-environment/python/lib (INSTALL_RPATH). The build tree's core finds
# the bundle's. A tree configured without the option removes that directory (the root
# CMakeLists.txt).
#
# With the install rules, all of it is installed into ${CMAKE_INSTALL_LIBDIR}/praenora/providers/
# python, in the default component: the provider's library and manifest, the installation's
# environment file (which names ../../python-environment and the core's file name), the core (on
# Windows its DLL, without its import library, which nothing links) and the build's copy of
# praenora.sdk, the files whose digest the core carries. cmake/PraenoraInstall.cmake installs the
# environment itself into ${CMAKE_INSTALL_LIBDIR}/praenora/python-environment.
#
# Its compile definitions:
#   PRAENORA_PYTHON_ENVIRONMENT_PACKAGE, PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256
#       the environment it was built against, which start() requires
#   PRAENORA_PYTHON_SDK_DIGEST
#       the directory digest of python/praenora/sdk (cmake/PraenoraPythonDigest.cmake), which the
#       first start() checks
#   PRAENORA_PYTHON_CORE_BUILD_ID
#       its build identity (_praenora_python_core_build_id() below), which build_id() reports; the
#       target properties PRAENORA_PYTHON_CORE_BUILD_ID and PRAENORA_PYTHON_SDK_DIGEST hold the
#       same values, and PRAENORA_PYTHON_CORE_DIR the core's directory (it may contain $<CONFIG>).
# praenora_python_provider_support gives the providers that link it the environment's package and
# lock and the core's build identity, under the same names, which its loader requires at start.

# _praenora_python_core_build_id(<out_var> SDK_DIGEST <digest> FILES <file>...)
#
# The build identity of the core: 64 lowercase hexadecimal digits, the SHA-256 of the two lines
#   sources <directory digest of FILES, relative to the source tree>\n
#   praenora.sdk <SDK_DIGEST>\n
# FILES are the files the core is built from (its sources, and the headers it compiles against)
# and those of what is built with it and must agree with it (the loader of the providers that run
# Python belongs there too): each is added to CMAKE_CONFIGURE_DEPENDS, so an edit configures the
# build again and the identity follows.
function(_praenora_python_core_build_id out_var)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SDK_DIGEST" "FILES")
    if(NOT arg_SDK_DIGEST OR NOT arg_FILES)
        message(FATAL_ERROR "_praenora_python_core_build_id: give SDK_DIGEST and FILES")
    endif()
    praenora_python_directory_digest(sources_digest ROOT "${PROJECT_SOURCE_DIR}"
        FILES ${arg_FILES})
    string(SHA256 identity "sources ${sources_digest}\npraenora.sdk ${arg_SDK_DIGEST}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${arg_FILES})
    set(${out_var} "${identity}" PARENT_SCOPE)
endfunction()

# _praenora_python_check_identity_files(FILES <file>... INCLUDE_DIRECTORIES <dir>...)
#
# Stops the configuration when a C or C++ file of FILES includes a file of the source tree,
# directly or through another, that is not among FILES: an edit of such a header (a limit, a
# struct's layout) would change the core without changing its build identity. An include is
# looked for as the compiler looks for it: "name" in the including file's directory and then in
# INCLUDE_DIRECTORIES, <praenora/...> in INCLUDE_DIRECTORIES only; other <...> includes (the
# standard library, Python, nlohmann/json) are not the source tree's, and a name found in none of
# the directories is not either (a generated header).
function(_praenora_python_check_identity_files)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "FILES;INCLUDE_DIRECTORIES")
    set(listed "")
    foreach(file IN LISTS arg_FILES)
        cmake_path(SET normal NORMALIZE "${file}")
        list(APPEND listed "${normal}")
    endforeach()
    set(pending ${listed})
    set(seen ${listed})
    while(pending)
        list(POP_FRONT pending file)
        if(NOT file MATCHES "\\.(h|hpp|c|cpp)$")
            continue()
        endif()
        file(STRINGS "${file}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]" ENCODING UTF-8)
        get_filename_component(here "${file}" DIRECTORY)
        foreach(line IN LISTS lines)
            if(line MATCHES "include[ \t]*\"([^\"]+)\"")
                set(directories "${here}" ${arg_INCLUDE_DIRECTORIES})
            elseif(line MATCHES "include[ \t]*<(praenora/[^>]+)>")
                set(directories ${arg_INCLUDE_DIRECTORIES})
            else()
                continue()
            endif()
            set(name "${CMAKE_MATCH_1}")
            set(found "")
            foreach(directory IN LISTS directories)
                if(EXISTS "${directory}/${name}" AND NOT IS_DIRECTORY "${directory}/${name}")
                    cmake_path(SET found NORMALIZE "${directory}/${name}")
                    break()
                endif()
            endforeach()
            if(found STREQUAL "")
                continue()
            endif()
            if(NOT found IN_LIST listed)
                message(FATAL_ERROR "The Python core's build identity does not cover ${found}, "
                    "which ${file} includes: add it to _praenora_python_identity_files "
                    "(src/providers/python/CMakeLists.txt)")
            endif()
            if(NOT found IN_LIST seen)
                list(APPEND seen "${found}")
                list(APPEND pending "${found}")
            endif()
        endforeach()
    endwhile()
endfunction()

praenora_build_tree_provider_dir(_praenora_python_providers_dir)
set(_praenora_python_core_dir "${_praenora_python_providers_dir}/python")

set(_praenora_python_core_sources
    core/python_core.h
    core/python_include.hpp
    core/core.cpp
    core/digest.hpp
    core/digest.cpp
    core/documents.hpp
    core/documents.cpp
    core/environment.hpp
    core/environment.cpp
    core/interpreter.hpp
    core/interpreter.cpp
    core/native_module.hpp
    core/native_module.cpp
    core/objects.hpp
    core/objects.cpp)
set(_praenora_python_runtime_sources
    "${PROJECT_SOURCE_DIR}/src/runtime/core/sha256.hpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/core/sha256.cpp")
# The headers of other directories that the core compiles against: the service's (its document
# limit), the provider interface's (the provider host's layout) and the module SDK's C headers.
set(_praenora_python_interface_headers
    "${PROJECT_SOURCE_DIR}/include/praenora/sdk/python.h"
    "${PROJECT_SOURCE_DIR}/include/praenora/sdk/abi.h"
    "${PROJECT_SOURCE_DIR}/include/praenora/sdk/status.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/provider/provider_abi.h")
# The loader of the core and the service glue (praenora_python_provider_support), which must agree
# with the core: what it passes start() and how it reads the environment file.
set(_praenora_python_support_sources
    common/environment_files.hpp
    common/loader.hpp
    common/loader.cpp
    common/service_glue.hpp
    common/service_glue.cpp)
# Where the includes of the core and of the support library are found: their own directories, the
# runtime's (core/sha256.hpp), the provider interface's and the public headers.
set(_praenora_python_include_directories
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${PROJECT_SOURCE_DIR}/src/runtime"
    "${PROJECT_SOURCE_DIR}/src/runtime/provider"
    "${PROJECT_SOURCE_DIR}/include")

# The files of the build identity: the core's sources, the runtime's that it compiles, the headers
# it compiles against, the support library's sources, and the build files that decide how they are
# built and how the environment file is written (this file, the copy script,
# cmake/PraenoraPythonProvider.cmake). Whatever else must agree with the core goes here too;
# _praenora_python_check_identity_files() makes sure that every header of the source tree that they
# include is among them.
list(TRANSFORM _praenora_python_core_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE _praenora_python_identity_files)
list(TRANSFORM _praenora_python_support_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE _praenora_python_support_paths)
list(APPEND _praenora_python_identity_files
    ${_praenora_python_runtime_sources}
    ${_praenora_python_interface_headers}
    ${_praenora_python_support_paths}
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/copy_python_sources.cmake"
    "${PROJECT_SOURCE_DIR}/cmake/PraenoraPythonProvider.cmake")

# praenora.sdk: every file below python/praenora/sdk but those below __pycache__, as the digest
# takes them. A file added or removed configures the build again (CONFIGURE_DEPENDS), and so does
# an edit (CMAKE_CONFIGURE_DEPENDS).
set(_praenora_sdk_root "${PROJECT_SOURCE_DIR}/python/praenora/sdk")
file(GLOB_RECURSE _praenora_sdk_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
    RELATIVE "${_praenora_sdk_root}" "${_praenora_sdk_root}/*")
list(FILTER _praenora_sdk_files EXCLUDE REGEX "(^|/)__pycache__/")
list(SORT _praenora_sdk_files)
praenora_python_directory_digest(_praenora_sdk_digest ROOT "${_praenora_sdk_root}"
    FILES ${_praenora_sdk_files})
list(TRANSFORM _praenora_sdk_files PREPEND "${_praenora_sdk_root}/"
    OUTPUT_VARIABLE _praenora_sdk_paths)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_praenora_sdk_paths})

_praenora_python_check_identity_files(
    FILES ${_praenora_python_identity_files}
    INCLUDE_DIRECTORIES ${_praenora_python_include_directories})
_praenora_python_core_build_id(_praenora_python_core_build_id
    SDK_DIGEST "${_praenora_sdk_digest}"
    FILES ${_praenora_python_identity_files})

add_library(praenora_python_core SHARED
    ${_praenora_python_core_sources}
    ${_praenora_python_runtime_sources})
target_include_directories(praenora_python_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(praenora_python_core PRIVATE
    praenora_provider_interface
    praenora_libpython
    nlohmann_json::nlohmann_json)
target_compile_definitions(praenora_python_core PRIVATE
    PRAENORA_PYTHON_CORE_IMPLEMENTATION
    "PRAENORA_PYTHON_CORE_BUILD_ID=\"${_praenora_python_core_build_id}\""
    "PRAENORA_PYTHON_SDK_DIGEST=\"${_praenora_sdk_digest}\""
    "PRAENORA_PYTHON_ENVIRONMENT_PACKAGE=\"${PRAENORA_PYTHON_ENVIRONMENT_PACKAGE}\""
    "PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256=\"${PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256}\"")
get_filename_component(_praenora_libpython_dir "${PraenoraRuntimePyenv_PYTHON_LIBRARY}" DIRECTORY)
set_target_properties(praenora_python_core PROPERTIES
    PREFIX ""
    OUTPUT_NAME "praenora-python-core"
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${_praenora_python_core_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${_praenora_python_core_dir}"
    BUILD_RPATH "${_praenora_libpython_dir}"
    PRAENORA_PYTHON_CORE_BUILD_ID "${_praenora_python_core_build_id}"
    PRAENORA_PYTHON_SDK_DIGEST "${_praenora_sdk_digest}"
    PRAENORA_PYTHON_CORE_DIR "${_praenora_python_core_dir}")
if(APPLE)
    set_target_properties(praenora_python_core PROPERTIES
        INSTALL_RPATH "@loader_path/../../python-environment/python/lib")
    target_link_options(praenora_python_core PRIVATE
        "LINKER:-exported_symbol,_praenora_python_core_get_api")
elseif(NOT WIN32)
    set_target_properties(praenora_python_core PROPERTIES
        INSTALL_RPATH "$ORIGIN/../../python-environment/python/lib")
    set(_praenora_python_core_map "${CMAKE_CURRENT_BINARY_DIR}/praenora_python_core.map")
    file(CONFIGURE OUTPUT "${_praenora_python_core_map}" NEWLINE_STYLE UNIX CONTENT [=[{
  global:
    praenora_python_core_get_api;
  local:
    *;
};
]=])
    target_link_options(praenora_python_core PRIVATE
        "LINKER:--version-script=${_praenora_python_core_map}")
    set_property(TARGET praenora_python_core APPEND PROPERTY LINK_DEPENDS
        "${_praenora_python_core_map}")
endif()
praenora_target_warnings(praenora_python_core)

# The copy of praenora.sdk beside the core (python/praenora/sdk), made again when a file of it
# changes: exactly the files of its digest.
set(_praenora_sdk_list "${CMAKE_CURRENT_BINARY_DIR}/praenora-sdk-files.txt")
list(JOIN _praenora_sdk_files "\n" _praenora_sdk_lines)
file(CONFIGURE OUTPUT "${_praenora_sdk_list}" CONTENT "${_praenora_sdk_lines}\n"
    NEWLINE_STYLE UNIX)
set(_praenora_sdk_stamp "${CMAKE_CURRENT_BINARY_DIR}/praenora-sdk-$<CONFIG>.stamp")
add_custom_command(OUTPUT "${_praenora_sdk_stamp}"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${_praenora_sdk_root}"
        "-DDESTINATION=${_praenora_python_core_dir}/python/praenora/sdk"
        "-DLIST=${_praenora_sdk_list}"
        "-DSTAMP=${_praenora_sdk_stamp}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_python_sources.cmake"
    DEPENDS ${_praenora_sdk_paths} "${_praenora_sdk_list}"
        "${CMAKE_CURRENT_SOURCE_DIR}/copy_python_sources.cmake"
    COMMENT "Copying praenora.sdk beside the Python core"
    VERBATIM)
add_custom_target(praenora_python_core_sdk DEPENDS "${_praenora_sdk_stamp}")
add_dependencies(praenora_python_core praenora_python_core_sdk)

# The loader of the core and the service glue, linked into every provider that runs Python: no
# Python header and no Python link (the core is loaded at start, by its absolute path). It is
# compiled with the environment's package and lock and the core's build identity, and gives them to
# the providers that link it. Static and position-independent, with hidden symbols: the providers'
# export lists keep them in.
add_library(praenora_python_provider_support STATIC ${_praenora_python_support_sources})
target_include_directories(praenora_python_provider_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_link_libraries(praenora_python_provider_support
    PUBLIC praenora_provider_interface
    PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_definitions(praenora_python_provider_support PUBLIC
    "PRAENORA_PYTHON_CORE_BUILD_ID=\"${_praenora_python_core_build_id}\""
    "PRAENORA_PYTHON_ENVIRONMENT_PACKAGE=\"${PRAENORA_PYTHON_ENVIRONMENT_PACKAGE}\""
    "PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256=\"${PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256}\"")
set_target_properties(praenora_python_provider_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
praenora_target_warnings(praenora_python_provider_support)

# The provider python, beside the core. Its engine object is PRAENORA_PYTHON_ENGINE_JSON: the
# interpreter and the environment, which the fingerprint names.
set(_praenora_python_install "")
set(_praenora_python_install_dir "${CMAKE_INSTALL_LIBDIR}/praenora/providers/python")
if(PRAENORA_INSTALL_RULES)
    set(_praenora_python_install INSTALL_DESTINATION "${_praenora_python_install_dir}")
endif()
praenora_add_python_provider(praenora_provider_python
    NAME python
    VERSION "${PRAENORA_RUNTIME_VERSION}"
    SERVICES praenora.python@0.0
    SOURCES provider/python_provider.cpp
    OUTPUT_DIRECTORY "${_praenora_python_core_dir}"
    ${_praenora_python_install})
praenora_target_warnings(praenora_provider_python)

# The core and its copy of praenora.sdk, installed beside the provider. install(TARGETS) gives the
# installed core its INSTALL_RPATH, relative to itself. The import library of the core's DLL goes
# to a component of its own, which no installation asks for.
if(PRAENORA_INSTALL_RULES)
    install(TARGETS praenora_python_core
        LIBRARY DESTINATION "${_praenora_python_install_dir}"
        RUNTIME DESTINATION "${_praenora_python_install_dir}"
        ARCHIVE DESTINATION "${_praenora_python_install_dir}"
            COMPONENT PraenoraPythonCoreImportLibrary EXCLUDE_FROM_ALL)
    install(DIRECTORY "${_praenora_python_core_dir}/python/"
        DESTINATION "${_praenora_python_install_dir}/python"
        PATTERN "__pycache__" EXCLUDE)
endif()
