# Checks how run_twice.cmake, the driver of the providers.<name>.processes tests, shows two outputs
# that differ: the driver of providers.run_twice.shows_first_difference
# (tests/providers/CMakeLists.txt). Script mode, cmake -P, with WORK set to a directory of the build
# tree, which it deletes and fills; it needs only CMake.
#
# Each case writes two outputs to first.txt and second.txt in a directory of its own and runs
# run_twice.cmake with this script as the printer: PRINT is cmake, and MANIFEST, FORMAT, MODEL and
# END are -DFIRST=<first.txt>, -DSECOND=<second.txt>, -P and this script. With FIRST and SECOND set
# this script prints the first file, the first time, and the second file, every later time, to
# standard output as it is, and a line that names the run to standard error, and checks nothing.
# Each output passes run_twice.cmake's other checks. Its first line holds ; inside and outside
# brackets and a [ that no ] closes, and its third line a [ that no ] closes and a ; before the
# byte where case 1 differs: CMake's list commands treat ; [ and ] specially. run_twice.cmake must
# fail, and its report is checked line by line, all but the command's arguments: each line must
# be shown whole, with no other line between them or after them.
# 1. The outputs differ on line 3 and again on line 5: the report names line 3 and the byte where
#    it first differs, and shows it as each run printed it.
# 2. The second output is the first's without its last line: the second is shorter, a prefix of
#    the first, and ends before the line that differs.
# 3. The first output is the second's without its final line feed: the first is shorter, a prefix
#    of the second, and ends in the line that differs.
# 4. The second output is the first's and one more line, an empty one: the first is shorter, a
#    prefix of the second, and ends before that line, which the report shows empty.
cmake_minimum_required(VERSION 3.24)

if(DEFINED FIRST AND DEFINED SECOND)
    set(marker "${FIRST}.printed")
    if(EXISTS "${marker}")
        set(run second)
        set(output "${SECOND}")
    else()
        file(WRITE "${marker}" "")
        set(run first)
        set(output "${FIRST}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${output}")
    message(NOTICE "printed by the ${run} run")
    return()
endif()

if(NOT DEFINED WORK)
    message(FATAL_ERROR "check_run_twice.cmake: WORK is required")
endif()
set(driver "${CMAKE_CURRENT_LIST_DIR}/run_twice.cmake")
file(REMOVE_RECURSE "${WORK}")

# Fails the test with `problem`, then what run_twice.cmake printed (`out` and `err`), each line with
# a space added at its start, so that message() shows it as it is.
function(_fail problem)
    string(REPLACE "\n" "\n " out " ${out}")
    string(REPLACE "\n" "\n " err " ${err}")
    message(FATAL_ERROR "${problem}\n--- stdout of run_twice.cmake ---\n${out}\n"
        "--- stderr of run_twice.cmake ---\n${err}")
endfunction()

# Runs run_twice.cmake on the outputs `first` and `second` and checks its report: `problem`, which
# the line that shows the command follows, and the second run's exit code and outputs after it.
# Each line of the report is shown after three spaces: CMake's indent of two and the space that
# tests/support/run_command.cmake adds.
function(_check case first second problem)
    set(directory "${WORK}/${case}")
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${directory}/first.txt" "${first}")
    file(WRITE "${directory}/second.txt" "${second}")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DPRINT=${CMAKE_COMMAND}"
            "-DMANIFEST=-DFIRST=${directory}/first.txt"
            "-DFORMAT=-DSECOND=${directory}/second.txt" "-DMODEL=-P"
            "-DEND=${CMAKE_CURRENT_LIST_FILE}" -P "${driver}"
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(exit_code STREQUAL "0")
        _fail("${case}: run_twice.cmake did not fail on two outputs that differ.")
    endif()
    string(CONCAT shown_problem "\npraenora_simulation_print: ${problem}\n"
        "Below are the second run's command, exit code and outputs.\ncommand: ")
    string(CONCAT shown_outputs "\nexit code: 0\n--- stdout ---\n${second}\n"
        "--- stderr ---\nprinted by the second run\n")
    string(REPLACE "\n" "\n   " shown_problem "${shown_problem}")
    string(REPLACE "\n" "\n   " shown_outputs "${shown_outputs}")
    # The standard error ends with a line feed: a line that holds only the spaces ends the report.
    string(APPEND shown_outputs "\n")
    # Both are found, the problem first, with only the command's arguments between them, which
    # hold no line feed; the character after the outputs is not a space, so no line of the report
    # follows.
    string(FIND "${err}" "${shown_problem}" problem_position)
    string(FIND "${err}" "${shown_outputs}" outputs_position)
    string(LENGTH "${shown_problem}" problem_length)
    string(LENGTH "${shown_outputs}" outputs_length)
    math(EXPR arguments_position "${problem_position} + ${problem_length}")
    math(EXPR arguments_length "${outputs_position} - ${arguments_position}")
    set(arguments "\n")
    set(next "")
    if(NOT problem_position EQUAL -1 AND NOT outputs_position EQUAL -1 AND
       arguments_length GREATER_EQUAL 0)
        string(SUBSTRING "${err}" ${arguments_position} ${arguments_length} arguments)
        math(EXPR next_position "${outputs_position} + ${outputs_length}")
        string(SUBSTRING "${err}" ${next_position} 1 next)
    endif()
    if(arguments MATCHES "\n" OR next STREQUAL " ")
        string(CONCAT problem "${case}: run_twice.cmake did not show, as they were printed, the "
            "problem\n${shown_problem}\nthen, on the same line, the command's arguments and then "
            "the rest of the report, with no line after it\n${shown_outputs}")
        _fail("${problem}")
    endif()
    message(STATUS "${case}: run_twice.cmake shows the report as${shown_problem}..."
        "${shown_outputs}")
