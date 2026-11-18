# Runs a print tool twice, each time in a new process, and checks that both print the same bytes:
# the driver of the providers.<name>.processes tests (tests/providers/CMakeLists.txt). Script mode,
# cmake -P, with these variables:
#
#   PRINT=<file>      the print tool's executable
#   TOOL=<name>       optional: the print tool PRINT is, praenora_simulation_print (the default)
#                     or praenora_conversion_print; any other name is an error
#   MANIFEST=<file>   the provider manifest
#   FORMAT=<value>    the print tool's three arguments after the manifest: for
#   MODEL=<file>      praenora_simulation_print, the model format, the model file and the end time
#   END=<value>       of its runs; for praenora_conversion_print, the format to convert from, the
#                     source file and the format to convert to
#   WORK=<directory>  a directory of the build tree, which it makes, where it writes each run's
#                     standard output, every byte as it was printed, to first.out and second.out
#
# The test fails when either run exits with a nonzero code, prints a sanitizer report
# (tests/support/run_command.cmake) or prints no result (praenora_simulation_print: no simulation
# and batch; praenora_conversion_print: no conversion and conversion back), or when the two
# standard outputs differ in any byte. Those checks of each run read its outputs as CMake captures
# them, without the carriage return of each carriage return and line feed and without NUL bytes;
# the two standard outputs are compared as the files hold them, byte for byte. The report names the
# tool. When a run fails, it also names the run, as in
# "praenora_simulation_print (second run): exit code 1, expected 0", and shows that run's command,
# exit code and outputs; before those of the second run, it shows the first run's standard output
# and standard error. When the outputs differ, it names the first line where the two standard
# outputs differ, by its number, and shows that line as each run printed it, as CMake captures it.
# When one output is a prefix of the other, it says which one is shorter and names the longer one's
# next byte in hexadecimal; otherwise it also names the byte of that line where they first differ,
# counted in the bytes as printed, and that byte of each output in hexadecimal, and, when the line
# looks the same in both as shown, it says why. The first run's standard output and standard error
# follow, and then the second run's command, exit code and outputs. Where the outputs differ is
# found from their bytes, as file(READ ... HEX) reads them, two hexadecimal digits for each. Their
# texts are handled with string(FIND), string(SUBSTRING) and string(LENGTH), never turned into
# lists: CMake's list commands split at ; and treat [ and ] specially, and both tools' outputs hold
# JSON.
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

# Sets the variable named `count` to the number of bytes that the outputs `a` and `b`, given as
# hexadecimal digits, two for each byte, have in common at their start. That start is found by
# halving: a start of some bytes is common when a longer one is.
function(_common_bytes a b count)
    string(LENGTH "${a}" a_length)
    string(LENGTH "${b}" b_length)
    # The number of bytes lies between low and high.
    set(low 0)
    math(EXPR high "${a_length} / 2")
    if(b_length LESS a_length)
        math(EXPR high "${b_length} / 2")
    endif()
    while(low LESS high)
        math(EXPR middle "(${low} + ${high} + 1) / 2")
        math(EXPR digits "${middle} * 2")
        string(SUBSTRING "${a}" 0 ${digits} a_start)
        string(SUBSTRING "${b}" 0 ${digits} b_start)
        if(a_start STREQUAL b_start)
            set(low ${middle})
        else()
            math(EXPR high "${middle} - 1")
        endif()
    endwhile()
    set(${count} ${low} PARENT_SCOPE)
endfunction()

foreach(variable IN ITEMS PRINT MANIFEST FORMAT MODEL END WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "run_twice.cmake: ${variable} is required")
    endif()
endforeach()

# What each tool's output must show: two regular expressions, and the problem when it does not.
if(NOT DEFINED TOOL)
    set(TOOL praenora_simulation_print)
endif()
if(TOOL STREQUAL "praenora_simulation_print")
    set(shows_1 "\nsimulate rows 0x0p\\+0 ")
    set(shows_2 "\nrun_batch outcomes 0 0 0\n")
    set(shows_nothing "no simulation and batch in the output")
elseif(TOOL STREQUAL "praenora_conversion_print")
    set(shows_1 "\nconvert [^\n]*: report [{]")
    set(shows_2 "\nback [^\n]*: report [{]")
    set(shows_nothing "no conversion and conversion back in the output")
else()
    message(FATAL_ERROR "run_twice.cmake: TOOL '${TOOL}' is neither praenora_simulation_print "
        "nor praenora_conversion_print")
endif()

