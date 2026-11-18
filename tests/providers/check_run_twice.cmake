# Checks how run_twice.cmake, the driver of the providers.<name>.processes tests, shows two outputs
# that differ, and a run that fails: the driver of providers.run_twice.shows_first_difference
# (tests/providers/CMakeLists.txt). Script mode, cmake -P, with WORK set to a directory of the build
# tree, which it deletes and fills; it needs only CMake.
#
# Each case writes two outputs to first.txt and second.txt in a directory of its own, a NUL byte for
# each <NUL> in them (_write()), and runs run_twice.cmake with this script as the printer: PRINT is
# cmake, and MANIFEST, FORMAT, MODEL and END are -DFIRST=<first.txt>, -DSECOND=<second.txt>, -P and
# this script; WORK is the outputs directory inside the case's directory. With FIRST and SECOND set
# this script prints the first file, the first time, and the second file, every later time, to
# standard output as it is, and a line that names the run to standard error, and checks nothing.
# When the printed file has a companion <file>.stderr, it prints that file's text to standard error
# before that line; when it has a companion <file>.fails, it then exits with an error, which CMake
# reports on standard error after that line. In cases 1 to 13 run_twice.cmake must fail, and its
# report is checked line by line, all but the command's arguments: each line must be shown whole,
# with no other line between them. When no run fails, no line may follow them; otherwise CMake's
# report of the error follows, of which only the start is checked. A report that shows the second
# run's command, exit code and outputs shows the first run's standard output and standard error
# before them. It shows the outputs as CMake captures them, without the carriage return of a
# carriage return and line feed and without NUL bytes.
#
# Cases 1 to 4, 12 and 13 make the outputs differ, and each output passes run_twice.cmake's other
# checks; in cases 12 and 13 they differ only in a byte that CMake's capture drops. The first
# output's first line holds ; inside and outside brackets and a [ that no ] closes, and its third
# line a [ that no ] closes and a ; before the byte where case 1 differs: CMake's list commands
# treat ; [ and ] specially. Its first line also holds क (U+0915), whose UTF-8 bytes e0 a4 95 hold
# the hexadecimal digits 0a across two bytes, where run_twice.cmake must not count a line feed. In
# cases 2 to 4 one output is a prefix of the other, and the report names the longer one's next
# byte.
# 1. The outputs differ on line 3 and again on line 5: the report names line 3, the byte where it
#    first differs and that byte of each output, and shows the line as each run printed it.
# 2. The second output is the first's without its last line: the second is shorter, a prefix of
#    the first, and ends before the line that differs.
# 3. The first output is the second's without its final line feed: the first is shorter, a prefix
#    of the second, and ends in the line that differs.
# 4. The second output has one more line than the first, an empty one: the first is shorter, a
#    prefix of the second, and ends before that line, which the report shows empty.
# Cases 5 to 11 make one run fail, and the report must name that run. In cases 7, 9 and 11 the
# second run fails, and its output differs from the first run's, which the report must show.
# 5. Both outputs are the same lines of a conversion without its two report lines (no conversion
#    and no conversion back): the first run fails.
# 6. and 7. The first run, then the second, exits with an error: exit code 1.
# 8. and 9. The first run's output has no batch, then the second's no simulation: either is
#    reported as no simulation and batch.
# 10. and 11. The first run prints a sanitizer report on standard output, then the second on
#    standard error: run_command.cmake searches both.
# 12. The second output ends line 1 with a carriage return before the line feed, so the outputs
#    are the same text as CMake captures them: the report names line 1, the byte where it first
#    differs and that byte of each output, shows the line as the same in both and says why.
# 13. The second output holds a NUL byte on line 1, which CMake's capture drops too: the report is
#    as in case 12.
# 14. Both outputs are the same bytes, with a carriage return before each line feed and a NUL byte:
#    run_twice.cmake must pass, and count every byte.
# Case 5 runs run_twice.cmake as praenora_conversion_print's driver
# (TOOL=praenora_conversion_print); the others run it without TOOL, as praenora_simulation_print's
# driver.
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
    set(text "")
    if(EXISTS "${output}.stderr")
        file(READ "${output}.stderr" text)
    endif()
    message(NOTICE "${text}printed by the ${run} run")
    if(EXISTS "${output}.fails")
        message(FATAL_ERROR "the ${run} run fails")
    endif()
    return()
endif()

if(NOT DEFINED WORK)
    message(FATAL_ERROR "check_run_twice.cmake: WORK is required")
endif()
set(driver "${CMAKE_CURRENT_LIST_DIR}/run_twice.cmake")
file(REMOVE_RECURSE "${WORK}")

