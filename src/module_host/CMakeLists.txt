# The module host, praenora-module-host: the executable in which a step's module instance runs when
# the step runs in a process of its own (docs/specs/process-isolation.md). A thin main.cpp over the
# runtime's objects and its server (src/runtime/isolation/module_host_server.cpp), so it holds the
# same code as the runtime: loader, documents and checks. Built on POSIX systems only: on Windows,
# process isolation is not available in this version, and this directory is not added.
#
# The runtime finds it relative to its own binary, at
# <directory of the runtime library>/praenora/../../libexec/praenora/praenora-module-host, so a
# build tree and an installation, moved or not, each use their own. In the build tree it goes
# there beside the runtime library (build/<preset>/libexec/praenora/); it is installed to the same
# place relative to the installed runtime library: ${CMAKE_INSTALL_LIBDIR}/../libexec/praenora,
# which is libexec/praenora for the usual lib.
add_executable(praenora_module_host
    main.cpp
    "${PROJECT_SOURCE_DIR}/src/runtime/isolation/module_host_server.hpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/isolation/module_host_server.cpp")
get_target_property(_praenora_runtime_library_dir praenora_runtime LIBRARY_OUTPUT_DIRECTORY)
if(NOT _praenora_runtime_library_dir)
    get_target_property(_praenora_runtime_library_dir praenora_runtime BINARY_DIR)
endif()
cmake_path(SET _praenora_module_host_dir NORMALIZE
    "${_praenora_runtime_library_dir}/praenora/../../libexec/praenora")
set_target_properties(praenora_module_host PROPERTIES
    OUTPUT_NAME praenora-module-host
    RUNTIME_OUTPUT_DIRECTORY "${_praenora_module_host_dir}")
# A multi-config generator would add a directory per configuration: the executable must sit at the
# one place the runtime looks.
foreach(_praenora_config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${_praenora_config}" _praenora_config)
    set_target_properties(praenora_module_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_praenora_config} "${_praenora_module_host_dir}")
endforeach()
# The runtime's objects, linked directly as white-box tests link them.
target_link_libraries(praenora_module_host PRIVATE praenora_runtime_objects praenora_runtime_deps)
target_compile_definitions(praenora_module_host PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_module_host)

if(PRAENORA_INSTALL_RULES)
    cmake_path(SET _praenora_module_host_destination NORMALIZE
        "${CMAKE_INSTALL_LIBDIR}/praenora/../../libexec/praenora")
    install(TARGETS praenora_module_host
        RUNTIME DESTINATION "${_praenora_module_host_destination}")
endif()
