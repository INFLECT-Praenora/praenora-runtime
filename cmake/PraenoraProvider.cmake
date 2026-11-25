# praenora_add_provider(<target>
#     NAME <name> VERSION <semver> SERVICES <service>@<major>.<minor>... ENGINE_JSON <json>
#     SOURCES <source>...
#     [OUTPUT_DIRECTORY <dir>] [INSTALL_DESTINATION <dir> [INSTALL_COMPONENT <component>]]
#     [LINK_LIBRARIES <library>...])
#
# Builds a service provider: a library that offers runtime services to modules through the
# provider interface (src/runtime/provider/provider_abi.h), plus its provider manifest
# (docs/specs/runtime-services.md). The provider interface is internal to the runtime, so this
# helper is used only in this repository and is not installed.
#
# - <target> is always an add_library(MODULE) target. Its file is <name> plus the platform's module
#   suffix, without a "lib" prefix. Sources compile with hidden symbol visibility, and an export
#   list keeps every symbol but the entry point praenora_provider_get_api out of the library's
#   exports, also those of static libraries linked into it (macOS: -exported_symbol; other ELF
#   platforms: a version script; Windows exports only what is marked __declspec(dllexport)).
# - It links the provider interface (praenora_provider_interface: provider_abi.h, which code
#   includes as "provider_abi.h", and the module SDK's C headers) and LINK_LIBRARIES, and gets the
#   compile definitions PRAENORA_PROVIDER_IMPLEMENTATION, PRAENORA_PROVIDER_NAME="<name>",
#   PRAENORA_PROVIDER_VERSION="<semver>", PRAENORA_PROVIDER_SERVICES (a C string literal holding
#   the services as a JSON array, [{"name":"<service>","major":<major>,"minor":<minor>},...]) and
#   PRAENORA_PROVIDER_ENGINE (a C string literal holding ENGINE_JSON), from which describe() can
#   report exactly what the manifest says.
# - SERVICES lists the services the provider offers, each once per name and major. ENGINE_JSON is
#   a JSON object describing the engine behind them ({} for none); it goes into the manifest as
#   written (a line break and the white space around it become one space) and must not contain
#   ';' or "$<".
# - The library goes to OUTPUT_DIRECTORY (default: <current binary dir>/<name>); relative paths
#   are relative to the current binary directory. A multi-config generator gets one directory per
#   configuration: OUTPUT_DIRECTORY may contain $<CONFIG> for that, otherwise /$<CONFIG> is
#   appended.
# - The manifest <name>.provider.json is generated beside the library for each configuration:
#   kind provider-manifest, format_version PRAENORA_FORMAT_VERSION_provider-manifest,
#   runtime_version the runtime's version, abi the provider interface version, library.path the
#   library's file name, platform the platform being built for, services and engine.
# - With INSTALL_DESTINATION, the library and the manifest are installed there, together;
#   INSTALL_COMPONENT names their install component (by default, CMake's default component).
# - The target properties PRAENORA_PROVIDER_DIR and PRAENORA_PROVIDER_MANIFEST hold the output
#   directory and the manifest path; each may contain $<CONFIG>, which $<TARGET_PROPERTY> leaves
#   as it is in a property of its own: read them with
#   $<GENEX_EVAL:$<TARGET_PROPERTY:<target>,PRAENORA_PROVIDER_MANIFEST>>.
#
# praenora_build_tree_provider_dir(<variable>)
#
# Sets <variable> to the directory where the build tree's runtime library looks for the providers
# of its installation: <directory of praenora_runtime>/praenora/providers (on Windows,
# <directory of the runtime DLL>/../lib/praenora/providers), with /$<CONFIG> after the runtime's
# directory for a multi-config generator. A provider built into <that directory>/<name> is found
# by the build tree's tool and runtime without a provider path, as an installation's is.
#
# praenora_remove_build_tree_provider(<name>)
#
# Removes <name>'s directory below praenora_build_tree_provider_dir() (for each configuration of a
# multi-config generator) when it exists and lies in CMAKE_BINARY_DIR: a build tree that built the
# provider before, and is configured again without it, must not keep finding it.
#
# praenora_python_environment_file_from(<variable> <directory>)
#
# Sets <variable> to the path of the installation's environment file of the provider python,
# ${CMAKE_INSTALL_LIBDIR}/praenora/providers/python/python-environment.json, relative to the
# installation's directory <directory> (relative to the installation's prefix, or absolute), with
# '/' separators: ../lib64/praenora/providers/python/python-environment.json from bin, for
# example. A relative directory is taken below CMAKE_INSTALL_PREFIX, as an absolute
# CMAKE_INSTALL_LIBDIR is expected to be.
#
# praenora_python_environment_file_definition(<target> <directory>)
#
# Gives <target>, a program of the runtime that an installation holds in <directory>, the compile
# definition PRAENORA_PYTHON_ENVIRONMENT_FILE: a C string literal holding
# praenora_python_environment_file_from()'s path, so that the program finds the file below the
# installation's library directory, whatever it is named (src/frontend/python_environment.hpp).
# Every program that sets the Python environment's variables gets it, whether or not the tree has
# the provider.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraModule.cmake") # _praenora_module_platform()

