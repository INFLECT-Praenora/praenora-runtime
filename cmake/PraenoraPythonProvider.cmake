# praenora_add_python_provider(<target>
#     NAME <name> VERSION <semver> SERVICES <service>@<major>.<minor>... SOURCES <source>...
#     [PYTHON_SOURCES <file-or-directory>... [PYTHON_ROOT <dir>]] [ENGINE_JSON <json>]
#     [OUTPUT_DIRECTORY <dir>] [INSTALL_DESTINATION <dir>])
#
# Builds a service provider that runs Python: praenora_add_provider() (cmake/PraenoraProvider.cmake)
# with what the provider needs to load and start the process's Python core at its start()
# (docs/specs/python-service.md, the providers that run Python). Internal, as the provider
# interface is: used only in this repository, with PRAENORA_WITH_PYTHON_ENVIRONMENT, after
# src/providers/python made the Python core (praenora_python_core) and the static library of the
# loader and the service glue (praenora_python_provider_support), and never installed.
#
# - The provider links praenora_python_provider_support, which gives it the compile definitions
#   PRAENORA_PYTHON_ENVIRONMENT_PACKAGE and PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256 (the
#   environment the build uses) and PRAENORA_PYTHON_CORE_BUILD_ID (the core's build identity),
#   which its loader requires at start; it gets PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256, the
#   directory digest of its Python sources (empty without PYTHON_SOURCES), besides. It links no
#   Python.
# - The engine object of its manifest and its describe(): without ENGINE_JSON exactly
#   PRAENORA_PYTHON_ENGINE_JSON (cmake/PraenoraPythonEnvironment.cmake: the interpreter and the
#   environment); with it, that JSON object ({} for none) as written, a line break and the white
#   space around it made one space as praenora_add_provider() writes it, with the entry
#   "environment": <PRAENORA_PYTHON_ENVIRONMENT_JSON> (the environment's name, version, package
#   and lock) after its last one: both texts are kept byte for byte, never rewritten by a JSON
#   writer. ENGINE_JSON must not have an "environment" of its own.
# - PYTHON_SOURCES, files and directories (every file below one but those below __pycache__),
#   relative to PYTHON_ROOT (default: the current source directory) or absolute below it, are
#   copied at build time into <output directory>/python, keeping their paths below PYTHON_ROOT;
#   that directory then holds them and nothing else. Their digest
#   (praenora_python_directory_digest()) is what PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256 holds. A
#   provider in the core's own directory cannot have them: python/ there is the core's.
# - The environment file python-environment.json, {"environment": <dir>, "core": <file>}, which the
#   loader reads: beside the library, naming the bundle the build uses and the build tree's core
#   by their absolute paths; and, for an installation, one naming them relative to its own
#   directory (../../python-environment from <libdir>/praenora/providers/<name>, and the core in
#   <libdir>/praenora/providers/python), in
#   <current binary dir>/praenora-python-providers/<target>/ (one directory per configuration of a
#   multi-config generator).
# - With INSTALL_DESTINATION (relative to the installation's prefix), the library, its manifest,
#   the installation's environment file and the Python sources (in python/) are installed there.
# - The other arguments are praenora_add_provider()'s; OUTPUT_DIRECTORY defaults to
#   <current binary dir>/<name> as there.
# - The target properties PRAENORA_PYTHON_ENVIRONMENT_FILE (the build tree's environment file; it
#   may contain $<CONFIG>) and PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256 hold what their names say.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraProvider.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraPythonDigest.cmake")

