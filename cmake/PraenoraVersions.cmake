# Versions owned by the build.
#
# - The runtime package version is the VERSION of project() in the top-level CMakeLists.txt.
# - The SDK, protocol (PMP) and document format versions are defined here.
# - The module ABI version is owned by include/praenora/sdk/abi.h, which this file parses. Without
#   that header in the tree, the ABI version is 0.0.

include_guard(GLOBAL)

set(PRAENORA_RUNTIME_VERSION "${PROJECT_VERSION}")
set(PRAENORA_SDK_VERSION "0.1.0")
set(PRAENORA_PMP_VERSION "0.1.0")
set(PRAENORA_FORMAT_VERSION "0.1.0")

set(_praenora_abi_header "${PROJECT_SOURCE_DIR}/include/praenora/sdk/abi.h")
if(EXISTS "${_praenora_abi_header}")
    foreach(_praenora_part IN ITEMS MAJOR MINOR)
        file(STRINGS "${_praenora_abi_header}" _praenora_line
            REGEX "^#[ \t]*define[ \t]+PRAENORA_ABI_${_praenora_part}[ \t]+[0-9]+")
        if(NOT _praenora_line MATCHES "PRAENORA_ABI_${_praenora_part}[ \t]+([0-9]+)")
            message(FATAL_ERROR
                "Cannot read PRAENORA_ABI_${_praenora_part} from ${_praenora_abi_header}")
        endif()
        set(PRAENORA_ABI_${_praenora_part} "${CMAKE_MATCH_1}")
    endforeach()
    # Re-run the configuration when the ABI version changes.
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${_praenora_abi_header}")
else()
    set(PRAENORA_ABI_MAJOR 0)
    set(PRAENORA_ABI_MINOR 0)
endif()

message(STATUS "Praenora: runtime ${PRAENORA_RUNTIME_VERSION}, SDK ${PRAENORA_SDK_VERSION}, "
    "PMP ${PRAENORA_PMP_VERSION}, ABI ${PRAENORA_ABI_MAJOR}.${PRAENORA_ABI_MINOR}, "
    "document formats ${PRAENORA_FORMAT_VERSION}")
