# Tests of provider libraries as built files (label contract), each run by a CMake driver:
#
# - providers.<name>.exports (check_exports.cmake): nm lists the symbols the library exports, and
#   the provider entry point is the only one. Not on Windows, where a DLL exports only what is
#   marked __declspec(dllexport).
# - providers.<name>.processes (run_twice.cmake): praenora_simulation_print (simulation_print.cpp)
#   loads a provider of praenora.simulation@0, simulates a model and runs a plan's batch and a
#   kernel, printing every double exactly; two runs, each in a new process, must print the same
#   bytes.
#
# They run for the fixture provider test-simulation and, with PRAENORA_WITH_ROADRUNNER, for the
# roadrunner provider (with tests/fixtures/models/sbml/decay.xml). One more test checks the driver
# of the second: providers.run_twice.shows_first_difference (check_run_twice.cmake), how it shows
# two outputs that differ.

# The helper loads providers through the runtime's provider loader, an internal API, so it links
# the runtime's object library, as the white-box tests do.
add_executable(praenora_simulation_print simulation_print.cpp)
target_link_libraries(praenora_simulation_print PRIVATE
    praenora_runtime_objects
    praenora_runtime_deps)
target_compile_definitions(praenora_simulation_print PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_simulation_print)
praenora_copy_runtime_dlls(praenora_simulation_print)
add_dependencies(praenora_simulation_print praenora_test_providers)

set(_praenora_providers_dir "${CMAKE_CURRENT_SOURCE_DIR}")
if(APPLE)
    set(_praenora_binary_format macho)
else()
    set(_praenora_binary_format elf)
endif()

# praenora_add_provider_file_tests(<name> <target> <model format> <model file> <end time>)
function(praenora_add_provider_file_tests name target format model end)
    if(NOT WIN32 AND CMAKE_NM)
        add_test(NAME providers.${name}.exports
            COMMAND "${CMAKE_COMMAND}"
                "-DNM=${CMAKE_NM}"
                "-DLIBRARY=$<TARGET_FILE:${target}>"
                "-DBINARY=${_praenora_binary_format}"
                -P "${_praenora_providers_dir}/check_exports.cmake")
        set_tests_properties(providers.${name}.exports PROPERTIES LABELS contract)
    endif()
    add_test(NAME providers.${name}.processes
        COMMAND "${CMAKE_COMMAND}"
            "-DPRINT=$<TARGET_FILE:praenora_simulation_print>"
            "-DMANIFEST=$<TARGET_PROPERTY:${target},PRAENORA_PROVIDER_MANIFEST>"
            "-DFORMAT=${format}"
            "-DMODEL=${model}"
            "-DEND=${end}"
            -P "${_praenora_providers_dir}/run_twice.cmake")
    # _praenora_test_environment (tests/CMakeLists.txt): what every test of a macOS
    # AddressSanitizer build needs.
    set_tests_properties(providers.${name}.processes PROPERTIES
        LABELS contract
        TIMEOUT 600
        ${_praenora_test_environment})
endfunction()

praenora_add_provider_file_tests(test-simulation test_simulation test-decay
    "${PROJECT_SOURCE_DIR}/tests/fixtures/models/test-decay/two-states.json" 2)
if(PRAENORA_WITH_ROADRUNNER)
    add_dependencies(praenora_simulation_print praenora_provider_roadrunner)
    praenora_add_provider_file_tests(roadrunner praenora_provider_roadrunner sbml
        "${PROJECT_SOURCE_DIR}/tests/fixtures/models/sbml/decay.xml" 2)
endif()

# The check of run_twice.cmake, the driver of providers.<name>.processes (check_run_twice.cmake):
# how it shows two outputs that differ. It runs only CMake. Its timeout stops a driver whose search
# for the line that differs does not end.
add_test(NAME providers.run_twice.shows_first_difference
    COMMAND "${CMAKE_COMMAND}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/run_twice-check"
        -P "${_praenora_providers_dir}/check_run_twice.cmake")
set_tests_properties(providers.run_twice.shows_first_difference PROPERTIES
    LABELS contract
    TIMEOUT 120)