# _praenora_python_json_escape(<out_var> <text>)
# `text` as the inside of a JSON string literal: quotes, backslashes and every control character
# escaped (\b, \t, \n, \f, \r, the others as \u00XX), everything else, text that is not ASCII
# included, as it is.
function(_praenora_python_json_escape out_var text)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE "\n" "\\n" text "${text}")
    string(REPLACE "\r" "\\r" text "${text}")
    string(REPLACE "\t" "\\t" text "${text}")
    foreach(code RANGE 1 31)
        if(code EQUAL 9 OR code EQUAL 10 OR code EQUAL 13)
            continue()
        endif()
        if(code EQUAL 8)
            set(escape "\\b")
        elseif(code EQUAL 12)
            set(escape "\\f")
        else()
            math(EXPR high "${code} / 16")
            math(EXPR low "${code} % 16")
            string(SUBSTRING "0123456789abcdef" ${low} 1 low)
            set(escape "\\u00${high}${low}")
        endif()
        string(ASCII ${code} character)
        string(REPLACE "${character}" "${escape}" text "${text}")
    endforeach()
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# _praenora_python_json_string(<out_var> <text>)
# A JSON string literal of `text`, escaped as _praenora_python_json_escape() escapes it.
function(_praenora_python_json_string out_var text)
    _praenora_python_json_escape(text "${text}")
    set(${out_var} "\"${text}\"" PARENT_SCOPE)
endfunction()

# _praenora_python_provider_engine(<out_var> <where> <engine_json> <environment_json>)
# The engine object of a provider that runs Python with an ENGINE_JSON of its own: `engine_json`
# as written, a line break and the white space around it made one space (as
# praenora_add_provider() writes it), with "environment": <environment_json> after its last entry,
# ", " before it when there is one. Text is only appended: no JSON writer rewrites the given
# values, their spelling of numbers or their text that is not ASCII. Stops the configuration,
# naming `where`, when `engine_json` is not a JSON object or has an "environment" of its own.
function(_praenora_python_provider_engine out_var where engine_json environment_json)
    string(REGEX REPLACE "[ \t]*[\r\n]+[ \t]*" " " engine "${engine_json}")
    string(JSON engine_type ERROR_VARIABLE engine_error TYPE "${engine}")
    if(engine_error OR NOT engine_type STREQUAL "OBJECT")
        message(FATAL_ERROR "${where}: ENGINE_JSON is not a JSON object: '${engine_json}'")
    endif()
    string(JSON own_environment ERROR_VARIABLE no_environment GET "${engine}" environment)
    if(NOT no_environment)
        message(FATAL_ERROR "${where}: ENGINE_JSON has an \"environment\" of its own; the "
            "environment's is added to it")
    endif()
    string(JSON entries LENGTH "${engine}")
    # The object's text ends with its closing brace, white space aside: the environment's entry
    # goes before it.
    string(STRIP "${engine}" engine)
    string(REGEX REPLACE "[ \t]*}$" "" engine "${engine}")
    if(entries GREATER 0)
        string(APPEND engine ", ")
    endif()
    set(${out_var} "${engine}\"environment\": ${environment_json}}" PARENT_SCOPE)
endfunction()

