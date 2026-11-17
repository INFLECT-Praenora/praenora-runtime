# Versions owned by the build.
#
# - The runtime package version is the VERSION of project() in the top-level CMakeLists.txt.
# - The SDK, protocol (PMP) and document format versions are defined here. Each document kind has
#   a format version of its own: PRAENORA_FORMAT_VERSIONS lists "<kind>=<version>" for every kind
#   that carries a format_version, and PRAENORA_FORMAT_VERSION_<kind> holds each one's version.
# - The module ABI version is owned by include/praenora/sdk/abi.h, and the provider interface
#   version by src/runtime/provider/provider_abi.h; this file parses both. Without a header in the
#   tree, its version is 0.0.

include_guard(GLOBAL)

set(PRAENORA_RUNTIME_VERSION "${PROJECT_VERSION}")
set(PRAENORA_SDK_VERSION "0.2.0")
set(PRAENORA_PMP_VERSION "0.1.0")
set(PRAENORA_FORMAT_VERSIONS
    "capability-definition=0.1.0"
    "message-type=0.1.0"
    "module-check-report=0.1.0"
    "module-manifest=0.2.0"
    "module-test-vectors=0.1.0"
    "provider-manifest=0.1.0"
    "run-record=0.2.0"
    "status-snapshot=0.1.0"
    "workflow-config=0.1.0"
    "workflow-definition=0.1.0")
foreach(_praenora_format IN LISTS PRAENORA_FORMAT_VERSIONS)
    if(NOT _praenora_format MATCHES "^([a-z][a-z0-9-]*)=([0-9]+\\.[0-9]+\\.[0-9]+)$")
        message(FATAL_ERROR "PRAENORA_FORMAT_VERSIONS: '${_praenora_format}' is not "
            "<kind>=<major>.<minor>.<patch>")
    endif()
    set(PRAENORA_FORMAT_VERSION_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
# The format of the module manifests praenora_add_module() writes (cmake/PraenoraModule.cmake).
set(PRAENORA_MODULE_MANIFEST_FORMAT_VERSION "${PRAENORA_FORMAT_VERSION_module-manifest}")

# _praenora_header_version(<header> <macro prefix> <variable prefix>)
#
# Sets <variable prefix>_MAJOR and <variable prefix>_MINOR from the lines
# "#define <macro prefix>_MAJOR <n>" and "#define <macro prefix>_MINOR <n>" of <header>, or to 0
# when the header does not exist. The configuration is re-run when the header changes.
function(_praenora_header_version header macro_prefix variable_prefix)
    if(NOT EXISTS "${header}")
        set(${variable_prefix}_MAJOR 0 PARENT_SCOPE)
        set(${variable_prefix}_MINOR 0 PARENT_SCOPE)
        return()
    endif()
    foreach(part IN ITEMS MAJOR MINOR)
        file(STRINGS "${header}" line
            REGEX "^#[ \t]*define[ \t]+${macro_prefix}_${part}[ \t]+[0-9]+")
        if(NOT line MATCHES "${macro_prefix}_${part}[ \t]+([0-9]+)")
            message(FATAL_ERROR "Cannot read ${macro_prefix}_${part} from ${header}")
        endif()
        set(${variable_prefix}_${part} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endforeach()
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${header}")
endfunction()

_praenora_header_version("${PROJECT_SOURCE_DIR}/include/praenora/sdk/abi.h"
    PRAENORA_ABI PRAENORA_ABI)
_praenora_header_version("${PROJECT_SOURCE_DIR}/src/runtime/provider/provider_abi.h"
    PRAENORA_PROVIDER_ABI PRAENORA_PROVIDER_ABI)

list(JOIN PRAENORA_FORMAT_VERSIONS ", " _praenora_formats_text)
message(STATUS "Praenora: runtime ${PRAENORA_RUNTIME_VERSION}, SDK ${PRAENORA_SDK_VERSION}, "
    "PMP ${PRAENORA_PMP_VERSION}, ABI ${PRAENORA_ABI_MAJOR}.${PRAENORA_ABI_MINOR}, "
    "provider ABI ${PRAENORA_PROVIDER_ABI_MAJOR}.${PRAENORA_PROVIDER_ABI_MINOR}, "
    "document formats ${_praenora_formats_text}")
unset(_praenora_formats_text)
