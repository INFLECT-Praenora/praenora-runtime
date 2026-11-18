# Tests of provider libraries as built files (label contract), each run by a CMake driver:
#
# - providers.<name>.exports (check_exports.cmake): nm lists the symbols the library exports, and
#   the provider entry point is the only one. Not on Windows, where a DLL exports only what is
#   marked __declspec(dllexport).
# - providers.<name>.processes (run_twice.cmake): a print tool loads the provider and prints what a
#   fixed sequence of calls computes; two runs, each in a new process, must print the same bytes.
#   praenora_simulation_print (simulation_print.cpp) loads a provider of praenora.simulation@0,
#   simulates a model and runs a plan's batch and a kernel, printing every double exactly;
#   praenora_conversion_print (conversion_print.cpp) loads a provider of
#   praenora.model-conversion@0, converts a source and converts the result back, printing
#   describe's document and both texts and reports verbatim.
#
# They run for the fixture provider test-simulation and, with PRAENORA_WITH_ROADRUNNER, for the
# roadrunner provider (with tests/fixtures/models/sbml/decay.xml), both with
# praenora_simulation_print, and for the fixture provider test-conversion (with
# tests/fixtures/models/test-decay-text/two-states.txt) and, with PRAENORA_WITH_ANTIMONY, for the
# antimony provider (with tests/fixtures/models/antimony/decay.ant, Antimony to SBML and back),
# both with praenora_conversion_print. One more
# test checks the driver of the second: providers.run_twice.shows_first_difference
# (check_run_twice.cmake), how it shows two outputs that differ and a run that fails.

# The tools load providers through the runtime's provider loader, an internal API, so they link
# the runtime's object library, as the white-box tests do.
foreach(_praenora_print IN ITEMS simulation conversion)
    set(_praenora_target praenora_${_praenora_print}_print)
    add_executable(${_praenora_target} ${_praenora_print}_print.cpp)
    target_link_libraries(${_praenora_target} PRIVATE
        praenora_runtime_objects
        praenora_runtime_deps)
    target_compile_definitions(${_praenora_target} PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
    praenora_target_warnings(${_praenora_target})
    praenora_copy_runtime_dlls(${_praenora_target})
    add_dependencies(${_praenora_target} praenora_test_providers)
endforeach()

set(_praenora_providers_dir "${CMAKE_CURRENT_SOURCE_DIR}")
if(APPLE)
    set(_praenora_binary_format macho)
else()
    set(_praenora_binary_format elf)
endif()

# praenora_add_provider_file_tests(<name> <target> <print target> <first> <second> <third>)
# providers.<name>.exports, and providers.<name>.processes, which runs <print target>
# (praenora_simulation_print or praenora_conversion_print) with the provider's manifest and its
# three arguments after it: the model format, the model file and the end time of
# praenora_simulation_print, or the format to convert from, the source file and the format to
# convert to of praenora_conversion_print.
function(praenora_add_provider_file_tests name target print first second third)
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
            "-DPRINT=$<TARGET_FILE:${print}>"
            "-DTOOL=${print}"
            "-DMANIFEST=$<TARGET_PROPERTY:${target},PRAENORA_PROVIDER_MANIFEST>"
            "-DFORMAT=${first}"
            "-DMODEL=${second}"
            "-DEND=${third}"
            -P "${_praenora_providers_dir}/run_twice.cmake")
    # _praenora_test_environment (tests/CMakeLists.txt): what every test of a macOS
    # AddressSanitizer build needs.
    set_tests_properties(providers.${name}.processes PROPERTIES
        LABELS contract
        TIMEOUT 600
        ${_praenora_test_environment})
endfunction()

praenora_add_provider_file_tests(test-simulation test_simulation praenora_simulation_print
    test-decay "${PROJECT_SOURCE_DIR}/tests/fixtures/models/test-decay/two-states.json" 2)
praenora_add_provider_file_tests(test-conversion test_conversion praenora_conversion_print
    test-decay-text "${PROJECT_SOURCE_DIR}/tests/fixtures/models/test-decay-text/two-states.txt"
    test-decay)
if(PRAENORA_WITH_ROADRUNNER)
    add_dependencies(praenora_simulation_print praenora_provider_roadrunner)
    praenora_add_provider_file_tests(roadrunner praenora_provider_roadrunner
        praenora_simulation_print sbml "${PROJECT_SOURCE_DIR}/tests/fixtures/models/sbml/decay.xml"
        2)
endif()
if(PRAENORA_WITH_ANTIMONY)
    add_dependencies(praenora_conversion_print praenora_provider_antimony)
    praenora_add_provider_file_tests(antimony praenora_provider_antimony praenora_conversion_print
        antimony "${PROJECT_SOURCE_DIR}/tests/fixtures/models/antimony/decay.ant" sbml)
endif()

# The check of run_twice.cmake, the driver of providers.<name>.processes (check_run_twice.cmake):
# how it shows two outputs that differ, and a run that fails in each of the three ways, the first
# run and then the second: it exits with a nonzero code, prints no result or prints a sanitizer
# report. It runs only CMake. Its timeout stops a driver whose search for the line that differs
# does not end.
add_test(NAME providers.run_twice.shows_first_difference
    COMMAND "${CMAKE_COMMAND}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/run_twice-check"
        -P "${_praenora_providers_dir}/check_run_twice.cmake")
set_tests_properties(providers.run_twice.shows_first_difference PROPERTIES
    LABELS contract
    TIMEOUT 120)