# One NUL byte, in the variable nul_byte, for _write(). No CMake string can be written with one,
# but file(READ) of a NUL byte at the end of a file puts it into a variable, which file(WRITE) and
# file(APPEND) write as they are: the last byte of a tar archive, which ends with blocks of NUL
# bytes. Only those commands, and the check below, may touch that variable.
file(WRITE "${WORK}/empty" "")
file(ARCHIVE_CREATE OUTPUT "${WORK}/empty.tar" PATHS "${WORK}/empty" FORMAT gnutar)
file(SIZE "${WORK}/empty.tar" size)
math(EXPR last "${size} - 1")
file(READ "${WORK}/empty.tar" nul_byte OFFSET ${last} LIMIT 1)
file(WRITE "${WORK}/nul" "${nul_byte}")
file(READ "${WORK}/nul" written HEX)
if(NOT written STREQUAL "00")
    message(FATAL_ERROR "check_run_twice.cmake: ${WORK}/nul holds the bytes ${written} "
        "(hexadecimal), not one NUL byte")
endif()

# Writes `text` to the file `file`, with a NUL byte for each <NUL> in it: the parts of the text
# around them, with nul_byte appended between them.
function(_write file text)
    string(FIND "${text}" "<NUL>" position)
    file(WRITE "${file}" "")
    while(NOT position EQUAL -1)
        string(SUBSTRING "${text}" 0 ${position} part)
        math(EXPR position "${position} + 5")
        string(SUBSTRING "${text}" ${position} -1 text)
        file(APPEND "${file}" "${part}")
        file(APPEND "${file}" "${nul_byte}")
        string(FIND "${text}" "<NUL>" position)
    endwhile()
    file(APPEND "${file}" "${text}")
endfunction()

