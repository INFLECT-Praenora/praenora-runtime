# praenora_add_module(<target>
#     NAME <name> VERSION <semver> CAPABILITIES <id@version>... SOURCES <source>...
#     [REQUIRES_SERVICES <name>@<major>.<minor>...] [OPTIONAL_SERVICES <name>@<major>.<minor>...]
#     [MANIFEST_TEMPLATE <file>] [TEST_VECTORS <file>] [OUTPUT_DIRECTORY <dir>]
#     [INSTALL_DESTINATION <dir> [INSTALL_COMPONENT <component>] [INSTALL_EXCLUDE_FROM_ALL]]
#     [LINK_LIBRARIES <library>...])
#
# Builds a Praenora module: an independently loadable library that implements the capabilities
# listed, plus its Praenora Module Manifest. Installed with the module SDK; module projects use it
# after find_package(Praenora COMPONENTS ModuleSDK), and this repository uses it the same way.
#
# - REQUIRES_SERVICES lists the runtime services the module requires and OPTIONAL_SERVICES those
#   it can use when they are there, each as <name>@<major>.<minor> (the lowest minor it accepts),
#   for example praenora.simulation@0.0. A service name and major may appear once in both lists
#   together. They become the manifest's requires.services; validation refuses a run whose
#   required services no provider offers, and a module sees only the provider services it lists.
#
# - <target> is always an add_library(MODULE) target, whatever BUILD_SHARED_LIBS says. Its file is
#   <name> plus the platform's module suffix, without a "lib" prefix (fx-echo.so, fx-echo.dll).
#   Sources compile with hidden symbol visibility, so only the entry point is exported.
# - It links Praenora::ModuleSDK and LINK_LIBRARIES, and gets the compile definitions
#   PRAENORA_MODULE_IMPLEMENTATION, PRAENORA_MODULE_NAME="<name>",
#   PRAENORA_MODULE_VERSION="<semver>" and PRAENORA_MODULE_CAPABILITIES (a C string literal
#   holding the capabilities as a JSON array, [{"id":"<id>","version":"<version>"},...]), which
#   the C++ adapter reports from describe().
# - The library goes to OUTPUT_DIRECTORY (default: <current binary dir>/<name>); relative paths
#   are relative to the current binary directory. A multi-config generator gets one directory per
#   configuration: OUTPUT_DIRECTORY may contain $<CONFIG> for that, otherwise /$<CONFIG> is
#   appended.
# - The manifest <name>.pmm.json is generated beside the library, in the module manifest format
#   of the SDK (PRAENORA_MODULE_MANIFEST_FORMAT_VERSION, 0.2.0). MANIFEST_TEMPLATE is a file
#   (relative to the current source directory) processed like configure_file(@ONLY) with the
#   variables below; generator expressions in the result are then evaluated, so the template must
#   not otherwise contain "$<". Without a template, a manifest with the required fields,
#   "parameters_schema": {"type": "object"} and, when the module lists services, "requires" is
#   generated. (Internal, for praenora_add_python_module(): INTERNAL_FEATURES_JSON <object> adds
#   "features": <object> to that generated manifest; it is ignored with a template.)
#     @PRAENORA_MODULE_MANIFEST_FORMAT_VERSION@ (the format_version to write)
#     @PRAENORA_MODULE_NAME@ @PRAENORA_MODULE_VERSION@ @PRAENORA_MODULE_CAPABILITIES_JSON@
#     @PRAENORA_MODULE_REQUIRES_JSON@ (the value of "requires": {"services": [{"name", "major",
#     "minor", "optional"}, ...]}, the required services first, an empty list for none)
#     @PRAENORA_ABI_MAJOR@ @PRAENORA_ABI_MINOR@ @PRAENORA_PMP_VERSION@ @PRAENORA_SDK_VERSION@
#     @PRAENORA_MODULE_OS@ @PRAENORA_MODULE_ARCH@ (manifest vocabulary: linux|macos|windows and
#     x86_64|arm64) @PRAENORA_MODULE_TEST_VECTORS@ (file name of the copied vectors, or empty)
#     @PRAENORA_MODULE_LIBRARY_FILE@ (the library's file name, for every configuration)
# - TEST_VECTORS is copied beside the manifest at build time.
# - With INSTALL_DESTINATION, the library, the manifest and the vectors are installed there,
#   together: the manifest names the other two relative to itself. INSTALL_COMPONENT names their
#   install component (by default, CMake's default component). With INSTALL_EXCLUDE_FROM_ALL a
#   full installation leaves them out, and only an installation of their component installs them
#   (cmake --install <build> --component <component>). Both need INSTALL_DESTINATION.
# - The target properties PRAENORA_MODULE_DIR, PRAENORA_MODULE_MANIFEST and
#   PRAENORA_MODULE_TEST_VECTORS_PATH hold the output directory, the manifest path and the path of
#   the copied test vectors (empty without TEST_VECTORS); each may contain $<CONFIG>, which
#   $<TARGET_PROPERTY> leaves as it is in a property of its own: read them with
#   $<GENEX_EVAL:$<TARGET_PROPERTY:<target>,PRAENORA_MODULE_MANIFEST>>. The target property
#   PRAENORA_MODULE_CONFIGURED_MANIFEST holds the path of the manifest as configured when the
#   function ran, before its generator expressions are evaluated (its contents are known at
#   configure time; praenora_add_python_module() reads them).
#
# The ABI, protocol, SDK and manifest format versions come from the variables
# PRAENORA_ABI_MAJOR, PRAENORA_ABI_MINOR, PRAENORA_PMP_VERSION, PRAENORA_SDK_VERSION and
# PRAENORA_MODULE_MANIFEST_FORMAT_VERSION, which the installed package configuration (or, in this
# repository, cmake/PraenoraVersions.cmake) defines.

