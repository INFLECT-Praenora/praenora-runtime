# Runs the performance probe and checks its report: the driver of the perf tests
# (tests/perf/CMakeLists.txt). Script mode, cmake -P, with these variables:
#
#   PROBE=<file>          the praenora_perf executable
#   TICKS=<n>             its --ticks
#   VALIDATION=<mode>     its --validation: strict or off
#   ISOLATION=<mode>      its --isolation: in-process (the default) or process
#
# The test fails only when the probe exits with a nonzero code, prints a sanitizer report
# (tests/support/run_command.cmake), or does not print the JSON object that perf_probe.cpp
# documents (every field, with the ticks, the validation and the isolation asked for). Timings
# never fail it. The report is printed.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS PROBE TICKS VALIDATION)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "run_perf.cmake: ${variable} is required")
    endif()
endforeach()

if(NOT DEFINED ISOLATION)
    set(ISOLATION in-process)
endif()
set(command "${PROBE}" --ticks "${TICKS}" --validation "${VALIDATION}" --isolation "${ISOLATION}")
praenora_test_run(RUN "praenora_perf" "${command}")
string(STRIP "${RUN_OUT}" report)

# Fails the test, showing what the probe did.
function(_fail problem)
    praenora_test_fail("praenora_perf" "${problem}" "${command}" "${RUN_EXIT}" "${RUN_OUT}"
        "${RUN_ERR}")
endfunction()

if(NOT RUN_EXIT STREQUAL "0")
    _fail("exit code ${RUN_EXIT}, expected 0")
endif()
string(JSON type ERROR_VARIABLE json_error TYPE "${report}")
if(json_error OR NOT type STREQUAL "OBJECT")
    _fail("standard output is not one JSON object (${json_error})")
endif()
foreach(field IN ITEMS ticks steps invocations total_ns mean_invocation_overhead_ns mean_tick_ns
                       validation isolation)
    string(JSON value ERROR_VARIABLE json_error GET "${report}" ${field})
    if(json_error)
        _fail("the report has no ${field}")
    endif()
    set(report_${field} "${value}")
endforeach()
if(NOT report_ticks STREQUAL TICKS OR NOT report_validation STREQUAL VALIDATION
        OR NOT report_isolation STREQUAL ISOLATION)
    string(CONCAT problem "the report is for ${report_ticks} ticks with validation "
        "${report_validation}, isolation ${report_isolation}, not ${TICKS} ticks with "
        "validation ${VALIDATION}, isolation ${ISOLATION}")
    _fail("${problem}")
endif()
message(STATUS "praenora_perf --ticks ${TICKS} --validation ${VALIDATION} --isolation "
    "${ISOLATION}:\n${report}")
