# Makes the CLI driver, run_cli.cmake, fail on a command whose arguments are empty or hold
# whitespace, a quote character or a backslash, and checks the line that shows the command
# (tests/support/run_command.cmake says how it is shown): the driver of
# cli.driver.shows_failing_command (tests/cli/CMakeLists.txt). Script mode, cmake -P; it needs only
# CMake. The command's program does not exist, so the command does not run and the driver fails.
cmake_minimum_required(VERSION 3.24)

string(ASCII 11 vertical_tab)
string(ASCII 12 form_feed)
# The command, and the line that must show it. The whitespace characters are inside arguments:
# cmake -D drops spaces, tabs and carriage returns from the end of a value.
set(command "no such program" plain "" "two words" "--title=\"hi\"" "it's" [[C:\dir]]
    [[C:\Program Files]] "a\tb" "a\nb" "a${vertical_tab}b" "a${form_feed}b" "a\rb")
string(CONCAT expected [[command: "no such program" plain "" "two words" "--title=\"hi\"" ]]
    [["it's" C:\dir "C:\\Program Files" "a\tb" "a\nb" "a\vb" "a\fb" "a\rb"]])

execute_process(COMMAND "${CMAKE_COMMAND}" "-DCMD=${command}"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_cli.cmake"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(exit_code STREQUAL "0")
    message(FATAL_ERROR "run_cli.cmake did not fail on a program that does not exist.\n"
        "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()
# The line ends after the command: CMake did not wrap it.
string(FIND "${err}" "${expected}\n" position)
if(position EQUAL -1)
    # The line that is missing starts with a space, so CMake shows it as it is.
    message(FATAL_ERROR "run_cli.cmake did not show the line\n ${expected}\n"
        "Its standard error:\n${err}")
endif()
message(STATUS "run_cli.cmake shows the command that failed as\n ${expected}")
