# Runs praenora_simulation_print twice, each time in a new process, and checks that both print the
# same bytes: the driver of the providers.<name>.processes tests (tests/providers/CMakeLists.txt).
# Script mode, cmake -P, with these variables:
#
#   PRINT=<file>      the praenora_simulation_print executable
#   MANIFEST=<file>   the provider manifest
#   FORMAT=<format>   the model format
#   MODEL=<file>      the model file
#   END=<time>        the end time of its runs
#
# The test fails when either run exits with a nonzero code, prints a sanitizer report
# (tests/support/run_command.cmake) or prints no simulation, or when the two outputs differ.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS PRINT MANIFEST FORMAT MODEL END)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "run_twice.cmake: ${variable} is required")
    endif()
endforeach()

set(command "${PRINT}" "${MANIFEST}" "${FORMAT}" "${MODEL}" "${END}")
foreach(run IN ITEMS FIRST SECOND)
    praenora_test_run(${run} "praenora_simulation_print" "${command}")
    if(NOT ${run}_EXIT STREQUAL "0")
        praenora_test_fail("praenora_simulation_print" "exit code ${${run}_EXIT}, expected 0"
            "${command}" "${${run}_EXIT}" "${${run}_OUT}" "${${run}_ERR}")
    endif()
    if(NOT ${run}_OUT MATCHES "\nsimulate rows 0x0p\\+0 " OR
       NOT ${run}_OUT MATCHES "\nrun_batch outcomes 0 0 0\n")
        praenora_test_fail("praenora_simulation_print" "no simulation and batch in the output"
            "${command}" "${${run}_EXIT}" "${${run}_OUT}" "${${run}_ERR}")
    endif()
endforeach()
if(NOT FIRST_OUT STREQUAL SECOND_OUT)
    praenora_test_fail("praenora_simulation_print"
        "two processes printed different results:\n--- first ---\n${FIRST_OUT}"
        "${command}" "${SECOND_EXIT}" "${SECOND_OUT}" "${SECOND_ERR}")
endif()
string(LENGTH "${FIRST_OUT}" length)
message(STATUS "Two processes printed the same ${length} bytes.")
