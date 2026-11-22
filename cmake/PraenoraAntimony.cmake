# praenora_find_antimony()
#
# Makes libAntimony's static library, with the SBMLNetwork and libSBML it was built against,
# available to the antimony service provider (src/providers/antimony), when PRAENORA_WITH_ANTIMONY
# is ON. Nothing is ever downloaded: libAntimony and its dependencies come from installations
# given to CMake (docs/platforms.md).
#
# - libAntimony: its header antimony_api.h (find_path) and its static library antimony-static
#   (find_library; on Windows also libantimony-static, the name of its .lib there), searched on
#   CMAKE_PREFIX_PATH: its installation has no CMake package.
# - libSBML: find_package(sbml-static CONFIG) (libsbml-static on Windows), searched on
#   CMAKE_PREFIX_PATH, which must hold the directory of its package files (libRoadRunner's
#   installation keeps them directly in <prefix>/lib/cmake). The packages libSBML links are found as
#   libAntimony's own build finds them, each with find_package(... CONFIG), searched first beside
#   libSBML's package files and then on CMAKE_PREFIX_PATH: expat (expat::expat), which libSBML
#   always links, and zlib (its static component) and bzip2, which it links only when it was built
#   with them; those two are made available, not required. On Windows expat is found first, on
#   CMAKE_PREFIX_PATH, and its location is handed to libSBML's package, which cannot find it
#   otherwise (praenora_engine_expat_for_libsbml(), cmake/PraenoraEngineDependencies.cmake).
# - SBMLNetwork: its header sbmlnetwork/libsbmlnetwork_common.h (find_path) and its static library
#   sbmlnetwork-static (find_library; on Windows also libsbmlnetwork-staticmd and, for Debug,
#   libsbmlnetwork-staticmdd, the names of libroadrunner-deps' .lib there), on CMAKE_PREFIX_PATH: it
#   has no CMake package either.
# - A probe, cmake/praenora_antimony_probe.cpp, is built and run when the build is configured
#   (try_run). It links all of them as the provider does, which shows that they link together, and
#   prints the versions the libraries report: libAntimony's getVersionStr() (without its leading
#   "v"), libSBML's getLibSBMLDottedVersion() and SBMLNetwork's getLibraryVersion(); they are the
#   engine object's. It also prints whether libSBML has the comp package. libAntimony 3.3.x only
#   (a status line says that a version other than 3.3.0 is not verified) and a libSBML with comp;
#   anything else is a FATAL_ERROR.
#
# libAntimony and SBMLNetwork become the imported targets praenora::libantimony and
# praenora::sbmlnetwork. Repairs are made on imported targets only, never in the installations:
# on macOS, the system libraries m, dl and pthread are removed from the link interfaces, as for
# the roadrunner provider (cmake/PraenoraEngineDependencies.cmake); on Windows, libSBML's target
# gets the definitions LIBSBML_STATIC and LIBLAX_STATIC, as there. libAntimony's header declares
# its functions for import from a DLL on Windows unless LIB_EXPORTS is defined, with which calls
# refer to the names its static library defines: the probe is compiled with it there, and so are
# the provider's sources (src/providers/antimony/CMakeLists.txt). With PRAENORA_WITH_ROADRUNNER
# also ON, cmake/PraenoraRoadrunner.cmake has already loaded libSBML's package and the packages it
# links: their targets are used as they are, never defined twice.
#
# Afterwards these exist:
#   praenora_antimony_engine            INTERFACE target linking libAntimony, SBMLNetwork, libSBML
#                                       and expat
#   PRAENORA_ANTIMONY_VERSION           libAntimony's version, from the probe
#   PRAENORA_ANTIMONY_LIBSBML_VERSION   libSBML's version, from the probe
#   PRAENORA_SBMLNETWORK_VERSION        SBMLNetwork's version, from the probe
# One status line per dependency (libAntimony, libSBML, SBMLNetwork, expat) reports its version
# and where it came from.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraEngineDependencies.cmake")

set(_PRAENORA_ANTIMONY_PROBE "${CMAKE_CURRENT_LIST_DIR}/praenora_antimony_probe.cpp")

