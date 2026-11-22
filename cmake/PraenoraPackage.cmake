# The target package (CPack): an archive of the installation's default install component, which
# holds the license files of share/praenora/licenses/ and never the example modules (component
# Examples), even with PRAENORA_INSTALL_EXAMPLES. It writes package/<name>.tar.xz (.zip on
# Windows) in the build tree, with a top-level directory of the same name, and
# package/<name>.tar.xz.sha256 (.zip.sha256) beside it, in the format of sha256sum. There is no
# source package (below). The top-level CMakeLists.txt includes this file after
# cmake/PraenoraInstall.cmake, when the install rules are generated and praenora-runtime is the
# top-level project.
#
# The archive's name is praenora-runtime-<version>-<os>-<arch>-<compiler>-<config>, the scheme of
# the dependency bundles of praenora-runtime-deps:
#   <os>, <arch>   PRAENORA_PLATFORM_OS and PRAENORA_PLATFORM_ARCH
#                  (cmake/PraenoraCompilerChecks.cmake)
#   <compiler>     appleclang<major>, clang<major>, gcc<major> or msvc<toolset>
#                  (MSVC_TOOLSET_VERSION, 143 for Visual Studio 2022). With another compiler
#                  the build goes on (cmake/PraenoraCompilerChecks.cmake only warns about it), but
#                  the target package stops, naming it
#   <config>       release or debug: the packaged configuration. A single-configuration generator
#                  packages CMAKE_BUILD_TYPE; a multi-configuration one the configuration that
#                  cpack -C names, which the target package (PACKAGE with Visual Studio) passes.
#                  At package time the generated PraenoraCPackProjectConfig.cmake
#                  (CPACK_PROJECT_CONFIG_FILE) sets the name, and stops cpack for another
#                  configuration than Release or Debug, or one that is not the tree's build type,
#                  for another compiler, and for the source configuration.
#
# Afterwards these exist:
#   PRAENORA_PACKAGE_NAME_PREFIX    praenora-runtime-<version>-<os>-<arch>-<compiler>; empty with
#                                   another compiler
#   PRAENORA_PACKAGE_CONFIG_LABEL   release or debug when CMAKE_BUILD_TYPE is Release or Debug and
#                                   the generator has one configuration; empty otherwise
#   PRAENORA_PACKAGE_NAME           <prefix>-<config label>; with the label still unknown,
#                                   <prefix>-<config>, as a pattern; empty with another compiler

include_guard(GLOBAL)

