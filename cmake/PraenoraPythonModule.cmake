# praenora_add_python_module(<target>
#     NAME <name> VERSION <semver> CAPABILITIES <id@version>... CLASS <package.module:Class>
#     PYTHON_SOURCES <file-or-directory>... [PYTHON_ROOT <dir>] [SELF_TEST]
#     [REQUIRES_SERVICES <name>@<major>.<minor>...] [OPTIONAL_SERVICES <name>@<major>.<minor>...]
#     [MANIFEST_TEMPLATE <file>] [TEST_VECTORS <file>] [OUTPUT_DIRECTORY <dir>]
#     [INSTALL_DESTINATION <dir> [INSTALL_COMPONENT <component>] [INSTALL_EXCLUDE_FROM_ALL]])
#
# Builds a Python module: a class derived from praenora.sdk.Module, its Python sources, and a
# generated module library that runs the class through the runtime service praenora.python@0
# (docs/specs/python-service.md). Installed with the module SDK (find_package(Praenora COMPONENTS
# ModuleSDK)); it needs no Python and no Python environment, only a C++17 compiler and CMake 3.21
# or later.
#
# - CLASS names the module's class, "package.module:Class" (the attribute may be dotted), each part
#   an ASCII Python identifier. Its top-level package (or module) must be among the sources, and
#   the sources may hold nothing beside it: their one top-level entry below PYTHON_ROOT is <top>/
#   or <top>.py, since the Python modules of a process share one import path.
# - PYTHON_SOURCES are files and directories (every file below a directory but those below
#   __pycache__), relative to PYTHON_ROOT (default: the current source directory) or absolute below
#   it. The build copies them, keeping their paths below PYTHON_ROOT, into
#   <output directory>/<name>.python/, which then holds them and nothing else, and the library
#   imports the class from there. Their directory digest (praenora_python_directory_digest()) is
#   computed when the project is configured, and the project is configured again when one of them
#   changes (a directory's files are globbed with CONFIGURE_DEPENDS).
# - The library is praenora_add_module()'s (cmake/PraenoraModule.cmake), with one generated source,
#   <current binary dir>/<target>_python_module.cpp, which expands
#   PRAENORA_SDK_DEFINE_PYTHON_MODULE() of praenora/sdk/python_module.hpp with CLASS, the digest
#   and SELF_TEST; the digest is thus part of the library's bytes, and so of its SHA-256 in the run
#   fingerprint. It requires praenora.python@0.0, which comes first in the manifest's
#   requires.services, before REQUIRES_SERVICES and OPTIONAL_SERVICES (which must not name
#   praenora.python). On Linux it links ${CMAKE_DL_LIBS} (dladdr()).
# - SELF_TEST: the table's self_test calls the class method self_test(host). Without a
#   MANIFEST_TEMPLATE the generated manifest then declares "features": {"self_test": true}; with
#   one, the configured manifest must declare it, and without SELF_TEST it must not: a configuration
#   error otherwise, as is a configured manifest that is not JSON.
# - NAME, VERSION, CAPABILITIES, MANIFEST_TEMPLATE, TEST_VECTORS, OUTPUT_DIRECTORY and the INSTALL_*
#   arguments are praenora_add_module()'s. With INSTALL_DESTINATION the build's copy of the sources
#   (the files whose digest the library carries) is installed too, into
#   <destination>/<name>.python/, beside the library and the manifest.
# - The target properties PRAENORA_PYTHON_MODULE_DIR (the copy of the sources; it may contain
#   $<CONFIG>) and PRAENORA_PYTHON_MODULE_SOURCES_SHA256 (their digest) are set, besides those of
#   praenora_add_module().
#
# Run as a script (cmake -DPRAENORA_PYTHON_COPY_ROOT=<dir> -DPRAENORA_PYTHON_COPY_DESTINATION=<dir>
# -DPRAENORA_PYTHON_COPY_LIST=<file> -DPRAENORA_PYTHON_COPY_STAMP=<file> -P <this file>), this file
# is the build step that copies the sources: it empties the destination, copies the files that the
# list names (one path relative to the root per line) byte for byte, then touches the stamp.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraModule.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraPythonDigest.cmake")

