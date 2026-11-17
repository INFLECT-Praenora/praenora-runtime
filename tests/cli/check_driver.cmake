# Makes the CLI driver, run_cli.cmake, fail on two commands and checks how it shows them
# (tests/support/run_command.cmake says how): the driver of cli.driver.shows_failing_command
# (tests/cli/CMakeLists.txt). Script mode, cmake -P; it needs only CMake.
#
# 1. A command whose arguments are empty or hold whitespace, a quote character or a backslash. Its
#    program does not exist, so the command does not run. The line that shows it is checked.
# 2. A command that prints lines on both outputs, some longer than 80 columns, some with runs of
#    spaces, and whose standard output does not match a regular expression that spans two lines.
#    The report is checked line by line, all but the command's arguments: each line must be shown
#    whole, as it was printed, with no other line between them or after them. The command runs
#    this script (cmake -P) with
#    PRINT_STDOUT and PRINT_STDERR set: it then prints each of them, followed by a line feed, to
#    standard output and standard error, and checks nothing.
cmake_minimum_required(VERSION 3.24)

if(DEFINED PRINT_STDOUT)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${PRINT_STDOUT}")
    message(NOTICE "${PRINT_STDERR}")
    return()
endif()

set(driver "${CMAKE_CURRENT_LIST_DIR}/run_cli.cmake")

# Fails the test with `problem`, then what run_cli.cmake printed (`out` and `err`), each line with a
# space added at its start, so that message() shows it as it is.
function(_fail problem)
    string(REPLACE "\n" "\n " out " ${out}")
    string(REPLACE "\n" "\n " err " ${err}")
    message(FATAL_ERROR "${problem}\n--- stdout of run_cli.cmake ---\n${out}\n"
        "--- stderr of run_cli.cmake ---\n${err}")
endfunction()

# 1. The command, and the line that must show it. The whitespace characters are inside arguments:
# cmake -D drops spaces, tabs and carriage returns from the end of a value.
string(ASCII 11 vertical_tab)
string(ASCII 12 form_feed)
set(command "no such program" plain "" "two words" "--title=\"hi\"" "it's" [[C:\dir]]
    [[C:\Program Files]] "a\tb" "a\nb" "a${vertical_tab}b" "a${form_feed}b" "a\rb")
string(CONCAT expected [[command: "no such program" plain "" "two words" "--title=\"hi\"" ]]
    [["it's" C:\dir "C:\\Program Files" "a\tb" "a\nb" "a\vb" "a\fb" "a\rb"]])

execute_process(COMMAND "${CMAKE_COMMAND}" "-DCMD=${command}" -P "${driver}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(exit_code STREQUAL "0")
    _fail("run_cli.cmake did not fail on a program that does not exist.")
endif()
# The line ends after the command: CMake did not wrap it.
string(FIND "${err}" "${expected}\n" position)
if(position EQUAL -1)
    _fail("run_cli.cmake did not show the line\n ${expected}")
endif()
message(STATUS "run_cli.cmake shows the command that failed as\n ${expected}")

# 2. What the command prints, and the regular expression its standard output must match (none of
# them holds a semicolon, which would split the command's list).
string(CONCAT stdout_line_1 [[{"command":"version","result":{"runtime":"0.1.0","sdk":"0.1.0"},]]
    [["text":"two  spaces,   three"}]])
set(stdout_line_2 "    an indented line,  with  runs  of  spaces")
string(CONCAT stderr_line "usage: praenora run --config <file> [--module-path <dir>]...  "
    "[--provider-path <dir>]... [--json]")
set(regex "no such text\non two   lines")
set(command "${CMAKE_COMMAND}" "-DPRINT_STDOUT=${stdout_line_1}\n${stdout_line_2}"
    "-DPRINT_STDERR=${stderr_line}" -P "${CMAKE_CURRENT_LIST_FILE}")
# The report as it must be shown, on lines that start with three spaces (CMake's indent of two and
# the space that run_command.cmake adds): the problem's two lines, which the line that shows the
# command follows, and the exit code and both outputs, which come after it. Both outputs end with
# a line feed, so a line that holds only those spaces follows the last line of each.
string(CONCAT shown_problem "\n   command: stdout does not match 'no such text\n"
    "   on two   lines'\n   command: ")
string(CONCAT shown_outputs "\n   exit code: 0\n"
    "   --- stdout ---\n   ${stdout_line_1}\n   ${stdout_line_2}\n   \n"
    "   --- stderr ---\n   ${stderr_line}\n   \n")

execute_process(COMMAND "${CMAKE_COMMAND}" "-DCMD=${command}" "-DEXPECT_STDOUT_REGEX=${regex}"
        -P "${driver}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(exit_code STREQUAL "0")
    _fail("run_cli.cmake did not fail on a standard output that does not match.")
endif()
# Both are found, the problem first, with only the command's arguments between them, which hold
# no line feed; the character after the outputs is not a space, so no line of the report follows.
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
    string(CONCAT problem "run_cli.cmake did not show, as they were printed, the problem\n"
        "${shown_problem}\nthen, on the same line, the command's arguments and then the rest of "
        "the report, with no line after it\n${shown_outputs}")
    _fail("${problem}")
endif()
message(STATUS "run_cli.cmake shows the report on the command that failed as"
    "${shown_problem}...${shown_outputs}")
