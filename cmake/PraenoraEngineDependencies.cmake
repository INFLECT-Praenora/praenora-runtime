# What the finders of the service providers' engines share (cmake/PraenoraRoadrunner.cmake and
# cmake/PraenoraAntimony.cmake): the error for a dependency that is missing, the status line of one
# that was found, the repair of imported targets on macOS, and what libSBML's static package needs
# on Windows.
#
# praenora_engine_dependency_missing(<option> <provider> <what> <how>)
#
#   Stops the configuration with a FATAL_ERROR: <option> is ON, but <what> was not found. <how>
#   says what to do; the message points to docs/platforms.md, which lists what the provider
#   <provider> needs, and says that nothing is downloaded.
#
# praenora_engine_dependency_found(<label> <version> <where>)
#
#   Prints the status line "Praenora: dependency <label> <version> (found at <where>)", once per
#   configuration: a dependency that both providers' finders find (libSBML) is reported once.
#
# praenora_engine_drop_system_libraries(<target>...)
#
#   On macOS, removes the system libraries m, dl and pthread, which several packages list and which
#   are all part of libSystem, from the INTERFACE_LINK_LIBRARIES of the targets that exist, so the
#   linker does not warn about duplicate libraries. Elsewhere it does nothing.
#
# praenora_engine_libsbml_package(<out>)
#
#   Sets <out> to the name of libSBML's static CMake package, which is also the name of its target:
#   libsbml-static on Windows and sbml-static elsewhere, as libroadrunner-deps' build names them.
#
# praenora_engine_expat_for_libsbml()
#
#   On Windows, sets EXPAT_LIBRARY and EXPAT_INCLUDE_DIR in the caller's scope from the target
#   expat::expat, which must exist; it is called before libSBML's package is loaded. That package
#   finds expat with a FindEXPAT module (libSBML's own, which searches the names expat and libexpat
#   only, where it still exists, else CMake's), which need not find libroadrunner-deps' expat on
#   Windows (libexpatMD, or libexpatdMD for Debug), and which uses these two variables when they are
#   set, as libRoadRunner's and libAntimony's own builds set them. Elsewhere it does nothing.
#
# praenora_engine_libsbml_static(<target>)
#
#   On Windows, appends LIBSBML_STATIC and LIBLAX_STATIC to the INTERFACE_COMPILE_DEFINITIONS of
#   libSBML's target <target>, unless they are there already: without them libSBML's headers
#   declare its functions __declspec(dllimport), and a call then refers to a name that only the
#   import library of a DLL defines, not the static library. Elsewhere it does nothing.

include_guard(GLOBAL)

function(praenora_engine_dependency_missing option provider what how)
    message(FATAL_ERROR "${option} is ON, but ${what} was not found. ${how} "
        "docs/platforms.md lists what the ${provider} provider needs. Nothing is downloaded.")
endfunction()

function(praenora_engine_dependency_found label version where)
    set(line "Praenora: dependency ${label} ${version} (found at ${where})")
    get_property(printed GLOBAL PROPERTY PRAENORA_ENGINE_DEPENDENCY_LINES)
    foreach(earlier IN LISTS printed)
        if(earlier STREQUAL line)
            return()
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY PRAENORA_ENGINE_DEPENDENCY_LINES "${line}")
    message(STATUS "${line}")
endfunction()

function(praenora_engine_drop_system_libraries)
    if(NOT APPLE)
        return()
    endif()
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(libraries ${target} INTERFACE_LINK_LIBRARIES)
        if(NOT libraries)
            continue()
        endif()
        list(REMOVE_ITEM libraries m dl pthread)
        set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${libraries}")
    endforeach()
endfunction()

function(praenora_engine_libsbml_package out)
    if(WIN32)
        set(${out} libsbml-static PARENT_SCOPE)
    else()
        set(${out} sbml-static PARENT_SCOPE)
    endif()
endfunction()

function(praenora_engine_expat_for_libsbml)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(library expat::expat LOCATION)
    get_target_property(include_dir expat::expat INTERFACE_INCLUDE_DIRECTORIES)
    set(EXPAT_LIBRARY "${library}" PARENT_SCOPE)
    set(EXPAT_INCLUDE_DIR "${include_dir}" PARENT_SCOPE)
endfunction()

function(praenora_engine_libsbml_static target)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(definitions ${target} INTERFACE_COMPILE_DEFINITIONS)
    if(NOT definitions)
        set(definitions "")
    endif()
    foreach(definition IN ITEMS LIBSBML_STATIC LIBLAX_STATIC)
        if(NOT definition IN_LIST definitions)
            set_property(TARGET ${target} APPEND PROPERTY
                INTERFACE_COMPILE_DEFINITIONS ${definition})
        endif()
    endforeach()
endfunction()