function(praenora_add_python_module target)
    set(where "praenora_add_python_module(${target})")
    if(CMAKE_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "${where}: needs CMake 3.21 or later (this is ${CMAKE_VERSION})")
    endif()
    set(one_value_keywords NAME VERSION CLASS PYTHON_ROOT MANIFEST_TEMPLATE TEST_VECTORS
        OUTPUT_DIRECTORY INSTALL_DESTINATION INSTALL_COMPONENT)
    cmake_parse_arguments(PARSE_ARGV 1 arg "SELF_TEST;INSTALL_EXCLUDE_FROM_ALL"
        "${one_value_keywords}" "CAPABILITIES;PYTHON_SOURCES;REQUIRES_SERVICES;OPTIONAL_SERVICES")
    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${where}: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${where}: no value given for ${arg_KEYWORDS_MISSING_VALUES}")
    endif()
    foreach(required IN ITEMS NAME VERSION CAPABILITIES CLASS PYTHON_SOURCES)
        if(NOT DEFINED arg_${required})
            message(FATAL_ERROR "${where}: ${required} is required")
        endif()
    endforeach()
    set(identifier "[A-Za-z_][A-Za-z0-9_]*")
    if(NOT arg_CLASS MATCHES "^${identifier}(\\.${identifier})*:${identifier}(\\.${identifier})*$")
        message(FATAL_ERROR "${where}: CLASS '${arg_CLASS}' is not package.module:Class, each part "
            "an ASCII Python identifier")
    endif()
    foreach(kind IN ITEMS REQUIRES_SERVICES OPTIONAL_SERVICES)
        foreach(service IN LISTS arg_${kind})
            if(service MATCHES "^praenora\\.python@")
                message(FATAL_ERROR "${where}: ${kind} names ${service}; every Python module "
                    "requires praenora.python@0.0, which this function adds")
            endif()
        endforeach()
    endforeach()

    # The Python sources: the files, their paths below the root, and their digest.
    if(DEFINED arg_PYTHON_ROOT)
        get_filename_component(root "${arg_PYTHON_ROOT}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        set(root "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(files "")
    foreach(entry IN LISTS arg_PYTHON_SOURCES)
        get_filename_component(absolute "${entry}" ABSOLUTE BASE_DIR "${root}")
        if(IS_DIRECTORY "${absolute}")
            file(GLOB_RECURSE found CONFIGURE_DEPENDS LIST_DIRECTORIES false "${absolute}/*")
            list(FILTER found EXCLUDE REGEX "/__pycache__/")
            list(APPEND files ${found})
        elseif(EXISTS "${absolute}")
            list(APPEND files "${absolute}")
        else()
            message(FATAL_ERROR "${where}: the Python source ${entry} does not exist")
        endif()
    endforeach()
    if(NOT files)
        message(FATAL_ERROR "${where}: PYTHON_SOURCES names no file")
    endif()
    # The digest refuses a file that is not below the root, and a symbolic link.
    praenora_python_directory_digest(digest ROOT "${root}" FILES ${files})
    set(relative_files "")
    foreach(file IN LISTS files)
        file(RELATIVE_PATH relative "${root}" "${file}")
        list(APPEND relative_files "${relative}")
    endforeach()
    list(REMOVE_DUPLICATES relative_files)
    list(SORT relative_files)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${files})
    # The sources hold one top-level entry, the top-level package of CLASS (<top>/ or <top>.py):
    # every Python module of a process shares one import path, on which another top-level name
    # (a helpers.py, a directory, which is a namespace package) could be another module's.
    string(REGEX MATCH "^[^.:]+" top "${arg_CLASS}")
    set(top_entry "")
    set(entries "")
    foreach(relative IN LISTS relative_files)
        if(relative MATCHES "^([^/]+)/")
            set(entry "${CMAKE_MATCH_1}/")
        else()
            set(entry "${relative}")
        endif()
        list(APPEND entries "${entry}")
        if(top_entry STREQUAL "" AND (entry STREQUAL "${top}/" OR entry STREQUAL "${top}.py"))
            set(top_entry "${entry}")
        endif()
    endforeach()
    if(top_entry STREQUAL "")
        message(FATAL_ERROR "${where}: the top-level package of CLASS '${arg_CLASS}' is not among "
            "the PYTHON_SOURCES: neither ${top}/ nor ${top}.py is below ${root}")
    endif()
    list(REMOVE_DUPLICATES entries)
    foreach(entry IN LISTS entries)
        if(NOT entry STREQUAL top_entry)
            message(FATAL_ERROR "${where}: PYTHON_SOURCES hold ${entry} beside ${top_entry}, the "
                "top-level package of CLASS '${arg_CLASS}'; a Python module's sources hold that "
                "package only, since the Python modules of a process share one import path, on "
                "which another top-level name could be another module's (move ${entry} into "
                "${top_entry})")
        endif()
    endforeach()

    # The library's one source.
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_python_module.cpp")
    if(arg_SELF_TEST)
        set(_praenora_python_self_test 1)
    else()
        set(_praenora_python_self_test 0)
    endif()
    set(_praenora_python_class "${arg_CLASS}")
    set(_praenora_python_digest "${digest}")
    file(CONFIGURE OUTPUT "${generated}" @ONLY NEWLINE_STYLE UNIX CONTENT [=[
// Generated by praenora_add_python_module() for the module @arg_NAME@: do not edit. Its Python
// sources are in <library directory>/@arg_NAME@.python, with the directory digest below.
#include <praenora/sdk/python_module.hpp>

PRAENORA_SDK_DEFINE_PYTHON_MODULE("@_praenora_python_class@",
                                  "@_praenora_python_digest@",
                                  @_praenora_python_self_test@)
]=])

    # The manifest: a template, or the generated one, which declares the self test with SELF_TEST.
    set(manifest_option "")
    if(DEFINED arg_MANIFEST_TEMPLATE)
        set(manifest_option MANIFEST_TEMPLATE "${arg_MANIFEST_TEMPLATE}")
    elseif(arg_SELF_TEST)
        set(manifest_option INTERNAL_FEATURES_JSON "{ \"self_test\": true }")
    endif()

    set(options "")
    foreach(keyword IN ITEMS TEST_VECTORS OUTPUT_DIRECTORY INSTALL_DESTINATION INSTALL_COMPONENT)
        if(DEFINED arg_${keyword})
            list(APPEND options ${keyword} "${arg_${keyword}}")
        endif()
    endforeach()
    if(arg_INSTALL_EXCLUDE_FROM_ALL)
        list(APPEND options INSTALL_EXCLUDE_FROM_ALL)
    endif()
    if(DEFINED arg_OPTIONAL_SERVICES)
        list(APPEND options OPTIONAL_SERVICES ${arg_OPTIONAL_SERVICES})
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND options LINK_LIBRARIES ${CMAKE_DL_LIBS})
    endif()
    praenora_add_module(${target}
        NAME "${arg_NAME}"
        VERSION "${arg_VERSION}"
        CAPABILITIES ${arg_CAPABILITIES}
        SOURCES "${generated}"
        REQUIRES_SERVICES praenora.python@0.0 ${arg_REQUIRES_SERVICES}
        ${manifest_option}
        ${options})

    # The configured manifest is JSON, and declares the self test exactly when the table has one.
    get_target_property(configured ${target} PRAENORA_MODULE_CONFIGURED_MANIFEST)
    file(READ "${configured}" manifest_text)
    string(JSON manifest_type ERROR_VARIABLE json_error TYPE "${manifest_text}")
    if(json_error)
        message(FATAL_ERROR "${where}: the manifest '${configured}' configured from "
            "MANIFEST_TEMPLATE '${arg_MANIFEST_TEMPLATE}' is not JSON: ${json_error}")
    endif()
    string(JSON declared ERROR_VARIABLE json_error GET "${manifest_text}" features self_test)
    if(json_error)
        set(declared OFF)
    endif()
    if(arg_SELF_TEST AND NOT declared)
        message(FATAL_ERROR "${where}: SELF_TEST needs the manifest to declare \"features\": "
            "{\"self_test\": true}, and the one configured from MANIFEST_TEMPLATE "
            "'${arg_MANIFEST_TEMPLATE}' does not")
    endif()
    if(NOT arg_SELF_TEST AND declared)
        message(FATAL_ERROR "${where}: the manifest configured from MANIFEST_TEMPLATE "
            "'${arg_MANIFEST_TEMPLATE}' declares features.self_test, but SELF_TEST is not given, "
            "so the module's table has no self_test")
    endif()

    # The copy of the sources, made by the build whenever one of them changes, and installed as
    # the build checked it: the copy holds exactly the files whose digest the library carries.
    get_target_property(directory ${target} PRAENORA_MODULE_DIR)
    set(python_directory "${directory}/${arg_NAME}.python")
    _praenora_python_copy_sources(${target}
        ROOT "${root}" FILES ${files} DESTINATION "${python_directory}")
    if(DEFINED arg_INSTALL_DESTINATION)
        set(install_options "")
        if(DEFINED arg_INSTALL_COMPONENT)
            list(APPEND install_options COMPONENT "${arg_INSTALL_COMPONENT}")
        endif()
        if(arg_INSTALL_EXCLUDE_FROM_ALL)
            list(APPEND install_options EXCLUDE_FROM_ALL)
        endif()
        install(DIRECTORY "${python_directory}/"
            DESTINATION "${arg_INSTALL_DESTINATION}/${arg_NAME}.python"
            ${install_options}
            PATTERN "__pycache__" EXCLUDE)
    endif()

    set_target_properties(${target} PROPERTIES
        PRAENORA_PYTHON_MODULE_DIR "${python_directory}"
        PRAENORA_PYTHON_MODULE_SOURCES_SHA256 "${digest}")