# _praenora_antimony_missing(<what> <how>)
function(_praenora_antimony_missing what how)
    praenora_engine_dependency_missing(PRAENORA_WITH_ANTIMONY antimony "${what}" "${how}")
endfunction()

macro(praenora_find_antimony)
    # libAntimony and SBMLNetwork, which have no package. Not cached, so that a changed
    # CMAKE_PREFIX_PATH is searched again.
    foreach(_praenora_ant_variable IN ITEMS _praenora_ant_include _praenora_ant_library
                                            _praenora_sbmlnetwork_include
                                            _praenora_sbmlnetwork_library)
        unset(${_praenora_ant_variable})
    endforeach()
    set(_praenora_ant_names antimony-static)
    set(_praenora_sbmlnetwork_names sbmlnetwork-static libsbmlnetwork-static)
    if(WIN32)
        # On Windows the static libraries keep the prefix lib, which CMake before 3.29 does not
        # try, and SBMLNetwork's name ends in md, or mdd for Debug (the MSVC runtime library).
        list(APPEND _praenora_ant_names libantimony-static)
        list(APPEND _praenora_sbmlnetwork_names libsbmlnetwork-staticmd libsbmlnetwork-staticmdd)
    endif()
    find_path(_praenora_ant_include NAMES antimony_api.h NO_CACHE)
    find_library(_praenora_ant_library NAMES ${_praenora_ant_names} NO_CACHE)
    if(NOT _praenora_ant_include OR NOT _praenora_ant_library)
        _praenora_antimony_missing("libAntimony (its header antimony_api.h and its static \
library antimony-static)" "Add the prefix of a libAntimony 3.3 installation built from source \
(which holds include/antimony_api.h and lib/libantimony-static.a) to CMAKE_PREFIX_PATH.")
    endif()
    find_path(_praenora_sbmlnetwork_include NAMES sbmlnetwork/libsbmlnetwork_common.h NO_CACHE)
    find_library(_praenora_sbmlnetwork_library NAMES ${_praenora_sbmlnetwork_names} NO_CACHE)
    if(NOT _praenora_sbmlnetwork_include OR NOT _praenora_sbmlnetwork_library)
        _praenora_antimony_missing("SBMLNetwork (its header sbmlnetwork/libsbmlnetwork_common.h \
and its static library sbmlnetwork-static), which libAntimony links" "Add the prefix that holds \
it, such as the installation of libRoadRunner's dependencies, to CMAKE_PREFIX_PATH.")
    endif()

    # libSBML's package. Loaded once with what is on CMAKE_PREFIX_PATH, to learn where its package
    # files are; the packages it links are then searched there first. Its targets file needs
    # expat::expat, which may not exist yet: it then reports the package as not found, but defines
    # the target, which is complete once expat is found below. On Windows the package cannot find
    # expat by itself, so expat is found first.
    praenora_engine_libsbml_package(_praenora_ant_sbml)
    if(NOT TARGET ${_praenora_ant_sbml})
        if(WIN32)
            find_package(expat CONFIG QUIET)
            if(NOT TARGET expat::expat)
                _praenora_antimony_missing("the CMake package expat (target expat::expat), which \
libSBML links" "Add the directory that holds its package files (for libRoadRunner's \
dependencies, <prefix>/lib/cmake or <prefix>/cmake) to CMAKE_PREFIX_PATH.")
            endif()
            praenora_engine_expat_for_libsbml()
        endif()
        find_package(${_praenora_ant_sbml} CONFIG QUIET)
    endif()
    set(_praenora_ant_sbml_dir "${${_praenora_ant_sbml}_DIR}")
    if(NOT _praenora_ant_sbml_dir OR NOT EXISTS "${_praenora_ant_sbml_dir}")
        _praenora_antimony_missing("libSBML's CMake package ${_praenora_ant_sbml}"
            "Add the directory that holds its package files (for libRoadRunner's dependencies, \
<prefix>/lib/cmake) to CMAKE_PREFIX_PATH.")
    endif()
    foreach(_praenora_ant_package IN ITEMS "expat|expat::expat|"
                                           "ZLIB||COMPONENTS;static"
                                           "bzip2||")
        string(REPLACE "|" ";" _praenora_ant_fields "${_praenora_ant_package}")
        list(GET _praenora_ant_fields 0 _praenora_ant_name)
        list(GET _praenora_ant_fields 1 _praenora_ant_target)
        list(SUBLIST _praenora_ant_fields 2 -1 _praenora_ant_extra)
        find_package(${_praenora_ant_name} CONFIG QUIET ${_praenora_ant_extra}
            HINTS "${_praenora_ant_sbml_dir}")
        if(_praenora_ant_target AND NOT TARGET ${_praenora_ant_target})
            _praenora_antimony_missing("the CMake package ${_praenora_ant_name} (target \
${_praenora_ant_target}), which libSBML links" "It is searched beside libSBML's package files \
(${_praenora_ant_sbml_dir}), then on CMAKE_PREFIX_PATH.")
        endif()
    endforeach()
    if(NOT TARGET ${_praenora_ant_sbml})
        find_package(${_praenora_ant_sbml} CONFIG QUIET HINTS "${_praenora_ant_sbml_dir}")
    endif()
    if(NOT TARGET ${_praenora_ant_sbml})
        _praenora_antimony_missing("the target ${_praenora_ant_sbml} of libSBML's CMake package \
(${_praenora_ant_sbml_dir})" "The libSBML installation is incomplete.")
    endif()
    praenora_engine_drop_system_libraries(expat::expat ${_praenora_ant_sbml})
    praenora_engine_libsbml_static(${_praenora_ant_sbml})

    # libAntimony and SBMLNetwork as imported targets, each linking what it needs.
    if(NOT TARGET praenora::sbmlnetwork)
        add_library(praenora::sbmlnetwork STATIC IMPORTED)
        set_target_properties(praenora::sbmlnetwork PROPERTIES
            IMPORTED_LOCATION "${_praenora_sbmlnetwork_library}"
            IMPORTED_LINK_INTERFACE_LANGUAGES CXX
            INTERFACE_INCLUDE_DIRECTORIES "${_praenora_sbmlnetwork_include}"
            INTERFACE_LINK_LIBRARIES "${_praenora_ant_sbml};${CMAKE_DL_LIBS}")
        praenora_engine_drop_system_libraries(praenora::sbmlnetwork)
    endif()
    if(NOT TARGET praenora::libantimony)
        add_library(praenora::libantimony STATIC IMPORTED)
        set_target_properties(praenora::libantimony PROPERTIES
            IMPORTED_LOCATION "${_praenora_ant_library}"
            IMPORTED_LINK_INTERFACE_LANGUAGES CXX
            INTERFACE_INCLUDE_DIRECTORIES "${_praenora_ant_include}"
            INTERFACE_LINK_LIBRARIES "praenora::sbmlnetwork;${_praenora_ant_sbml}")
    endif()

    # The probe: the libraries link together, and report their versions. On Windows it is compiled
    # with LIB_EXPORTS, as the provider's sources are. A multi-configuration generator builds it in
    # CMAKE_TRY_COMPILE_CONFIGURATION (Debug when that is not set), whose MSVC runtime library must
    # be the one the libraries were built with (the ci-windows-x64-* presets set it).
    set(_praenora_ant_probe_definitions "")
    if(WIN32)
        set(_praenora_ant_probe_definitions COMPILE_DEFINITIONS -DLIB_EXPORTS)
    endif()
    set(_praenora_ant_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/praenora-antimony-probe")
    unset(_praenora_ant_probe_exit CACHE)
    try_run(_praenora_ant_probe_exit _praenora_ant_probe_built
        "${_praenora_ant_probe_dir}" "${_PRAENORA_ANTIMONY_PROBE}"
        LINK_LIBRARIES praenora::libantimony
        ${_praenora_ant_probe_definitions}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        COMPILE_OUTPUT_VARIABLE _praenora_ant_probe_log
        RUN_OUTPUT_VARIABLE _praenora_ant_probe_output)
    if(NOT _praenora_ant_probe_built)
        message(FATAL_ERROR "PRAENORA_WITH_ANTIMONY is ON, but a program cannot be built with "
            "libAntimony (${_praenora_ant_library}), SBMLNetwork (${_praenora_sbmlnetwork_library})"
            " and libSBML (${_praenora_ant_sbml_dir}): they do not link together. "
            "docs/platforms.md says how to build libAntimony against the libSBML and SBMLNetwork "
            "it links. The build of ${_PRAENORA_ANTIMONY_PROBE} said:\n${_praenora_ant_probe_log}")
    endif()
    if(NOT _praenora_ant_probe_exit EQUAL 0)
        message(FATAL_ERROR "PRAENORA_WITH_ANTIMONY is ON, but the program built with "
            "libAntimony, SBMLNetwork and libSBML (${_PRAENORA_ANTIMONY_PROBE}) failed "
            "(${_praenora_ant_probe_exit}):\n${_praenora_ant_probe_output}")
    endif()
    foreach(_praenora_ant_pair IN ITEMS "libantimony|PRAENORA_ANTIMONY_VERSION"
                                        "libsbml|PRAENORA_ANTIMONY_LIBSBML_VERSION"
                                        "sbmlnetwork|PRAENORA_SBMLNETWORK_VERSION")
        string(REPLACE "|" ";" _praenora_ant_pair "${_praenora_ant_pair}")
        list(GET _praenora_ant_pair 0 _praenora_ant_key)
        list(GET _praenora_ant_pair 1 _praenora_ant_variable)
        set(${_praenora_ant_variable} "")
        if(_praenora_ant_probe_output MATCHES
           "(^|\n)${_praenora_ant_key}=v?([0-9]+\\.[0-9]+\\.[0-9]+)\r?(\n|$)")
            set(${_praenora_ant_variable} "${CMAKE_MATCH_2}")
        else()
            message(FATAL_ERROR "PRAENORA_WITH_ANTIMONY is ON, but the program built with "
                "libAntimony, SBMLNetwork and libSBML (${_PRAENORA_ANTIMONY_PROBE}) printed no "
                "version of ${_praenora_ant_key}:\n${_praenora_ant_probe_output}")
        endif()
    endforeach()
    # The provider relies on libAntimony's behavior as of 3.3 (its lexer, which the provider's
    # check of imports mirrors; its main module; its configuration functions): other releases are
    # refused.
    if(NOT PRAENORA_ANTIMONY_VERSION MATCHES "^3\\.3\\.")
        message(FATAL_ERROR "PRAENORA_WITH_ANTIMONY is ON, but libAntimony "
            "${PRAENORA_ANTIMONY_VERSION} was found (${_praenora_ant_library}); the antimony "
            "provider needs libAntimony 3.3.x.")
    endif()
    if(NOT _praenora_ant_probe_output MATCHES "(^|\n)comp=1")
        message(FATAL_ERROR "PRAENORA_WITH_ANTIMONY is ON, but libSBML "
            "${PRAENORA_ANTIMONY_LIBSBML_VERSION} (${_praenora_ant_sbml_dir}) has no comp "
            "package, which libAntimony needs. docs/platforms.md lists what the antimony provider "
            "needs.")
    endif()
    praenora_engine_dependency_found(libAntimony "${PRAENORA_ANTIMONY_VERSION}"
        "${_praenora_ant_library}")
    if(NOT PRAENORA_ANTIMONY_VERSION VERSION_EQUAL 3.3.0)
        message(STATUS "Praenora: libAntimony ${PRAENORA_ANTIMONY_VERSION} is not verified with "
            "the antimony provider (3.3.0 is)")
    endif()
    praenora_engine_dependency_found(libSBML "${PRAENORA_ANTIMONY_LIBSBML_VERSION}"
        "${_praenora_ant_sbml_dir}")
    praenora_engine_dependency_found(SBMLNetwork "${PRAENORA_SBMLNETWORK_VERSION}"
        "${_praenora_sbmlnetwork_library}")
    praenora_engine_dependency_found(expat "${expat_VERSION}" "${expat_DIR}")

    if(NOT TARGET praenora_antimony_engine)
        add_library(praenora_antimony_engine INTERFACE)
        target_link_libraries(praenora_antimony_engine INTERFACE praenora::libantimony)
    endif()
endmacro()
