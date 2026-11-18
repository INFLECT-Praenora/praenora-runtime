# Running commands in the CMake drivers of the tests (tests/cli/run_cli.cmake,
# tests/install/run_install_test.cmake, tests/perf/run_perf.cmake,
# tests/providers/check_exports.cmake, tests/providers/run_twice.cmake), which include this file.
# Script mode (cmake -P); it needs only CMake.
#
# praenora_test_run(<prefix> <what> <command> [AFTER_PROBLEM <text>])
#   Runs the command, a ;-list of its program and then its arguments, handed over as one argument
#   ("${command}"), and sets <prefix>_EXIT, <prefix>_OUT and <prefix>_ERR in the caller's scope:
#   its exit code (or why it did not run), its standard output and its standard error. Each element
#   of the list reaches the program as one argument, as it is, an empty one included (one spelled
#   like a keyword of execute_process(), such as COMMAND or TIMEOUT, is read as that keyword). A
#   command handed over as several arguments (${command}, unquoted) is refused: CMake drops the
#   empty elements of a list that it expands unquoted. A sanitizer report in either output fails
#   the test with "<what>: a sanitizer reported a problem", whatever the exit code: a sanitizer
#   build may report a problem and still exit with 0. With AFTER_PROBLEM, <text>, handed over as
#   one argument too, follows that problem in the report, as it is: tests/providers/run_twice.cmake
#   adds the outputs of the first of its two runs to a failure of the second.
#
# praenora_test_fail(<what> <problem> <command> <exit code> <stdout> <stderr>)
#   Fails the test with a report in one message(FATAL_ERROR): "<what>: <problem>", then the
#   command (a ;-list), its exit code and both outputs, each output under a heading. Each line of
#   the report starts with a space: message() prints such a line as it is, after its own indent of
#   two spaces, and formats every other line, one that starts with a tab included, as a paragraph,
#   wrapped near 77 columns at spaces, its runs of spaces collapsed, set apart by blank lines. So
#   the problem and both outputs are shown line by line as they were printed, each line with a
#   space added at its start. An output that ends with a line feed thus ends with a line that holds
#   only that space, and an empty output is shown as one such line. The outputs are not printed
#   apart with message(NOTICE), which prints a text as it is: that would print them before CMake's
#   "CMake Error at" line, away from the rest of the report.
#
#   The command is shown on one line, its arguments separated by spaces. An argument that is empty
#   or holds whitespace (a space, tab, line feed, vertical tab, form feed or carriage return) or a
#   quote character (" or ') is shown in double quotes, inside which a backslash precedes each \
#   and ", and a tab, line feed, vertical tab, form feed or carriage return is written \t, \n, \v,
#   \f or \r. Every other argument is shown as it is.
include_guard(GLOBAL)

function(praenora_test_fail what problem command exit_code out err)
    string(ASCII 11 vertical_tab)
    string(ASCII 12 form_feed)
    set(shown "")
    foreach(argument IN LISTS command)
        if(argument STREQUAL "" OR
           argument MATCHES "[ \t\n${vertical_tab}${form_feed}\r\"']")
            string(REPLACE "\\" "\\\\" argument "${argument}")
            string(REPLACE "\"" "\\\"" argument "${argument}")
            string(REPLACE "\t" "\\t" argument "${argument}")
            string(REPLACE "\n" "\\n" argument "${argument}")
            string(REPLACE "${vertical_tab}" "\\v" argument "${argument}")
            string(REPLACE "${form_feed}" "\\f" argument "${argument}")
            string(REPLACE "\r" "\\r" argument "${argument}")
            set(argument "\"${argument}\"")
        endif()
        string(APPEND shown " ${argument}")
    endforeach()
    string(CONCAT report "${what}: ${problem}\n"
        "command:${shown}\n"
        "exit code: ${exit_code}\n"
        "--- stdout ---\n${out}\n"
        "--- stderr ---\n${err}")
    # Each line of the report starts with a space.
    string(REPLACE "\n" "\n " report " ${report}")
    message(FATAL_ERROR "${report}")
endfunction()

function(praenora_test_run prefix what command)
    set(after_problem "")
    if(ARGC EQUAL 5 AND "${ARGV3}" STREQUAL "AFTER_PROBLEM")
        set(after_problem "${ARGV4}")
    elseif(NOT ARGC EQUAL 3)
        message(FATAL_ERROR "${what}: praenora_test_run() was handed the command as several "
            "arguments; hand it over as one, quoted (\"\${command}\"), because CMake drops the "
            "empty elements of a list that it expands unquoted (it takes <prefix> <what> "
            "<command>, then optionally AFTER_PROBLEM and its text, as one argument too)")
    endif()
    # execute_process(COMMAND ${command}) would drop them as well. The call is built instead with
    # one quoted variable reference per element, which passes the element as one argument, as it
    # is: its value is not read as CMake code.
    set(call "execute_process(COMMAND")
    set(index 0)
    foreach(argument IN LISTS command)
        set(argument_${index} "${argument}")
        string(APPEND call " \"\${argument_${index}}\"")
        math(EXPR index "${index} + 1")
    endforeach()
    string(APPEND call " RESULT_VARIABLE exit_code OUTPUT_VARIABLE out ERROR_VARIABLE err)")
    cmake_language(EVAL CODE "${call}")
    # What a report of AddressSanitizer, LeakSanitizer, ThreadSanitizer or
    # UndefinedBehaviorSanitizer looks like.
    set(sanitizer_report "runtime error:|AddressSanitizer|LeakSanitizer|ThreadSanitizer")
    if(out MATCHES "${sanitizer_report}" OR err MATCHES "${sanitizer_report}")
        praenora_test_fail("${what}" "a sanitizer reported a problem${after_problem}"
            "${command}" "${exit_code}" "${out}" "${err}")
    endif()
    set(${prefix}_EXIT "${exit_code}" PARENT_SCOPE)
    set(${prefix}_OUT "${out}" PARENT_SCOPE)
    set(${prefix}_ERR "${err}" PARENT_SCOPE)
endfunction()