string(REGEX MATCH "^[0-9]+" _praenora_package_compiler_major "${CMAKE_CXX_COMPILER_VERSION}")
set(_praenora_package_compiler "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(_praenora_package_compiler "appleclang${_praenora_package_compiler_major}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(_praenora_package_compiler "clang${_praenora_package_compiler_major}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_praenora_package_compiler "gcc${_praenora_package_compiler_major}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MSVC_TOOLSET_VERSION)
    set(_praenora_package_compiler "msvc${MSVC_TOOLSET_VERSION}")
endif()
set(_praenora_package_compiler_found "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

set(PRAENORA_PACKAGE_NAME_PREFIX "")
if(NOT _praenora_package_compiler STREQUAL "")
    set(PRAENORA_PACKAGE_NAME_PREFIX "praenora-runtime-${PROJECT_VERSION}-\
${PRAENORA_PLATFORM_OS}-${PRAENORA_PLATFORM_ARCH}-${_praenora_package_compiler}")
endif()

get_property(_praenora_package_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
string(TOLOWER "${CMAKE_BUILD_TYPE}" _praenora_package_build_type)
set(PRAENORA_PACKAGE_CONFIG_LABEL "")
if(NOT _praenora_package_multi_config AND _praenora_package_build_type MATCHES "^(release|debug)$")
    set(PRAENORA_PACKAGE_CONFIG_LABEL "${_praenora_package_build_type}")
endif()
if(PRAENORA_PACKAGE_NAME_PREFIX STREQUAL "")
    set(PRAENORA_PACKAGE_NAME "")
    message(STATUS "Praenora: package unavailable: an archive is named after its compiler, "
        "AppleClang, Clang, GCC or MSVC, so the target package stops for "
        "${_praenora_package_compiler_found}")
elseif(PRAENORA_PACKAGE_CONFIG_LABEL)
    set(PRAENORA_PACKAGE_NAME "${PRAENORA_PACKAGE_NAME_PREFIX}-${PRAENORA_PACKAGE_CONFIG_LABEL}")
    message(STATUS "Praenora: package ${PRAENORA_PACKAGE_NAME}")
else()
    set(PRAENORA_PACKAGE_NAME "${PRAENORA_PACKAGE_NAME_PREFIX}-<config>")
    if(_praenora_package_multi_config)
        message(STATUS "Praenora: package ${PRAENORA_PACKAGE_NAME}, <config> being release or "
            "debug: the configuration packaged (cpack -C)")
    else()
        message(STATUS "Praenora: package ${PRAENORA_PACKAGE_NAME}: the target package needs "
            "CMAKE_BUILD_TYPE Release or Debug, not '${CMAKE_BUILD_TYPE}'")
    endif()
endif()

# Read by cpack once per generator, after CPackConfig.cmake (or the source configuration, below):
# CPACK_BUILD_CONFIG holds the configurations of cpack -C, if given.
set(_praenora_package_project_config "${PROJECT_BINARY_DIR}/cmake/PraenoraCPackProjectConfig.cmake")
file(CONFIGURE OUTPUT "${_praenora_package_project_config}" CONTENT [==[
# Generated by cmake/PraenoraPackage.cmake. cpack includes this file once per generator, after
# CPackConfig.cmake, to name the archive after the configuration it packages.
set(_praenora_package_prefix [=[@PRAENORA_PACKAGE_NAME_PREFIX@]=])
set(_praenora_package_compiler_found [=[@_praenora_package_compiler_found@]=])
set(_praenora_package_multi_config [=[@_praenora_package_multi_config@]=])
set(_praenora_package_build_type [=[@CMAKE_BUILD_TYPE@]=])
# The source configuration installs no project.
if(CPACK_INSTALL_CMAKE_PROJECTS STREQUAL "")
    message(FATAL_ERROR "Praenora: there is no source package; the target package archives an "
        "installation, and a source archive is made from the repository (git archive).")
endif()
if(_praenora_package_prefix STREQUAL "")
    message(FATAL_ERROR "Praenora: an archive is named after its compiler, which must be "
        "AppleClang, Clang, GCC or MSVC with a known toolset; this tree's is "
        "${_praenora_package_compiler_found}.")
endif()
if(_praenora_package_multi_config)
    set(_praenora_package_config "${CPACK_BUILD_CONFIG}")
    if(_praenora_package_config STREQUAL "")
        message(FATAL_ERROR "Praenora: name the configuration to package with cpack -C, "
            "Release or Debug; the target package (PACKAGE with Visual Studio) passes the "
            "configuration it builds.")
    endif()
else()
    set(_praenora_package_config "${_praenora_package_build_type}")
    string(TOLOWER "${CPACK_BUILD_CONFIG}" _praenora_package_given)
    string(TOLOWER "${_praenora_package_build_type}" _praenora_package_built)
    if(NOT _praenora_package_given STREQUAL "" AND
       NOT _praenora_package_given STREQUAL _praenora_package_built)
        message(FATAL_ERROR "Praenora: this build tree is built as '${_praenora_package_config}', "
            "but cpack -C names '${CPACK_BUILD_CONFIG}'.")
    endif()
endif()
string(TOLOWER "${_praenora_package_config}" _praenora_package_label)
if(NOT _praenora_package_label MATCHES "^(release|debug)$")
    message(FATAL_ERROR "Praenora: an archive holds one configuration, Release or Debug, not "
        "'${_praenora_package_config}'.")
endif()
set(CPACK_PACKAGE_FILE_NAME "${_praenora_package_prefix}-${_praenora_package_label}")
]==] @ONLY)

if(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME)
    set(_praenora_package_component "${CMAKE_INSTALL_DEFAULT_COMPONENT_NAME}")
else()
    set(_praenora_package_component "Unspecified")
endif()

set(CPACK_PACKAGE_NAME "praenora-runtime")
set(CPACK_PACKAGE_VENDOR "The Praenora developers")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
# The name that PraenoraCPackProjectConfig.cmake sets, when it is known now.
if(NOT PRAENORA_PACKAGE_NAME_PREFIX STREQUAL "" AND PRAENORA_PACKAGE_CONFIG_LABEL)
    set(CPACK_PACKAGE_FILE_NAME "${PRAENORA_PACKAGE_NAME}")
else()
    set(CPACK_PACKAGE_FILE_NAME "praenora-runtime-${PROJECT_VERSION}")
endif()
set(CPACK_PROJECT_CONFIG_FILE "${_praenora_package_project_config}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/package")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TXZ)
endif()
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
# One project, one component, monolithic: what cmake --install --component <default> installs.
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${PROJECT_BINARY_DIR};${PROJECT_NAME};${_praenora_package_component};/")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_THREADS 0)
# No source package: a source archive is made from the repository. include(CPack) always writes a
# source configuration; an empty CPACK_SOURCE_GENERATOR counts as unset there and adds
# CPACK_SOURCE_<generator> options to the cache, so it gets the binary generator instead. The
# configuration goes to cmake/ of the build tree, not to CPackSourceConfig.cmake at its top, where
# CMake looks for it to define the target package_source: so there is no such target, and
# PraenoraCPackProjectConfig.cmake stops cpack if it is given that configuration all the same.
set(CPACK_SOURCE_GENERATOR "${CPACK_GENERATOR}")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "${PROJECT_BINARY_DIR}/cmake/CPackSourceConfig.cmake")
file(REMOVE "${PROJECT_BINARY_DIR}/CPackSourceConfig.cmake")
set(CPACK_VERBATIM_VARIABLES ON)
include(CPack)
