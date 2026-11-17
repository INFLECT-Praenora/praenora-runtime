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
# (tests/support/run_command.cmake) or prints no simulation, or when the two outputs differ. Then
# its report names the first line where the two standard outputs differ, by its number, and shows
# that line as each run printed it. When one output is a prefix of the other, it says which one is
# shorter; otherwise it also names the byte of that line where they first differ. The first run's
# standard output and standard error follow, and then the second run's command, exit code and
# outputs. The outputs are compared with string(FIND), string(SUBSTRING) and string(LENGTH), never
# turned into lists: CMake's list commands split at ; and treat [ and ] specially, and the
# simulation output holds JSON.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

# Moves the first line of the text in the variable named `rest`, its line feed included, into the
# variable named `line`. The text after the last line feed is a line too; an empty text has no
# line, and `line` is set empty.
function(_take_line rest line)
    string(FIND "${${rest}}" "\n" end)
    if(end EQUAL -1)
        set(${line} "${${rest}}" PARENT_SCOPE)
        set(${rest} "" PARENT_SCOPE)
    else()
        math(EXPR end "${end} + 1")
        string(SUBSTRING "${${rest}}" 0 ${end} taken)
        string(SUBSTRING "${${rest}}" ${end} -1 left)
        set(${line} "${taken}" PARENT_SCOPE)
        set(${rest} "${left}" PARENT_SCOPE)
    endif()
endfunction()

# Sets the variable named `byte` to the first byte, counted from 1, where the texts `a` and `b`
# differ; they must differ. Their longest common start is found by halving: the start of a length
# is common when that of a longer one is.
function(_first_different_byte a b byte)
    string(LENGTH "${a}" a_length)
    string(LENGTH "${b}" b_length)
    # The length of the common start lies between low and high.
    set(low 0)
    set(high ${a_length})
    if(b_length LESS a_length)
        set(high ${b_length})
    endif()
    while(low LESS high)
        math(EXPR middle "(${low} + ${high} + 1) / 2")
        string(SUBSTRING "${a}" 0 ${middle} a_start)
        string(SUBSTRING "${b}" 0 ${middle} b_start)
        if(a_start STREQUAL b_start)
            set(low ${middle})
        else()
            math(EXPR high "${middle} - 1")
        endif()
    endwhile()
    math(EXPR low "${low} + 1")
    set(${byte} ${low} PARENT_SCOPE)
endfunction()

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
    # The first line that differs; the outputs differ, so the loop ends.
    set(first_rest "${FIRST_OUT}")
    set(second_rest "${SECOND_OUT}")
    set(first_line "")
    set(second_line "")
    set(number 0)
    while(first_line STREQUAL second_line)
        math(EXPR number "${number} + 1")
        _take_line(first_rest first_line)
        _take_line(second_rest second_line)
    endwhile()
    string(CONCAT problem "two processes printed different results: their standard outputs first "
        "differ on line ${number}")
    # Whether one output is a prefix of the other, and which.
    string(LENGTH "${FIRST_OUT}" first_length)
    string(LENGTH "${SECOND_OUT}" second_length)
    set(shorter "")
    if(first_length LESS second_length)
        string(SUBSTRING "${SECOND_OUT}" 0 ${first_length} start)
        if(start STREQUAL FIRST_OUT)
            set(shorter first)
            set(longer second)
        endif()
    elseif(second_length LESS first_length)
        string(SUBSTRING "${FIRST_OUT}" 0 ${second_length} start)
        if(start STREQUAL SECOND_OUT)
            set(shorter second)
            set(longer first)
        endif()
    endif()
    if(NOT shorter STREQUAL "")
        if(${shorter}_line STREQUAL "")
            set(end "ends before line ${number}")
        else()
            set(end "ends in line ${number}, without a line feed")
        endif()
        string(APPEND problem
            ". The ${shorter} run's is shorter, a prefix of the ${longer} run's: it ${end}.")
    else()
        _first_different_byte("${first_line}" "${second_line}" byte)
        string(APPEND problem ", at byte ${byte} of that line.")
    endif()
    # The line as each run printed it, without its line feed; a run whose output ends before it
    # printed none.
    foreach(run IN ITEMS first second)
        if(NOT ${run}_line STREQUAL "")
            string(REGEX REPLACE "\n$" "" shown "${${run}_line}")
            string(APPEND problem "\n--- line ${number} of the ${run} run's stdout ---\n${shown}")
        endif()
    endforeach()
    string(APPEND problem "\n--- stdout of the first run ---\n${FIRST_OUT}\n"
        "--- stderr of the first run ---\n${FIRST_ERR}\n"
        "Below are the second run's command, exit code and outputs.")
    praenora_test_fail("praenora_simulation_print" "${problem}"
        "${command}" "${SECOND_EXIT}" "${SECOND_OUT}" "${SECOND_ERR}")
endif()
string(LENGTH "${FIRST_OUT}" length)
message(STATUS "Two processes printed the same ${length} bytes.")