endfunction()

# _praenora_python_copy_sources(<target> ROOT <dir> FILES <file>... DESTINATION <dir>)
#
# Internal, for praenora_add_python_module() and the C++ fixture module fx-python-call of this
# repository's tests: the build step that copies the Python sources FILES (absolute paths below
# ROOT) into DESTINATION (which may hold $<CONFIG>), keeping their paths below ROOT, so that
# DESTINATION holds them and nothing else. It runs before <target> is built, whenever one of the
# files or their list changes; the list is
# <current binary dir>/praenora-python-modules/<target>-files.txt.
function(_praenora_python_copy_sources target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ROOT;DESTINATION" "FILES")
    set(relative_files "")
    foreach(file IN LISTS arg_FILES)
        file(RELATIVE_PATH relative "${arg_ROOT}" "${file}")
        list(APPEND relative_files "${relative}")
    endforeach()
    list(REMOVE_DUPLICATES relative_files)
    list(SORT relative_files)
    set(work "${CMAKE_CURRENT_BINARY_DIR}/praenora-python-modules")
    set(list_file "${work}/${target}-files.txt")
    # @ONLY with the list as the one variable: the file names are written as they are.
    list(JOIN relative_files "\n" _praenora_python_lines)
    file(CONFIGURE OUTPUT "${list_file}" CONTENT "@_praenora_python_lines@\n" @ONLY
        NEWLINE_STYLE UNIX)
    set(stamp "${work}/${target}-$<CONFIG>.stamp")
    add_custom_command(OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}"
            "-DPRAENORA_PYTHON_COPY_ROOT=${arg_ROOT}"
            "-DPRAENORA_PYTHON_COPY_DESTINATION=${arg_DESTINATION}"
            "-DPRAENORA_PYTHON_COPY_LIST=${list_file}"
            "-DPRAENORA_PYTHON_COPY_STAMP=${stamp}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        DEPENDS ${arg_FILES} "${list_file}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        COMMENT "Copying the Python sources of ${target}"
        VERBATIM)
    add_custom_target(${target}_python_sources DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_python_sources)