function(praenora_add_python_provider target)
    set(one_value_keywords NAME VERSION PYTHON_ROOT ENGINE_JSON OUTPUT_DIRECTORY
        INSTALL_DESTINATION)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "${one_value_keywords}"
        "SERVICES;SOURCES;PYTHON_SOURCES")
    set(where "praenora_add_python_provider(${target})")
    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${where}: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${where}: no value given for ${arg_KEYWORDS_MISSING_VALUES}")
    endif()
    foreach(required IN ITEMS NAME VERSION SERVICES SOURCES)
        if(NOT DEFINED arg_${required})
            message(FATAL_ERROR "${where}: ${required} is required")
        endif()
    endforeach()
    if(DEFINED arg_PYTHON_ROOT AND NOT DEFINED arg_PYTHON_SOURCES)
        message(FATAL_ERROR "${where}: PYTHON_ROOT needs PYTHON_SOURCES")
    endif()
    if(DEFINED arg_INSTALL_DESTINATION AND IS_ABSOLUTE "${arg_INSTALL_DESTINATION}")
        message(FATAL_ERROR "${where}: INSTALL_DESTINATION must be relative to the installation's "
            "prefix, so that the installation's environment file names its directories relative "
            "to itself and the installation can be moved")
    endif()
    foreach(required_target IN ITEMS praenora_python_core praenora_python_provider_support)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR "${where}: the target ${required_target} does not exist; add "
                "src/providers/python first (PRAENORA_WITH_PYTHON_ENVIRONMENT)")
        endif()
    endforeach()
    foreach(variable IN ITEMS PRAENORA_PYTHON_ENGINE_JSON PRAENORA_PYTHON_ENVIRONMENT_JSON
                              PRAENORA_PYTHON_ENVIRONMENT_DIR)
        if(NOT DEFINED ${variable})
            message(FATAL_ERROR "${where}: ${variable} is not defined "
                "(cmake/PraenoraPythonEnvironment.cmake defines it)")
        endif()
    endforeach()

    # The engine object.
    if(DEFINED arg_ENGINE_JSON)
        _praenora_python_provider_engine(engine "${where}" "${arg_ENGINE_JSON}"
            "${PRAENORA_PYTHON_ENVIRONMENT_JSON}")
    else()
        set(engine "${PRAENORA_PYTHON_ENGINE_JSON}")
    endif()

    # The output directory, as praenora_add_provider() makes it.
    if(DEFINED arg_OUTPUT_DIRECTORY)
        set(output_directory "${arg_OUTPUT_DIRECTORY}")
    else()
        set(output_directory "${CMAKE_CURRENT_BINARY_DIR}/${arg_NAME}")
    endif()
    set(install_options "")
    if(DEFINED arg_INSTALL_DESTINATION)
        set(install_options INSTALL_DESTINATION "${arg_INSTALL_DESTINATION}")
    endif()
    praenora_add_provider(${target}
        NAME "${arg_NAME}"
        VERSION "${arg_VERSION}"
        SERVICES ${arg_SERVICES}
        ENGINE_JSON "${engine}"
        SOURCES ${arg_SOURCES}
        OUTPUT_DIRECTORY "${output_directory}"
        LINK_LIBRARIES praenora_python_provider_support
        ${install_options})
    get_target_property(directory ${target} PRAENORA_PROVIDER_DIR)
    get_target_property(core_directory praenora_python_core PRAENORA_PYTHON_CORE_DIR)
    # The environment file names the core, which must be built first.
    add_dependencies(${target} praenora_python_core)

    # The Python sources: their digest, their copy and their installation.
    set(sources_digest "")
    if(DEFINED arg_PYTHON_SOURCES)
        cmake_path(SET normal_directory NORMALIZE "${directory}")
        cmake_path(SET normal_core_directory NORMALIZE "${core_directory}")
        if(normal_directory STREQUAL normal_core_directory)
            message(FATAL_ERROR "${where}: a provider in the Python core's directory "
                "(${core_directory}) cannot have PYTHON_SOURCES: its python/ is the core's")
        endif()
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
        praenora_python_directory_digest(sources_digest ROOT "${root}" FILES ${files})
        set(relative_files "")
        foreach(file IN LISTS files)
            file(RELATIVE_PATH relative "${root}" "${file}")
            list(APPEND relative_files "${relative}")
        endforeach()
        list(REMOVE_DUPLICATES relative_files)
        list(SORT relative_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${files})

        set(list_file "${CMAKE_CURRENT_BINARY_DIR}/praenora-python-providers/${target}-files.txt")
        list(JOIN relative_files "\n" lines)
        file(CONFIGURE OUTPUT "${list_file}" CONTENT "${lines}\n" NEWLINE_STYLE UNIX)
        set(stamp "${CMAKE_CURRENT_BINARY_DIR}/praenora-python-providers/${target}-$<CONFIG>.stamp")
        add_custom_command(OUTPUT "${stamp}"
            COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_ROOT=${root}"
                "-DDESTINATION=${directory}/python"
                "-DLIST=${list_file}"
                "-DSTAMP=${stamp}"
                -P "${PROJECT_SOURCE_DIR}/src/providers/python/copy_python_sources.cmake"
            DEPENDS ${files} "${list_file}"
                "${PROJECT_SOURCE_DIR}/src/providers/python/copy_python_sources.cmake"
            COMMENT "Copying the Python sources of the provider ${arg_NAME}"
            VERBATIM)
        add_custom_target(${target}_python_sources DEPENDS "${stamp}")
        add_dependencies(${target} ${target}_python_sources)
        if(DEFINED arg_INSTALL_DESTINATION)
            foreach(relative IN LISTS relative_files)
                get_filename_component(subdirectory "${relative}" DIRECTORY)
                install(FILES "${root}/${relative}"
                    DESTINATION "${arg_INSTALL_DESTINATION}/python/${subdirectory}")
            endforeach()
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE
        "PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256=\"${sources_digest}\"")

    # The build tree's environment file: absolute paths. The core's is its directory, escaped as
    # the environment's is, and its file name, which CMake gives (the core's OUTPUT_NAME and the
    # platform's suffix, which need no escape): $<TARGET_FILE> could not be escaped.
    # The directory is normalized first. On Windows with a multi-config generator it is
    # <runtime directory>/$<CONFIG>/../lib/praenora/providers/python
    # (praenora_build_tree_provider_dir()), so the file's own path, evaluated for each
    # configuration, is one and the same, and the text of the step through $<CONFIG> would make its
    # content differ from one configuration to the next, which file(GENERATE) refuses ("written
    # multiple times with different content"). A path that does depend on the configuration keeps
    # $<CONFIG> and gets a file of its own per configuration.
    _praenora_python_json_string(environment_literal "${PRAENORA_PYTHON_ENVIRONMENT_DIR}")
    cmake_path(SET core_directory_normal NORMALIZE "${core_directory}")
    _praenora_python_json_escape(core_directory_text "${core_directory_normal}")
    set(environment_file "${directory}/python-environment.json")
    file(GENERATE OUTPUT "${environment_file}" CONTENT "{
  \"environment\": ${environment_literal},
  \"core\": \"${core_directory_text}/$<TARGET_FILE_NAME:praenora_python_core>\"
}
" NEWLINE_STYLE UNIX)

    # The installation's: paths relative to its own directory.
    if(DEFINED arg_INSTALL_DESTINATION)
        set(destination "${arg_INSTALL_DESTINATION}")
    else()
        set(destination "${CMAKE_INSTALL_LIBDIR}/praenora/providers/${arg_NAME}")
    endif()
    cmake_path(SET destination NORMALIZE "/${destination}")
    cmake_path(SET installed_environment NORMALIZE
        "/${CMAKE_INSTALL_LIBDIR}/praenora/python-environment")
    cmake_path(SET installed_core_directory NORMALIZE
        "/${CMAKE_INSTALL_LIBDIR}/praenora/providers/python")
    cmake_path(RELATIVE_PATH installed_environment BASE_DIRECTORY "${destination}")
    cmake_path(RELATIVE_PATH installed_core_directory BASE_DIRECTORY "${destination}")
    if(installed_core_directory STREQUAL "." OR installed_core_directory STREQUAL "")
        set(installed_core "$<TARGET_FILE_NAME:praenora_python_core>")
    else()
        string(REGEX REPLACE "/$" "" installed_core_directory "${installed_core_directory}")
        _praenora_python_json_escape(installed_core_directory "${installed_core_directory}")
        set(installed_core "${installed_core_directory}/$<TARGET_FILE_NAME:praenora_python_core>")
    endif()
    string(REGEX REPLACE "/$" "" installed_environment "${installed_environment}")
    _praenora_python_json_string(installed_environment_literal "${installed_environment}")
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(installed_file_directory "${CMAKE_CURRENT_BINARY_DIR}/praenora-python-providers/${target}")
    if(multi_config)
        string(APPEND installed_file_directory "/$<CONFIG>")
    endif()
    set(installed_file "${installed_file_directory}/python-environment.json")
    file(GENERATE OUTPUT "${installed_file}" CONTENT "{
  \"environment\": ${installed_environment_literal},
  \"core\": \"${installed_core}\"
}
" NEWLINE_STYLE UNIX)
    if(DEFINED arg_INSTALL_DESTINATION)
        install(FILES "${installed_file}" DESTINATION "${arg_INSTALL_DESTINATION}")
    endif()

    set_target_properties(${target} PROPERTIES
        PRAENORA_PYTHON_ENVIRONMENT_FILE "${environment_file}"
        PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256 "${sources_digest}")
endfunction()
