# Running commands in the CMake drivers of the tests (tests/cli/run_cli.cmake,
# tests/install/run_install_test.cmake, tests/perf/run_perf.cmake), which include this file. Script
# mode (cmake -P); it needs only CMake.
#
# praenora_test_run(<prefix> <what> <command>...)
#   Runs the command, its program first, and sets <prefix>_EXIT, <prefix>_OUT and <prefix>_ERR in
#   the caller's scope: its exit code (or why it did not run), its standard output and its standard
#   error. A sanitizer report in either output fails the test with "<what>: a sanitizer reported a
#   problem", whatever the exit code: a sanitizer build may report a problem and still exit with 0.
#
# praenora_test_fail(<what> <problem> <command> <exit code> <stdout> <stderr>)
#   Fails the test with "<what>: <problem>", then the command (a ;-list, shown with spaces), its
#   exit code and both outputs.
include_guard(GLOBAL)

function(praenora_test_fail what problem command exit_code out err)
    string(REPLACE ";" " " shown "${command}")
    message(FATAL_ERROR "${what}: ${problem}\n"
        "command: ${shown}\n"
        "exit code: ${exit_code}\n"
        "--- stdout ---\n${out}\n"
        "--- stderr ---\n${err}\n")
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