# Runs run_twice.cmake on the outputs first.txt and second.txt in `directory` as the driver of
# `tool` (praenora_simulation_print: without TOOL; praenora_conversion_print: with TOOL), and sets
# exit_code, out and err to its exit code and outputs.
function(_run_twice directory tool)
    set(tool_argument "")
    if(NOT tool STREQUAL "praenora_simulation_print")
        set(tool_argument "-DTOOL=${tool}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DPRINT=${CMAKE_COMMAND}" ${tool_argument}
            "-DMANIFEST=-DFIRST=${directory}/first.txt"
            "-DFORMAT=-DSECOND=${directory}/second.txt" "-DMODEL=-P"
            "-DEND=${CMAKE_CURRENT_LIST_FILE}" "-DWORK=${directory}/outputs" -P "${driver}"
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(exit_code "${exit_code}" PARENT_SCOPE)
    set(out "${out}" PARENT_SCOPE)
    set(err "${err}" PARENT_SCOPE)
endfunction()

# Fails the test with `problem`, then what run_twice.cmake printed (`out` and `err`), each line with
# a space added at its start, so that message() shows it as it is.
function(_fail problem)
    string(REPLACE "\n" "\n " out " ${out}")
    string(REPLACE "\n" "\n " err " ${err}")
    message(FATAL_ERROR "${problem}\n--- stdout of run_twice.cmake ---\n${out}\n"
        "--- stderr of run_twice.cmake ---\n${err}")
endfunction()

# Runs run_twice.cmake on the outputs `first` and `second`, written with _write(), as the driver of
# `tool` (_run_twice()) and checks its report. `fails` is the run that exits with an error, first
# or second, or empty when none does. The report must show `head`, its first line or lines, which
# tell what failed; when `shown` is second, then the first run's standard output and standard
# error, and a line that says that the second run's command, exit code and outputs follow; then the
# line that shows the command; then the exit code and outputs of the `shown` run. When two more
# arguments follow, the first names a run, first or second, and that run prints the other argument,
# a text that ends with a line feed, to standard error before its line. Each line of the report is
# shown after three spaces: CMake's indent of two and the space that
# tests/support/run_command.cmake adds.
function(_check case tool fails first second shown head)
    set(directory "${WORK}/${case}")
    file(MAKE_DIRECTORY "${directory}")
    _write("${directory}/first.txt" "${first}")
    _write("${directory}/second.txt" "${second}")
    # The standard error of each run.
    set(first_err "printed by the first run\n")
    set(second_err "printed by the second run\n")
    if(ARGC EQUAL 9)
        file(WRITE "${directory}/${ARGV7}.txt.stderr" "${ARGV8}")
        set(${ARGV7}_err "${ARGV8}${${ARGV7}_err}")
    endif()
    if(NOT fails STREQUAL "")
        file(WRITE "${directory}/${fails}.txt.fails" "")
    endif()
    _run_twice("${directory}" ${tool})
    if(exit_code STREQUAL "0")
        _fail("${case}: run_twice.cmake did not fail.")
    endif()
    # The outputs as CMake captures them, as the report shows them: without the carriage return of
    # a carriage return and line feed, and without NUL bytes.
    foreach(output IN ITEMS first second)
        string(REPLACE "\r\n" "\n" ${output} "${${output}}")
        string(REPLACE "<NUL>" "" ${output} "${${output}}")
    endforeach()
    set(shown_head "\n${head}\n")
    if(shown STREQUAL "second")
        string(APPEND shown_head "--- stdout of the first run ---\n${first}\n"
            "--- stderr of the first run ---\n${first_err}\n"
            "Below are the second run's command, exit code and outputs.\n")
    endif()
    string(APPEND shown_head "command: ")
    set(shown_exit_code 0)
    if(fails STREQUAL shown)
        set(shown_exit_code 1)
    endif()
    string(CONCAT shown_tail "\nexit code: ${shown_exit_code}\n--- stdout ---\n${${shown}}\n"
        "--- stderr ---\n${${shown}_err}")
    string(REPLACE "\n" "\n   " shown_head "${shown_head}")
    string(REPLACE "\n" "\n   " shown_tail "${shown_tail}")
    if(fails STREQUAL "")
        # The standard error ends with a line feed: a line that holds only the spaces ends the
        # report.
        string(APPEND shown_tail "\n")
    else()
        # CMake's report of the printer's error follows.
        string(APPEND shown_tail "CMake Error at ")
    endif()
    # Both are found, the head first, with only the command's arguments between them, which hold no
    # line feed. When no run fails, the character after the tail is not a space, so no line of the
    # report follows.
    string(FIND "${err}" "${shown_head}" head_position)
    string(FIND "${err}" "${shown_tail}" tail_position)
    string(LENGTH "${shown_head}" head_length)
    string(LENGTH "${shown_tail}" tail_length)
    math(EXPR arguments_position "${head_position} + ${head_length}")
    math(EXPR arguments_length "${tail_position} - ${arguments_position}")
    set(arguments "\n")
    set(next "")
    if(NOT head_position EQUAL -1 AND NOT tail_position EQUAL -1 AND
       arguments_length GREATER_EQUAL 0)
        string(SUBSTRING "${err}" ${arguments_position} ${arguments_length} arguments)
        if(fails STREQUAL "")
            math(EXPR next_position "${tail_position} + ${tail_length}")
            string(SUBSTRING "${err}" ${next_position} 1 next)
        endif()
    endif()
    if(arguments MATCHES "\n" OR next STREQUAL " ")
        if(fails STREQUAL "")
            set(after "with no line after it")
        else()
            set(after "and then CMake's report of the printer's error")
        endif()
        string(CONCAT problem "${case}: run_twice.cmake did not show, as they were printed, the "
            "start of the report\n${shown_head}\nthen, on the same line, the command's arguments "
            "and then the rest of the report, ${after}\n${shown_tail}")
        _fail("${problem}")
    endif()
    message(STATUS "${case}: run_twice.cmake shows the report as${shown_head}...${shown_tail}")
endfunction()

# The outputs. Each line of the first ends with a line feed.
set(line_1 [=[describe_model {"states":["x;y","क"]} a;b [c]=])
set(line_2 [=[simulate {"rows":[[0.0,10.0]]}]=])
set(line_3 "simulate rows 0x0p+0 0x1.4p+3 [x;y 0x1p+2")
set(line_4 "run_batch outcomes 0 0 0")
set(line_5 "run_batch out 0x0p+0 0x1.4p+2")
set(head "${line_1}\n${line_2}\n")
set(first "${head}${line_3}\n${line_4}\n${line_5}\n")
set(tool praenora_simulation_print)
set(differ "${tool}: two processes printed different results: their standard outputs first differ")

# 1. Lines 3 and 5 differ. Line 3 first differs at its byte 39, after the 38 bytes
# "simulate rows 0x0p+0 0x1.4p+3 [x;y 0x1": p (0x70) in the first, . (0x2e) in the second.
set(other_line_3 "simulate rows 0x0p+0 0x1.4p+3 [x;y 0x1.0000000000001p+2")
set(other_line_5 "run_batch out 0x0p+0 0x1.4p+1")
set(other "${head}${other_line_3}\n${line_4}\n${other_line_5}\n")
string(CONCAT problem "${differ} on line 3, at byte 39 of that line, where the first run printed "
    "0x70 and the second 0x2e.\n"
    "--- line 3 of the first run's stdout ---\n${line_3}\n"
    "--- line 3 of the second run's stdout ---\n${other_line_3}")
_check(1 ${tool} "" "${first}" "${other}" second "${problem}")

# 2. The second output ends before line 5.
string(CONCAT problem "${differ} on line 5. The second run's is shorter, a prefix of the first "
    "run's: it ends before line 5, where the first run's continues with 0x72.\n"
    "--- line 5 of the first run's stdout ---\n${line_5}")
_check(2 ${tool} "" "${first}" "${head}${line_3}\n${line_4}\n" second "${problem}")

# 3. The first output ends in line 5, without a line feed.
string(CONCAT problem "${differ} on line 5. The first run's is shorter, a prefix of the second "
    "run's: it ends in line 5, without a line feed, where the second run's continues with 0x0a.\n"
    "--- line 5 of the first run's stdout ---\n${line_5}\n"
    "--- line 5 of the second run's stdout ---\n${line_5}")
_check(3 ${tool} "" "${head}${line_3}\n${line_4}\n${line_5}" "${first}" second "${problem}")

# 4. The second output has one more line, an empty one.
string(CONCAT problem "${differ} on line 6. The first run's is shorter, a prefix of the second "
    "run's: it ends before line 6, where the second run's continues with 0x0a.\n"
    "--- line 6 of the second run's stdout ---\n")
_check(4 ${tool} "" "${first}" "${first}\n" second "${problem}")

# 5. The lines of a conversion and of the conversion back, without their report lines.
string(CONCAT conversion_text "describe {\"conversions\":[{\"from\":\"a\",\"to\":\"b\"}]}\n"
    "convert a to b: text of 1 bytes\nx\nback b to a: text of 1 bytes\nx\n")
_check(5 praenora_conversion_print "" "${conversion_text}" "${conversion_text}" first
    "praenora_conversion_print (first run): no conversion and conversion back in the output")

# 6. and 7. A run exits with an error.
_check(6 ${tool} first "${first}" "${first}" first "${tool} (first run): exit code 1, expected 0")
_check(7 ${tool} second "${first}" "${other}" second
    "${tool} (second run): exit code 1, expected 0")

# 8. An output without line 4, the batch, and 9. one without line 3, the simulation.
_check(8 ${tool} "" "${head}${line_3}\n${line_5}\n" "${first}" first
    "${tool} (first run): no simulation and batch in the output")
_check(9 ${tool} "" "${first}" "${head}${line_4}\n${line_5}\n" second
    "${tool} (second run): no simulation and batch in the output")

# 10. A sanitizer's report after the simulation and batch on standard output, and 11. on standard
# error.
set(report "==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x1\n")
_check(10 ${tool} "" "${first}${report}" "${first}" first
    "${tool} (first run): a sanitizer reported a problem")
_check(11 ${tool} "" "${first}" "${other}" second
    "${tool} (second run): a sanitizer reported a problem" second "${report}")

# 12. The second output ends line 1 with a carriage return and a line feed. Line 1 holds 46 bytes
# (क is three), so the outputs first differ at its byte 47: a line feed (0x0a) in the first, a
# carriage return (0x0d) in the second.
string(CONCAT looks_the_same "That line looks the same in both below: it is shown as CMake "
    "captures it, without a carriage return before its line feed and without NUL bytes.\n"
    "--- line 1 of the first run's stdout ---\n${line_1}\n"
    "--- line 1 of the second run's stdout ---\n${line_1}")
string(CONCAT problem "${differ} on line 1, at byte 47 of that line, where the first run printed "
    "0x0a and the second 0x0d. ${looks_the_same}")
string(REPLACE "${line_1}\n" "${line_1}\r\n" carriage_return "${first}")
_check(12 ${tool} "" "${first}" "${carriage_return}" second "${problem}")

# 13. The second output holds a NUL byte after describe_model, the first 14 bytes of line 1, so the
# outputs first differ at its byte 15: a space (0x20) in the first, a NUL byte (0x00) in the second.
string(CONCAT problem "${differ} on line 1, at byte 15 of that line, where the first run printed "
    "0x20 and the second 0x00. ${looks_the_same}")
string(REPLACE "describe_model " "describe_model<NUL> " nul_output "${first}")
_check(13 ${tool} "" "${first}" "${nul_output}" second "${problem}")

# 14. The same bytes twice, with a carriage return before each line feed and a NUL byte: the count
# is of every byte, not of the text that CMake captures.
string(REPLACE "\n" "\r\n" same "${nul_output}")
set(directory "${WORK}/14")
_write("${directory}/first.txt" "${same}")
_write("${directory}/second.txt" "${same}")
file(SIZE "${directory}/first.txt" size)
_run_twice("${directory}" ${tool})
if(NOT exit_code STREQUAL "0" OR
   NOT out MATCHES "^-- Two processes printed the same ${size} bytes\\.\n$")
    _fail("14: run_twice.cmake did not pass two runs that printed the same ${size} bytes.")
endif()
message(STATUS "14: run_twice.cmake passes two runs that printed the same ${size} bytes")
