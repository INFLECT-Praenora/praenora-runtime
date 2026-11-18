# praenora_find_roadrunner()
#
# Makes libRoadRunner's static C++ library available to the roadrunner service provider
# (src/providers/roadrunner), when PRAENORA_WITH_ROADRUNNER is ON. Nothing is ever downloaded:
# libRoadRunner and LLVM come from installations given to CMake (docs/platforms.md).
#
# - libRoadRunner: find_package(roadrunner-static CONFIG), searched on CMAKE_PREFIX_PATH (or
#   roadrunner-static_DIR). Its installation keeps the package files directly in <prefix>/lib/cmake,
#   so that directory must be on CMAKE_PREFIX_PATH (the prefix itself is not enough).
# - The packages that roadrunner-static links, and only those (zlib, expat, libSBML, rr-libstruct,
#   CLAPACK, NLEQ1, NLEQ2, POCO Foundation, Net and XML, SUNDIALS), each with
#   find_package(... CONFIG), searched first beside roadrunner-static's package files (where
#   libRoadRunner's installation has them; POCO's in lib/cmake/Poco) and then on
#   CMAKE_PREFIX_PATH. libRoadRunner's own helper, ImportRoadrunnerAndDependencies.cmake, also
#   finds bzip2, Threads, LibLZMA and the shared roadrunner and roadrunner_c_api packages, which
#   the static library does not use; they are not looked up.
# - LLVM: the installation at LLVM_INSTALL_PREFIX (the variable libRoadRunner's own FindLLVM.cmake
#   reads), or else the one whose llvm-config is found on CMAKE_PREFIX_PATH. Its llvm-config gives
#   the version. The libraries are not taken from llvm-config: roadrunner-static lists them itself.
# - Versions: libRoadRunner 2.11.x only (a status line says that a version other than 2.11.0 is not
#   verified), and LLVM 13, which libRoadRunner 2.11 is built against; anything else is a
#   FATAL_ERROR.
#
# libRoadRunner's packages need three repairs, made here on the imported targets, never in the
# installation:
# - roadrunner-static's link interface names LLVM's static libraries by the absolute paths of the
#   machine that built it. Every absolute path that does not exist is replaced by the library of
#   the same file name in the LLVM installation's lib directory; a library that is not there is
#   a FATAL_ERROR.
# - On macOS, the system libraries m, dl and pthread, which several of the packages list and which
#   are all part of libSystem, are removed from the link interfaces, so the linker does not warn
#   about duplicate libraries.
# - zlib's package is asked for its static component only (it ships no shared library).
#
# Afterwards these exist:
#   praenora_roadrunner_engine     INTERFACE target linking roadrunner-static::roadrunner-static
#   PRAENORA_ROADRUNNER_VERSION    libRoadRunner's version, from its rr/rrVersionInfo.h
#   PRAENORA_LLVM_VERSION          LLVM's version, from llvm-config --version
#   PRAENORA_SUNDIALS_VERSION      SUNDIALS's version, from its package
#   PRAENORA_LIBSBML_VERSION       libSBML's version, from its package
# One status line per dependency reports its version and where it came from. The messages and the
# repair are those that cmake/PraenoraEngineDependencies.cmake gives every engine's finder.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PraenoraEngineDependencies.cmake")

set(LLVM_INSTALL_PREFIX "" CACHE PATH
    "LLVM installation that libRoadRunner was built with (PRAENORA_WITH_ROADRUNNER)")

# _praenora_roadrunner_missing(<what> <how>)
function(_praenora_roadrunner_missing what how)
    praenora_engine_dependency_missing(PRAENORA_WITH_ROADRUNNER roadrunner "${what}" "${how}")
endfunction()