function(praenora_build_tree_provider_dir variable)
    if(WIN32)
        get_target_property(directory praenora_runtime RUNTIME_OUTPUT_DIRECTORY)
    else()
        get_target_property(directory praenora_runtime LIBRARY_OUTPUT_DIRECTORY)
    endif()
    if(NOT directory)
        get_target_property(directory praenora_runtime BINARY_DIR)
    endif()
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        string(APPEND directory "/$<CONFIG>")
    endif()
    if(WIN32)
        string(APPEND directory "/../lib/praenora/providers")
    else()
        string(APPEND directory "/praenora/providers")
    endif()
    set(${variable} "${directory}" PARENT_SCOPE)
endfunction()

function(praenora_remove_build_tree_provider name)
    praenora_build_tree_provider_dir(directory)
    set(directories "${directory}/${name}")
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(directories "")
        foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(REPLACE "$<CONFIG>" "${config}" configured "${directory}/${name}")
            list(APPEND directories "${configured}")
        endforeach()
    endif()
    foreach(stale IN LISTS directories)
        cmake_path(NORMAL_PATH stale)
        cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${stale}" NORMALIZE inside)
        if(inside AND IS_DIRECTORY "${stale}")
            file(REMOVE_RECURSE "${stale}")
            message(STATUS "Praenora: removed the provider ${name} that an earlier configuration "
                "built into ${stale}")
        endif()
    endforeach()
endfunction()

function(praenora_python_environment_file_from variable directory)
    set(file "${CMAKE_INSTALL_LIBDIR}/praenora/providers/python/python-environment.json")
    set(base "${directory}")
    foreach(path IN ITEMS file base)
        if(NOT IS_ABSOLUTE "${${path}}")
            set(${path} "${CMAKE_INSTALL_PREFIX}/${${path}}")
        endif()
        cmake_path(SET ${path} NORMALIZE "${${path}}")
    endforeach()
    cmake_path(RELATIVE_PATH file BASE_DIRECTORY "${base}")
    set(${variable} "${file}" PARENT_SCOPE)
endfunction()

function(praenora_python_environment_file_definition target directory)
    praenora_python_environment_file_from(file "${directory}")
    target_compile_definitions(${target} PRIVATE "PRAENORA_PYTHON_ENVIRONMENT_FILE=\"${file}\"")
endfunction()