set(command "${PRINT}" "${MANIFEST}" "${FORMAT}" "${MODEL}" "${END}")
# The outputs of an earlier test, which a run that fails would leave next to its own.
file(REMOVE "${WORK}/first.out" "${WORK}/second.out")
# What a report on the first run adds after its problem (nothing), then what a report on the second
# run adds: the first run's outputs. The report on two outputs that differ adds it too.
set(after_problem "")
foreach(run IN ITEMS first second)
    string(TOUPPER "${run}" prefix)
    set(what "${TOOL} (${run} run)")
    praenora_test_run(${prefix} "${what}" "${command}" AFTER_PROBLEM "${after_problem}"
        OUTPUT_FILE "${WORK}/${run}.out")
    if(NOT ${prefix}_EXIT STREQUAL "0")
        praenora_test_fail("${what}" "exit code ${${prefix}_EXIT}, expected 0${after_problem}"
            "${command}" "${${prefix}_EXIT}" "${${prefix}_OUT}" "${${prefix}_ERR}")
    endif()
    if(NOT ${prefix}_OUT MATCHES "${shows_1}" OR NOT ${prefix}_OUT MATCHES "${shows_2}")
        praenora_test_fail("${what}" "${shows_nothing}${after_problem}"
            "${command}" "${${prefix}_EXIT}" "${${prefix}_OUT}" "${${prefix}_ERR}")
    endif()
    string(CONCAT after_problem "\n--- stdout of the first run ---\n${FIRST_OUT}\n"
        "--- stderr of the first run ---\n${FIRST_ERR}\n"
        "Below are the second run's command, exit code and outputs.")
endforeach()
# Each standard output's bytes.
file(READ "${WORK}/first.out" first_bytes HEX)
file(READ "${WORK}/second.out" second_bytes HEX)
if(NOT first_bytes STREQUAL second_bytes)
    # The bytes before the first one that differs, `common` of them, with a space after each, so
    # that a line feed, 0a, is found only where a byte starts.
    _common_bytes("${first_bytes}" "${second_bytes}" common)
    math(EXPR digits "${common} * 2")
    string(SUBSTRING "${first_bytes}" 0 ${digits} start)
    string(REGEX REPLACE "(..)" "\\1 " start "${start}")
    # The line of that byte: one more than the line feeds before it.
    string(REPLACE "0a " "" start_without_line_feeds "${start}")
    string(LENGTH "${start}" start_length)
    string(LENGTH "${start_without_line_feeds}" without_length)
    math(EXPR number "(${start_length} - ${without_length}) / 3 + 1")
    # Where that byte is in its line, counted from 1.
    string(FIND "${start}" "0a " line_feed REVERSE)
    if(line_feed EQUAL -1)
        math(EXPR byte "${common} + 1")
    else()
        math(EXPR byte "${common} - ${line_feed} / 3")
    endif()
    # That line as each run printed it, as CMake captures it, without its line feed. Capturing an
    # output keeps its line feeds, so its lines are those of the bytes. A run whose output ends
    # before that line printed none.
    foreach(run IN ITEMS first second)
        string(TOUPPER "${run}" prefix)
        # Not `rest`, the name of _take_line()'s parameter, which would hide the variable.
        set(${run}_rest "${${prefix}_OUT}")
        foreach(line_index RANGE 1 ${number})
            _take_line(${run}_rest ${run}_line)
        endforeach()
        string(REGEX REPLACE "\n$" "" ${run}_shown "${${run}_line}")
    endforeach()
    string(CONCAT problem "two processes printed different results: their standard outputs first "
        "differ on line ${number}")
    # Whether one output is a prefix of the other, and which: the one whose every byte is common.
    string(LENGTH "${first_bytes}" first_digits)
    string(LENGTH "${second_bytes}" second_digits)
    set(shorter "")
    if(digits EQUAL first_digits)
        set(shorter first)
        set(longer second)
    elseif(digits EQUAL second_digits)
        set(shorter second)
        set(longer first)
    endif()
    if(NOT shorter STREQUAL "")
        if(byte EQUAL 1)
            set(end "ends before line ${number}")
        else()
            set(end "ends in line ${number}, without a line feed")
        endif()
        string(SUBSTRING "${${longer}_bytes}" ${digits} 2 next_byte)
        string(APPEND problem ". The ${shorter} run's is shorter, a prefix of the ${longer} "
            "run's: it ${end}, where the ${longer} run's continues with 0x${next_byte}.")
    else()
        string(SUBSTRING "${first_bytes}" ${digits} 2 first_byte)
        string(SUBSTRING "${second_bytes}" ${digits} 2 second_byte)
        string(APPEND problem ", at byte ${byte} of that line, where the first run printed "
            "0x${first_byte} and the second 0x${second_byte}.")
        if(first_shown STREQUAL second_shown)
            string(APPEND problem " That line looks the same in both below: it is shown as CMake "
                "captures it, without a carriage return before its line feed and without NUL "
                "bytes.")
        endif()
    endif()
    foreach(run IN ITEMS first second)
        if(NOT ${run}_line STREQUAL "")
            string(APPEND problem
                "\n--- line ${number} of the ${run} run's stdout ---\n${${run}_shown}")
        endif()
    endforeach()
    string(APPEND problem "${after_problem}")
    praenora_test_fail("${TOOL}" "${problem}"
        "${command}" "${SECOND_EXIT}" "${SECOND_OUT}" "${SECOND_ERR}")
endif()
string(LENGTH "${first_bytes}" digits)
math(EXPR length "${digits} / 2")
message(STATUS "Two processes printed the same ${length} bytes.")