include_guard(GLOBAL)

# _praenora_module_platform(<os-var> <arch-var>)
#
# The platform being built for, in manifest vocabulary. The architecture comes from the first
# source that is set: the compiler's target architecture, the Visual Studio platform,
# CMAKE_OSX_ARCHITECTURES, then CMAKE_SYSTEM_PROCESSOR. Values outside the vocabulary are
# lowercased and passed through.
function(_praenora_module_platform os_var arch_var)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(os "linux")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(os "macos")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(os "windows")
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" os)
    endif()

    list(LENGTH CMAKE_OSX_ARCHITECTURES osx_arch_count)
    if(osx_arch_count GREATER 1)
        message(FATAL_ERROR "praenora_add_module: a module manifest declares one architecture; "
            "CMAKE_OSX_ARCHITECTURES is '${CMAKE_OSX_ARCHITECTURES}'.")
    endif()
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
        set(arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID)
        set(arch "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
    elseif(CMAKE_VS_PLATFORM_NAME)
        set(arch "${CMAKE_VS_PLATFORM_NAME}")
    elseif(CMAKE_OSX_ARCHITECTURES)
        set(arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${arch}" arch)
    if(arch MATCHES "^(arm64|aarch64)$")
        set(arch "arm64")
    elseif(arch MATCHES "^(x86_64|amd64|x64)$")
        set(arch "x86_64")
    endif()

    set(${os_var} "${os}" PARENT_SCOPE)
    set(${arch_var} "${arch}" PARENT_SCOPE)
endfunction()

function(praenora_add_module target)
    set(one_value_keywords NAME VERSION MANIFEST_TEMPLATE TEST_VECTORS OUTPUT_DIRECTORY
        INSTALL_DESTINATION INSTALL_COMPONENT INTERNAL_FEATURES_JSON)
    cmake_parse_arguments(PARSE_ARGV 1 arg "INSTALL_EXCLUDE_FROM_ALL" "${one_value_keywords}"
        "CAPABILITIES;SOURCES;LINK_LIBRARIES;REQUIRES_SERVICES;OPTIONAL_SERVICES")
    set(where "praenora_add_module(${target})")
    if(DEFINED arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${where}: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${where}: no value given for ${arg_KEYWORDS_MISSING_VALUES}")
    endif()
    foreach(required IN ITEMS NAME VERSION CAPABILITIES SOURCES)
        if(NOT DEFINED arg_${required})
            message(FATAL_ERROR "${where}: ${required} is required")
        endif()
    endforeach()
    if((DEFINED arg_INSTALL_COMPONENT OR arg_INSTALL_EXCLUDE_FROM_ALL)
       AND NOT DEFINED arg_INSTALL_DESTINATION)
        message(FATAL_ERROR
            "${where}: INSTALL_COMPONENT and INSTALL_EXCLUDE_FROM_ALL need INSTALL_DESTINATION")
    endif()
    foreach(variable IN ITEMS PRAENORA_ABI_MAJOR PRAENORA_ABI_MINOR PRAENORA_PMP_VERSION
                              PRAENORA_SDK_VERSION PRAENORA_MODULE_MANIFEST_FORMAT_VERSION)
        if(NOT DEFINED ${variable})
            message(FATAL_ERROR "${where}: ${variable} is not defined; load the SDK with "
                "find_package(Praenora COMPONENTS ModuleSDK) first.")
        endif()
    endforeach()
    if(NOT TARGET Praenora::ModuleSDK)
        message(FATAL_ERROR "${where}: the target Praenora::ModuleSDK does not exist; load the "
            "SDK with find_package(Praenora COMPONENTS ModuleSDK) first.")
    endif()

    # Module names and capability ids, and SemVer 2.0 versions.
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

    set(capabilities_json "")
    set(capability_ids "")
    foreach(capability IN LISTS arg_CAPABILITIES)
        if(NOT capability MATCHES "^([^@]+)@([^@]+)$")
            message(FATAL_ERROR "${where}: capability '${capability}' is not <id>@<version>")
        endif()
        set(id "${CMAKE_MATCH_1}")
        set(version "${CMAKE_MATCH_2}")
        if(NOT id MATCHES "${id_pattern}" OR NOT version MATCHES "${semver_pattern}")
            message(FATAL_ERROR "${where}: capability '${capability}' needs an id matching "
                "${id_pattern} and a semantic version")
        endif()
        if(id IN_LIST capability_ids)
            message(FATAL_ERROR "${where}: capability ${id} is listed more than once")
        endif()
        list(APPEND capability_ids "${id}")
        if(NOT capabilities_json STREQUAL "")
            string(APPEND capabilities_json ",")
        endif()
        string(APPEND capabilities_json "{\"id\":\"${id}\",\"version\":\"${version}\"}")
    endforeach()
    set(capabilities_json "[${capabilities_json}]")

    # The services the module requires, then those it can use: requires.services.
    set(services_json "")
    set(service_keys "")
    foreach(kind IN ITEMS REQUIRES_SERVICES OPTIONAL_SERVICES)
        if(kind STREQUAL "OPTIONAL_SERVICES")
            set(optional "true")
        else()
            set(optional "false")
        endif()
        foreach(service IN LISTS arg_${kind})
            if(NOT service MATCHES "^([^@]+)@(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
                message(FATAL_ERROR "${where}: service '${service}' in ${kind} is not "
                    "<name>@<major>.<minor>")
            endif()
            set(service_name "${CMAKE_MATCH_1}")
            set(service_major "${CMAKE_MATCH_2}")
            set(service_minor "${CMAKE_MATCH_3}")
            if(NOT service_name MATCHES "${id_pattern}")
                message(FATAL_ERROR "${where}: service '${service}' in ${kind} needs a name "
                    "matching ${id_pattern}")
            endif()
            if(service_major GREATER 4294967295 OR service_minor GREATER 4294967295)
                message(FATAL_ERROR "${where}: service '${service}' in ${kind}: the major and "
                    "minor version must fit in 32 bits")
            endif()
            if("${service_name}@${service_major}" IN_LIST service_keys)
                message(FATAL_ERROR "${where}: service ${service_name}@${service_major} is listed "
                    "more than once in REQUIRES_SERVICES and OPTIONAL_SERVICES")
            endif()
            list(APPEND service_keys "${service_name}@${service_major}")
            if(NOT services_json STREQUAL "")
                string(APPEND services_json ",")
            endif()
            string(APPEND services_json "{\"name\":\"${service_name}\",\"major\":${service_major},"
                "\"minor\":${service_minor},\"optional\":${optional}}")
        endforeach()
    endforeach()
    set(requires_json "{\"services\":[${services_json}]}")

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
    set(manifest "${directory}/${arg_NAME}.pmm.json")

    # The library.
    add_library(${target} MODULE ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE Praenora::ModuleSDK)
    if(DEFINED arg_LINK_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${arg_LINK_LIBRARIES})
    endif()
    string(REPLACE "\"" "\\\"" capabilities_literal "${capabilities_json}")
    target_compile_definitions(${target} PRIVATE
        PRAENORA_MODULE_IMPLEMENTATION
        "PRAENORA_MODULE_NAME=\"${arg_NAME}\""
        "PRAENORA_MODULE_VERSION=\"${arg_VERSION}\""
        "PRAENORA_MODULE_CAPABILITIES=\"${capabilities_literal}\"")
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${arg_NAME}"
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${directory}"
        RUNTIME_OUTPUT_DIRECTORY "${directory}"
        PRAENORA_MODULE_DIR "${directory}"
        PRAENORA_MODULE_MANIFEST "${manifest}"
        PRAENORA_MODULE_TEST_VECTORS_PATH "")

    # Test vectors, copied beside the manifest whenever they change.
    set(PRAENORA_MODULE_TEST_VECTORS "")
    set(tests_member "")
    if(DEFINED arg_TEST_VECTORS)
        get_filename_component(vectors_source "${arg_TEST_VECTORS}" ABSOLUTE)
        if(NOT EXISTS "${vectors_source}")
            message(FATAL_ERROR "${where}: TEST_VECTORS '${vectors_source}' does not exist")
        endif()
        get_filename_component(PRAENORA_MODULE_TEST_VECTORS "${vectors_source}" NAME)
        set(vectors_output "${directory}/${PRAENORA_MODULE_TEST_VECTORS}")
        add_custom_command(OUTPUT "${vectors_output}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${vectors_source}" "${vectors_output}"
            DEPENDS "${vectors_source}"
            COMMENT "Copying the test vectors of module ${arg_NAME}"
            VERBATIM)
        target_sources(${target} PRIVATE "${vectors_output}")
        set_target_properties(${target} PROPERTIES
            PRAENORA_MODULE_TEST_VECTORS_PATH "${vectors_output}")
        set(tests_member ",\n  \"tests\": { \"vectors\": \"${PRAENORA_MODULE_TEST_VECTORS}\" }")
    endif()

    # The manifest: the template is configured now, and its generator expressions (the library
    # file name) are evaluated for each configuration when the build system is generated.
    _praenora_module_platform(PRAENORA_MODULE_OS PRAENORA_MODULE_ARCH)
    set(PRAENORA_MODULE_NAME "${arg_NAME}")
    set(PRAENORA_MODULE_VERSION "${arg_VERSION}")
    set(PRAENORA_MODULE_CAPABILITIES_JSON "${capabilities_json}")
    set(PRAENORA_MODULE_REQUIRES_JSON "${requires_json}")
    set(requires_member "")
    if(NOT services_json STREQUAL "")
        set(requires_member ",\n  \"requires\": ${requires_json}")
    endif()
    set(features_member "")
    if(DEFINED arg_INTERNAL_FEATURES_JSON)
        set(features_member ",\n  \"features\": ${arg_INTERNAL_FEATURES_JSON}")
    endif()
    set(PRAENORA_MODULE_LIBRARY_FILE "$<TARGET_FILE_NAME:${target}>")
    set(configured "${CMAKE_CURRENT_BINARY_DIR}/praenora-manifests/${target}.pmm.json.in")
    if(DEFINED arg_MANIFEST_TEMPLATE)
        get_filename_component(template "${arg_MANIFEST_TEMPLATE}" ABSOLUTE)
        if(NOT EXISTS "${template}")
            message(FATAL_ERROR "${where}: MANIFEST_TEMPLATE '${template}' does not exist")
        endif()
        configure_file("${template}" "${configured}" @ONLY NEWLINE_STYLE UNIX)
    else()
        file(CONFIGURE OUTPUT "${configured}" @ONLY NEWLINE_STYLE UNIX CONTENT [=[{
  "kind": "module-manifest",
  "format_version": "@PRAENORA_MODULE_MANIFEST_FORMAT_VERSION@",
  "module": {
    "name": "@PRAENORA_MODULE_NAME@",
    "version": "@PRAENORA_MODULE_VERSION@"
  },
  "abi": { "major": @PRAENORA_ABI_MAJOR@, "minor": @PRAENORA_ABI_MINOR@ },
  "pmp_version": "@PRAENORA_PMP_VERSION@",
  "sdk_version": "@PRAENORA_SDK_VERSION@",
  "library": { "path": "@PRAENORA_MODULE_LIBRARY_FILE@" },
  "platform": { "os": "@PRAENORA_MODULE_OS@", "arch": "@PRAENORA_MODULE_ARCH@" },
  "capabilities": @PRAENORA_MODULE_CAPABILITIES_JSON@,
  "parameters_schema": { "type": "object" }@features_member@@requires_member@@tests_member@
}
]=])
    endif()
    file(GENERATE OUTPUT "${manifest}" INPUT "${configured}" NEWLINE_STYLE UNIX)
    set_target_properties(${target} PROPERTIES PRAENORA_MODULE_CONFIGURED_MANIFEST "${configured}")

    if(DEFINED arg_INSTALL_DESTINATION)
        set(install_options "")
        if(DEFINED arg_INSTALL_COMPONENT)
            list(APPEND install_options COMPONENT "${arg_INSTALL_COMPONENT}")
        endif()
        if(arg_INSTALL_EXCLUDE_FROM_ALL)
            list(APPEND install_options EXCLUDE_FROM_ALL)
        endif()
        install(TARGETS ${target}
            LIBRARY DESTINATION "${arg_INSTALL_DESTINATION}" ${install_options}
            RUNTIME DESTINATION "${arg_INSTALL_DESTINATION}" ${install_options})
        install(FILES "${manifest}" DESTINATION "${arg_INSTALL_DESTINATION}" ${install_options})
        if(DEFINED arg_TEST_VECTORS)
            install(FILES "${vectors_source}" DESTINATION "${arg_INSTALL_DESTINATION}"
                ${install_options})
        endif()
    endif()
endfunction()