function(praenora_add_provider target)
    set(one_value_keywords NAME VERSION ENGINE_JSON OUTPUT_DIRECTORY INSTALL_DESTINATION
        INSTALL_COMPONENT)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "${one_value_keywords}"
        "SERVICES;SOURCES;LINK_LIBRARIES")
    set(where "praenora_add_provider(${target})")
    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${where}: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${where}: no value given for ${arg_KEYWORDS_MISSING_VALUES}")
    endif()
    foreach(required IN ITEMS NAME VERSION SERVICES ENGINE_JSON SOURCES)
        if(NOT DEFINED arg_${required})
            message(FATAL_ERROR "${where}: ${required} is required")
        endif()
    endforeach()
    if(DEFINED arg_INSTALL_COMPONENT AND NOT DEFINED arg_INSTALL_DESTINATION)
        message(FATAL_ERROR "${where}: INSTALL_COMPONENT needs INSTALL_DESTINATION")
    endif()
    foreach(variable IN ITEMS PRAENORA_RUNTIME_VERSION PRAENORA_PROVIDER_ABI_MAJOR
                              PRAENORA_PROVIDER_ABI_MINOR PRAENORA_FORMAT_VERSION_provider-manifest)
        if(NOT DEFINED ${variable})
            message(FATAL_ERROR "${where}: ${variable} is not defined "
                "(cmake/PraenoraVersions.cmake defines it)")
        endif()
    endforeach()
    if(NOT TARGET praenora_provider_interface)
        message(FATAL_ERROR "${where}: the target praenora_provider_interface does not exist; "
            "add src/runtime first")
    endif()

    # Provider and service names, and SemVer 2.0 versions.
    set(id_pattern "^[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)*$")
    set(semver_pattern "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)")
    string(APPEND semver_pattern "(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?")
    string(APPEND semver_pattern "(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
    if(NOT arg_NAME MATCHES "${id_pattern}")
        message(FATAL_ERROR "${where}: NAME '${arg_NAME}' does not match ${id_pattern}")
    endif()
    if(NOT arg_VERSION MATCHES "${semver_pattern}")
        message(FATAL_ERROR "${where}: VERSION '${arg_VERSION}' is not a semantic version")
    endif()

    set(services_json "")
    set(service_keys "")
    foreach(service IN LISTS arg_SERVICES)
        if(NOT service MATCHES "^([^@]+)@(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
            message(FATAL_ERROR "${where}: service '${service}' is not <name>@<major>.<minor>")
        endif()
        set(name "${CMAKE_MATCH_1}")
        set(major "${CMAKE_MATCH_2}")
        set(minor "${CMAKE_MATCH_3}")
        if(NOT name MATCHES "${id_pattern}")
            message(FATAL_ERROR
                "${where}: service '${service}' needs a name matching ${id_pattern}")
        endif()
        if(major GREATER 4294967295 OR minor GREATER 4294967295)
            message(FATAL_ERROR "${where}: service '${service}' has a version beyond 32 bits")
        endif()
        if("${name}@${major}" IN_LIST service_keys)
            message(FATAL_ERROR "${where}: service ${name}@${major} is listed more than once")
        endif()
        list(APPEND service_keys "${name}@${major}")
        if(NOT services_json STREQUAL "")
            string(APPEND services_json ",")
        endif()
        string(APPEND services_json
            "{\"name\":\"${name}\",\"major\":${major},\"minor\":${minor}}")
    endforeach()
    set(services_json "[${services_json}]")

    string(REGEX REPLACE "[ \t]*[\r\n]+[ \t]*" " " engine_json "${arg_ENGINE_JSON}")
    string(JSON engine_type ERROR_VARIABLE engine_error TYPE "${engine_json}")
    if(engine_error OR NOT engine_type STREQUAL "OBJECT")
        message(FATAL_ERROR "${where}: ENGINE_JSON is not a JSON object: '${arg_ENGINE_JSON}'")
    endif()

    # Output directory, with one directory per configuration for multi-config generators.
    if(DEFINED arg_OUTPUT_DIRECTORY)
        set(directory "${arg_OUTPUT_DIRECTORY}")
        if(NOT IS_ABSOLUTE "${directory}")
            set(directory "${CMAKE_CURRENT_BINARY_DIR}/${directory}")
        endif()
    else()
        set(directory "${CMAKE_CURRENT_BINARY_DIR}/${arg_NAME}")
    endif()
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    string(GENEX_STRIP "${directory}" directory_without_genex)
    if(multi_config AND directory_without_genex STREQUAL directory)
        string(APPEND directory "/$<CONFIG>")
    endif()
    set(manifest "${directory}/${arg_NAME}.provider.json")

    # The library, exporting only the entry point.
    add_library(${target} MODULE ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE praenora_provider_interface)
    if(DEFINED arg_LINK_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${arg_LINK_LIBRARIES})
    endif()
    foreach(json_variable IN ITEMS services_json engine_json)
        string(REPLACE "\\" "\\\\" literal "${${json_variable}}")
        string(REPLACE "\"" "\\\"" ${json_variable}_literal "${literal}")
    endforeach()
    target_compile_definitions(${target} PRIVATE
        PRAENORA_PROVIDER_IMPLEMENTATION
        "PRAENORA_PROVIDER_NAME=\"${arg_NAME}\""
        "PRAENORA_PROVIDER_VERSION=\"${arg_VERSION}\""
        "PRAENORA_PROVIDER_SERVICES=\"${services_json_literal}\""
        "PRAENORA_PROVIDER_ENGINE=\"${engine_json_literal}\"")
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${arg_NAME}"
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${directory}"
        RUNTIME_OUTPUT_DIRECTORY "${directory}"
        PRAENORA_PROVIDER_DIR "${directory}"
        PRAENORA_PROVIDER_MANIFEST "${manifest}")
    if(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-exported_symbol,_praenora_provider_get_api")
    elseif(NOT WIN32)
        set(version_script "${CMAKE_CURRENT_BINARY_DIR}/praenora-providers/${target}.map")
        file(CONFIGURE OUTPUT "${version_script}" NEWLINE_STYLE UNIX CONTENT [=[{
  global:
    praenora_provider_get_api;
  local:
    *;
};
]=])
        target_link_options(${target} PRIVATE "LINKER:--version-script=${version_script}")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${version_script}")
    endif()

    # The manifest, generated for each configuration (the library's file name may differ).
    _praenora_module_platform(provider_os provider_arch)
    file(GENERATE OUTPUT "${manifest}" CONTENT "{
  \"kind\": \"provider-manifest\",
  \"format_version\": \"${PRAENORA_FORMAT_VERSION_provider-manifest}\",
  \"provider\": { \"name\": \"${arg_NAME}\", \"version\": \"${arg_VERSION}\" },
  \"runtime_version\": \"${PRAENORA_RUNTIME_VERSION}\",
  \"abi\": { \"major\": ${PRAENORA_PROVIDER_ABI_MAJOR}, \"minor\": ${PRAENORA_PROVIDER_ABI_MINOR} },
  \"library\": { \"path\": \"$<TARGET_FILE_NAME:${target}>\" },
  \"platform\": { \"os\": \"${provider_os}\", \"arch\": \"${provider_arch}\" },
  \"services\": ${services_json},
  \"engine\": ${engine_json}
}
" NEWLINE_STYLE UNIX)

    if(DEFINED arg_INSTALL_DESTINATION)
        set(install_options "")
        if(DEFINED arg_INSTALL_COMPONENT)
            list(APPEND install_options COMPONENT "${arg_INSTALL_COMPONENT}")
        endif()
        install(TARGETS ${target}
            LIBRARY DESTINATION "${arg_INSTALL_DESTINATION}" ${install_options}
            RUNTIME DESTINATION "${arg_INSTALL_DESTINATION}" ${install_options})
        install(FILES "${manifest}" DESTINATION "${arg_INSTALL_DESTINATION}" ${install_options})
    endif()
endfunction()