endfunction()

# The outputs. Each line of the first ends with a line feed.
set(line_1 [=[describe_model {"states":["x;y"]} a;b [c]=])
set(line_2 [=[simulate {"rows":[[0.0,10.0]]}]=])
set(line_3 "simulate rows 0x0p+0 0x1.4p+3 [x;y 0x1p+2")
set(line_4 "run_batch outcomes 0 0 0")
set(line_5 "run_batch out 0x0p+0 0x1.4p+2")
set(head "${line_1}\n${line_2}\n")
set(first "${head}${line_3}\n${line_4}\n${line_5}\n")
# The first output's stdout and stderr, as the report shows them.
string(CONCAT first_outputs "--- stdout of the first run ---\n${first}\n"
    "--- stderr of the first run ---\nprinted by the first run\n")

# 1. Lines 3 and 5 differ. Line 3 first differs at its byte 39, after the 38 bytes
# "simulate rows 0x0p+0 0x1.4p+3 [x;y 0x1".
set(other_line_3 "simulate rows 0x0p+0 0x1.4p+3 [x;y 0x1.0000000000001p+2")
set(other_line_5 "run_batch out 0x0p+0 0x1.4p+1")
string(CONCAT problem "two processes printed different results: their standard outputs first "
    "differ on line 3, at byte 39 of that line.\n"
    "--- line 3 of the first run's stdout ---\n${line_3}\n"
    "--- line 3 of the second run's stdout ---\n${other_line_3}\n"
    "${first_outputs}")
_check(1 "${first}" "${head}${other_line_3}\n${line_4}\n${other_line_5}\n" "${problem}")

# 2. The second output ends before line 5.
string(CONCAT problem "two processes printed different results: their standard outputs first "
    "differ on line 5. The second run's is shorter, a prefix of the first run's: it ends before "
    "line 5.\n"
    "--- line 5 of the first run's stdout ---\n${line_5}\n"
    "${first_outputs}")
_check(2 "${first}" "${head}${line_3}\n${line_4}\n" "${problem}")

# 3. The first output ends in line 5, without a line feed.
set(shorter "${head}${line_3}\n${line_4}\n${line_5}")
string(CONCAT problem "two processes printed different results: their standard outputs first "
    "differ on line 5. The first run's is shorter, a prefix of the second run's: it ends in line "
    "5, without a line feed.\n"
    "--- line 5 of the first run's stdout ---\n${line_5}\n"
    "--- line 5 of the second run's stdout ---\n${line_5}\n"
    "--- stdout of the first run ---\n${shorter}\n"
    "--- stderr of the first run ---\nprinted by the first run\n")
_check(3 "${shorter}" "${first}" "${problem}")

# 4. The second output has one more line, an empty one.
string(CONCAT problem "two processes printed different results: their standard outputs first "
    "differ on line 6. The first run's is shorter, a prefix of the second run's: it ends before "
    "line 6.\n"
    "--- line 6 of the second run's stdout ---\n\n"
    "${first_outputs}")
_check(4 "${first}" "${first}\n" "${problem}")