macro(praenora_find_roadrunner)
    # libRoadRunner itself.
    find_package(roadrunner-static CONFIG QUIET)
    if(NOT TARGET roadrunner-static::roadrunner-static)
        _praenora_roadrunner_missing("libRoadRunner's CMake package roadrunner-static"
            "Add the lib/cmake directory of a libRoadRunner 2.11 installation (for example \
<prefix>/lib/cmake) to CMAKE_PREFIX_PATH, or set roadrunner-static_DIR to it.")
    endif()
    set(_praenora_rr_cmake_dir "${roadrunner-static_DIR}")
    get_target_property(_praenora_rr_includes roadrunner-static::roadrunner-static
        INTERFACE_INCLUDE_DIRECTORIES)
    set(_praenora_rr_version_header "")
    foreach(_praenora_rr_include IN LISTS _praenora_rr_includes)
        if(EXISTS "${_praenora_rr_include}/rr/rrVersionInfo.h")
            set(_praenora_rr_version_header "${_praenora_rr_include}/rr/rrVersionInfo.h")
            break()
        endif()
    endforeach()
    if(NOT _praenora_rr_version_header)
        _praenora_roadrunner_missing("rr/rrVersionInfo.h in roadrunner-static's include \
directories (${_praenora_rr_includes})" "The libRoadRunner installation is incomplete.")
    endif()
    file(STRINGS "${_praenora_rr_version_header}" _praenora_rr_version_lines
        REGEX "^#define RR_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
    set(PRAENORA_ROADRUNNER_VERSION "")
    foreach(_praenora_rr_part IN ITEMS MAJOR MINOR PATCH)
        foreach(_praenora_rr_line IN LISTS _praenora_rr_version_lines)
            if(_praenora_rr_line MATCHES "^#define RR_VERSION_${_praenora_rr_part} ([0-9]+)$")
                list(APPEND PRAENORA_ROADRUNNER_VERSION "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endforeach()
    list(LENGTH PRAENORA_ROADRUNNER_VERSION _praenora_rr_parts)
    if(NOT _praenora_rr_parts EQUAL 3)
        _praenora_roadrunner_missing("the version in ${_praenora_rr_version_header}"
            "The libRoadRunner installation is incomplete.")
    endif()
    list(JOIN PRAENORA_ROADRUNNER_VERSION "." PRAENORA_ROADRUNNER_VERSION)
    # The provider relies on libRoadRunner's internals as of 2.11 (its configuration keys, how its
    # reset and simulate treat events): other releases are refused.
    if(NOT PRAENORA_ROADRUNNER_VERSION MATCHES "^2\\.11\\.")
        message(FATAL_ERROR "PRAENORA_WITH_ROADRUNNER is ON, but libRoadRunner "
            "${PRAENORA_ROADRUNNER_VERSION} was found (${_praenora_rr_cmake_dir}); the roadrunner "
            "provider needs libRoadRunner 2.11.x.")
    endif()
    praenora_engine_dependency_found(libRoadRunner "${PRAENORA_ROADRUNNER_VERSION}"
        "${_praenora_rr_cmake_dir}")
    if(NOT PRAENORA_ROADRUNNER_VERSION VERSION_EQUAL 2.11.0)
        message(STATUS "Praenora: libRoadRunner ${PRAENORA_ROADRUNNER_VERSION} is not verified "
            "with the roadrunner provider (2.11.0 is)")
    endif()

    # The packages roadrunner-static links, searched first where libRoadRunner keeps them.
    set(_praenora_rr_hints "${_praenora_rr_cmake_dir}" "${_praenora_rr_cmake_dir}/Poco")
    foreach(_praenora_rr_package IN ITEMS
            "ZLIB|ZLIB::ZLIBSTATIC|COMPONENTS;static"
            "expat|expat::expat|"
            "sbml-static|sbml-static|"
            "rr-libstruct|rr-libstruct::rr-libstruct-static|"
            "clapack|clapack::lapack|"
            "nleq1|nleq1::nleq1-static|"
            "nleq2|nleq2::nleq2-static|"
            "PocoFoundation|Poco::Foundation|"
            "PocoNet|Poco::Net|"
            "PocoXML|Poco::XML|"
            "SUNDIALS|SUNDIALS::cvodes_static|")
        string(REPLACE "|" ";" _praenora_rr_fields "${_praenora_rr_package}")
        list(GET _praenora_rr_fields 0 _praenora_rr_name)
        list(GET _praenora_rr_fields 1 _praenora_rr_target)
        list(SUBLIST _praenora_rr_fields 2 -1 _praenora_rr_extra)
        find_package(${_praenora_rr_name} CONFIG QUIET ${_praenora_rr_extra}
            HINTS ${_praenora_rr_hints})
        if(NOT TARGET ${_praenora_rr_target})
            _praenora_roadrunner_missing("the CMake package ${_praenora_rr_name} (target \
${_praenora_rr_target}), which libRoadRunner's static library links"
                "libRoadRunner's binary installation ships it in lib/cmake; add that directory \
(and lib/cmake/Poco for POCO) to CMAKE_PREFIX_PATH.")
        endif()
    endforeach()
    set(PRAENORA_SUNDIALS_VERSION "${SUNDIALS_VERSION}")
    set(PRAENORA_LIBSBML_VERSION "${sbml-static_VERSION}")
    if(NOT PRAENORA_SUNDIALS_VERSION OR NOT PRAENORA_LIBSBML_VERSION)
        _praenora_roadrunner_missing("the versions of SUNDIALS and libSBML in their packages"
            "The libRoadRunner installation is incomplete.")
    endif()
    praenora_engine_dependency_found(SUNDIALS "${PRAENORA_SUNDIALS_VERSION}" "${SUNDIALS_DIR}")
    praenora_engine_dependency_found(libSBML "${PRAENORA_LIBSBML_VERSION}" "${sbml-static_DIR}")

    # LLVM: LLVM_INSTALL_PREFIX, else an llvm-config on CMAKE_PREFIX_PATH. Not cached, so a changed
    # LLVM_INSTALL_PREFIX is searched again (a cache entry of an older tree is removed first).
    unset(PRAENORA_LLVM_CONFIG CACHE)
    unset(PRAENORA_LLVM_CONFIG)
    if(LLVM_INSTALL_PREFIX)
        find_program(PRAENORA_LLVM_CONFIG NAMES llvm-config
            HINTS "${LLVM_INSTALL_PREFIX}/bin" NO_DEFAULT_PATH NO_CACHE)
    else()
        find_program(PRAENORA_LLVM_CONFIG NAMES llvm-config NO_SYSTEM_ENVIRONMENT_PATH
            NO_CMAKE_SYSTEM_PATH NO_CACHE)
    endif()
    if(NOT PRAENORA_LLVM_CONFIG)
        _praenora_roadrunner_missing("llvm-config of the LLVM installation libRoadRunner was \
built with (LLVM_INSTALL_PREFIX is '${LLVM_INSTALL_PREFIX}')"
            "Set LLVM_INSTALL_PREFIX to that installation (for libRoadRunner 2.11.0, LLVM 13).")
    endif()
    execute_process(COMMAND "${PRAENORA_LLVM_CONFIG}" --version
        RESULT_VARIABLE _praenora_llvm_result
        OUTPUT_VARIABLE PRAENORA_LLVM_VERSION
        ERROR_VARIABLE _praenora_llvm_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _praenora_llvm_result EQUAL 0 OR
       NOT PRAENORA_LLVM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "PRAENORA_WITH_ROADRUNNER is ON, but ${PRAENORA_LLVM_CONFIG} "
            "--version failed (${_praenora_llvm_result}: ${_praenora_llvm_error}"
            "${PRAENORA_LLVM_VERSION}). It must be executable (chmod a+x).")
    endif()
    # libRoadRunner 2.11 is built against LLVM 13: another LLVM's libraries of the same names would
    # link, but not work.
    if(NOT PRAENORA_LLVM_VERSION MATCHES "^13\\.")
        message(FATAL_ERROR "PRAENORA_WITH_ROADRUNNER is ON, but the LLVM at "
            "${PRAENORA_LLVM_CONFIG} is ${PRAENORA_LLVM_VERSION}; libRoadRunner "
            "${PRAENORA_ROADRUNNER_VERSION} needs LLVM 13 (LLVM_INSTALL_PREFIX).")
    endif()
    get_filename_component(_praenora_llvm_prefix "${PRAENORA_LLVM_CONFIG}" DIRECTORY)
    get_filename_component(_praenora_llvm_prefix "${_praenora_llvm_prefix}" DIRECTORY)
    set(_praenora_llvm_lib_dir "${_praenora_llvm_prefix}/lib")
    praenora_engine_dependency_found(LLVM "${PRAENORA_LLVM_VERSION}" "${_praenora_llvm_prefix}")

    # roadrunner-static's absolute library paths that do not exist: the LLVM installation's.
    get_target_property(_praenora_rr_libraries roadrunner-static::roadrunner-static
        INTERFACE_LINK_LIBRARIES)
    set(_praenora_rr_relinked "")
    set(_praenora_rr_unknown "")
    foreach(_praenora_rr_library IN LISTS _praenora_rr_libraries)
        if(IS_ABSOLUTE "${_praenora_rr_library}" AND NOT EXISTS "${_praenora_rr_library}")
            get_filename_component(_praenora_rr_file "${_praenora_rr_library}" NAME)
            if(EXISTS "${_praenora_llvm_lib_dir}/${_praenora_rr_file}")
                set(_praenora_rr_library "${_praenora_llvm_lib_dir}/${_praenora_rr_file}")
            else()
                list(APPEND _praenora_rr_unknown "${_praenora_rr_library}")
            endif()
        endif()
        list(APPEND _praenora_rr_relinked "${_praenora_rr_library}")
    endforeach()
    if(_praenora_rr_unknown)
        list(JOIN _praenora_rr_unknown "\n  " _praenora_rr_unknown)
        message(FATAL_ERROR "PRAENORA_WITH_ROADRUNNER is ON, but roadrunner-static links "
            "libraries that do not exist, and ${_praenora_llvm_lib_dir} (the LLVM installation, "
            "LLVM_INSTALL_PREFIX) has no library of their names:\n  ${_praenora_rr_unknown}\n"
            "Set LLVM_INSTALL_PREFIX to the LLVM installation libRoadRunner was built with.")
    endif()
    set_target_properties(roadrunner-static::roadrunner-static PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_praenora_rr_relinked}")
    praenora_engine_drop_system_libraries(roadrunner-static::roadrunner-static
        clapack::blas clapack::lapack clapack::f2c expat::expat sbml-static)

    if(NOT TARGET praenora_roadrunner_engine)
        add_library(praenora_roadrunner_engine INTERFACE)
        target_link_libraries(praenora_roadrunner_engine INTERFACE
            roadrunner-static::roadrunner-static)
    endif()
endmacro()
