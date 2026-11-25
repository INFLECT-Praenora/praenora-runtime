# Tests of provider libraries as built files (label contract), each run by a CMake driver:
#
# - providers.<name>.exports (check_exports.cmake): nm lists the symbols the library exports, and
#   the provider entry point is the only one. Not on Windows, where a DLL exports only what is
#   marked __declspec(dllexport). With PRAENORA_WITH_PYTHON_ENVIRONMENT also
#   providers.python-core.exports, for the Python core, whose one export is its entry point
#   praenora_python_core_get_api.
# - providers.<name>.processes (run_twice.cmake): a print tool loads the provider and prints what a
#   fixed sequence of calls computes; two runs, each in a new process, must print the same bytes.
#   Each run's standard output is written to a file, processes/<name>/first.out or second.out in
#   this directory of the build tree, and the two files are compared byte for byte: CMake's
#   capture of an output drops the carriage return of each carriage return and line feed and every
#   NUL byte. praenora_simulation_print (simulation_print.cpp) loads a provider of
#   praenora.simulation@0, simulates a model and runs a plan's batch and a kernel, printing every
#   double exactly; praenora_conversion_print (conversion_print.cpp) loads a provider of
#   praenora.model-conversion@0, converts a source and converts the result back, printing
#   describe's document and both texts and reports verbatim; praenora_python_print
#   (python_print.cpp) loads a provider of praenora.python@0, prints the provider's and the
#   service's describe documents, creates an object of a Python class and prints what one of its
#   methods returns; in its provider mode it loads a provider of test.echo@0 whose table is
#   implemented in Python, and prints the provider's describe document, the table's header and
#   what its echo returns.
#
# They run for the fixture provider test-simulation and, with PRAENORA_WITH_ROADRUNNER, for the
# roadrunner provider (with tests/fixtures/models/sbml/decay.xml), both with
# praenora_simulation_print, and for the fixture provider test-conversion (with
# tests/fixtures/models/test-decay-text/two-states.txt) and, with PRAENORA_WITH_ANTIMONY, for the
# antimony provider (with tests/fixtures/models/antimony/decay.ant, Antimony to SBML and back),
# both with praenora_conversion_print, and, with PRAENORA_WITH_PYTHON_ENVIRONMENT, for the
# provider python, with praenora_python_print and the fixture class core_fixtures.facts:Facts of
# tests/fixtures/python-core (what the interpreter says of itself, its hash of a string among it),
# and for the fixture provider test-python-echo, with praenora_python_print's provider mode (the
# table of test.echo@0, and what its echo returns, through Python, for a text, no bytes and bytes
# that are not text).
# One more test checks the driver of the second: providers.run_twice.shows_first_difference
# (check_run_twice.cmake), how it shows two outputs that differ and a run that fails.

# The tools load providers through the runtime's provider loader, an internal API, so they link
# the runtime's object library, as the white-box tests do.
set(_praenora_prints simulation conversion)
if(PRAENORA_WITH_PYTHON_ENVIRONMENT)
    list(APPEND _praenora_prints python)
endif()
foreach(_praenora_print IN LISTS _praenora_prints)
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
# (praenora_simulation_print, praenora_conversion_print or praenora_python_print) with the
# provider's manifest and its three arguments after it: the model format, the model file and the
# end time of praenora_simulation_print, the format to convert from, the source file and the
# format to convert to of praenora_conversion_print, or the directory of the Python sources, the
# class and the method of praenora_python_print (in its provider mode, --provider, the service and
# the text to echo).
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
            "-DMANIFEST=$<GENEX_EVAL:$<TARGET_PROPERTY:${target},PRAENORA_PROVIDER_MANIFEST>>"
            "-DFORMAT=${first}"
            "-DMODEL=${second}"
            "-DEND=${third}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/processes/${name}"
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
if(PRAENORA_WITH_PYTHON_ENVIRONMENT)
    # test_echo.h, for its provider mode, and text_sink.hpp. It runs the Python environment in
    # its process: on Windows it gets the manifest that allows long paths (tests/CMakeLists.txt).
    target_include_directories(praenora_python_print PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
    add_dependencies(praenora_python_print praenora_provider_python)
    praenora_add_long_path_manifest(praenora_python_print)
    praenora_add_provider_file_tests(python praenora_provider_python praenora_python_print
        "${PROJECT_SOURCE_DIR}/tests/fixtures/python-core" core_fixtures.facts:Facts interpreter)
    praenora_add_provider_file_tests(test-python-echo test_python_echo praenora_python_print
        --provider test.echo@0 hello)
    # The Python core, which the provider python loads: its entry point is its only export.
    if(NOT WIN32 AND CMAKE_NM)
        add_test(NAME providers.python-core.exports
            COMMAND "${CMAKE_COMMAND}"
                "-DNM=${CMAKE_NM}"
                "-DLIBRARY=$<TARGET_FILE:praenora_python_core>"
                "-DBINARY=${_praenora_binary_format}"
                "-DENTRY=praenora_python_core_get_api"
                -P "${_praenora_providers_dir}/check_exports.cmake")
        set_tests_properties(providers.python-core.exports PROPERTIES LABELS contract)
    endif()
endif()

# The check of run_twice.cmake, the driver of providers.<name>.processes (check_run_twice.cmake):
# how it shows two outputs that differ, also two that differ only in a byte that CMake's capture
# drops (a carriage return before a line feed, or a NUL byte), and a run that fails in each of the
# three ways, the first run and then the second: it exits with a nonzero code, prints no result or
# prints a sanitizer report; and that two runs that printed the same bytes pass, with their count.
# It runs only CMake. Its timeout stops a driver whose search for the line that differs does not
# end.
add_test(NAME providers.run_twice.shows_first_difference
    COMMAND "${CMAKE_COMMAND}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/run_twice-check"
        -P "${_praenora_providers_dir}/check_run_twice.cmake")
set_tests_properties(providers.run_twice.shows_first_difference PROPERTIES
    LABELS contract
    TIMEOUT 120)
