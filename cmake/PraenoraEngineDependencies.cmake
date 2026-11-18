# What the finders of the service providers' engines share (cmake/PraenoraRoadrunner.cmake and
# cmake/PraenoraAntimony.cmake): the error for a dependency that is missing, the status line of one
# that was found, and the repair of imported targets on macOS.
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
