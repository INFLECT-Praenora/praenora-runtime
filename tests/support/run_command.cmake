# Running commands in the CMake drivers of the tests (tests/cli/run_cli.cmake,
# tests/install/run_install_test.cmake, tests/perf/run_perf.cmake,
# tests/providers/check_exports.cmake, tests/providers/run_twice.cmake), which include this file.
# Script mode (cmake -P); it needs only CMake.
#
# praenora_test_run(<prefix> <what> <command>...)
#   Runs the command, its program first, and sets <prefix>_EXIT, <prefix>_OUT and <prefix>_ERR in
#   the caller's scope: its exit code (or why it did not run), its standard output and its standard
#   error. A sanitizer report in either output fails the test with "<what>: a sanitizer reported a
#   problem", whatever the exit code: a sanitizer build may report a problem and still exit with 0.
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
#   \f or \r. Every other argument is shown as it is. An empty element of the list is shown as "",
#   though praenora_test_run() does not pass it: CMake drops empty elements when it expands a list
#   unquoted.
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

function(praenora_test_run prefix what)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE exit_code
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    # What a report of AddressSanitizer, LeakSanitizer, ThreadSanitizer or
    # UndefinedBehaviorSanitizer looks like.
    set(sanitizer_report "runtime error:|AddressSanitizer|LeakSanitizer|ThreadSanitizer")
    if(out MATCHES "${sanitizer_report}" OR err MATCHES "${sanitizer_report}")
        praenora_test_fail("${what}" "a sanitizer reported a problem" "${ARGN}" "${exit_code}"
            "${out}" "${err}")
    endif()
    set(${prefix}_EXIT "${exit_code}" PARENT_SCOPE)
    set(${prefix}_OUT "${out}" PARENT_SCOPE)
    set(${prefix}_ERR "${err}" PARENT_SCOPE)
endfunction()