endfunction()

# The build step that copies the Python sources (above), run as a script.
function(_praenora_python_module_copy)
    foreach(variable IN ITEMS ROOT DESTINATION LIST STAMP)
        if(NOT DEFINED PRAENORA_PYTHON_COPY_${variable}
           OR "${PRAENORA_PYTHON_COPY_${variable}}" STREQUAL "")
            message(FATAL_ERROR
                "PraenoraPythonModule.cmake: give -DPRAENORA_PYTHON_COPY_${variable}")
        endif()
    endforeach()
    file(STRINGS "${PRAENORA_PYTHON_COPY_LIST}" files ENCODING UTF-8)
    file(REMOVE_RECURSE "${PRAENORA_PYTHON_COPY_DESTINATION}")
    file(MAKE_DIRECTORY "${PRAENORA_PYTHON_COPY_DESTINATION}")
    foreach(file IN LISTS files)
        get_filename_component(directory "${PRAENORA_PYTHON_COPY_DESTINATION}/${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${directory}")
        file(COPY_FILE "${PRAENORA_PYTHON_COPY_ROOT}/${file}"
            "${PRAENORA_PYTHON_COPY_DESTINATION}/${file}")
    endforeach()
    file(TOUCH "${PRAENORA_PYTHON_COPY_STAMP}")
endfunction()

if(DEFINED CMAKE_SCRIPT_MODE_FILE AND DEFINED PRAENORA_PYTHON_COPY_LIST)
    _praenora_python_module_copy()
endif()
