# Runs the praenora command-line tool and checks what it did: the driver of the CLI tests
# (tests/cli/CMakeLists.txt). Script mode, cmake -P, with these variables (lists are ;-lists):
#
#   CMD=<command line>             the command to run, the tool first (required)
#   EXPECT_EXIT=<n>                its exit code (default 0)
#   EXPECT_STDOUT_REGEX=<regex>    standard output must match
#   EXPECT_STDERR_REGEX=<regex>    standard error must match
#   EXPECT_STDOUT_FILE=<file>      standard output must equal the file's bytes
#   EXPECT_JSON_PATH=<paths>       standard output is one JSON object with a value at each path:
#                                  object keys and array indices joined by '.', for example
#                                  result.describe.capabilities
#   EXPECT_JSON_VALUE=<values>     the value at each path (true, false and null for those
#                                  values; * for any value)
#   EXPECT_JSON_LENGTH=<n>         the array or object at the first path has n items
#   EXPECT_FILE=<file>             the file exists afterwards
#   EXPECT_FILE_JSON_PATH=<paths>  the file is one JSON object with a value at each path, written
#                                  as for EXPECT_JSON_PATH
#   EXPECT_FILE_JSON_VALUE=<values>
#                                  the value at each of those paths, as for EXPECT_JSON_VALUE
#   CLEAN_DIR=<dir>                removed before anything runs
#   COPY_FILES=<source;destination;...>
#                                  files copied (their directories created) before the commands
#   SETUP_CMD=<command line>       run first; must exit 0
#   AFTER_CMD=<command line>       run last, with exit code AFTER_EXPECT_EXIT (default 0)
#
# Needs only CMake: the commands run through tests/support/run_command.cmake, which passes each
# element of a command line to the program as one argument, an empty one included, and
# string(JSON) reads the output. On a mismatch it prints the command, its exit code and both
# outputs, and fails; so does a command that prints a sanitizer report (sanitizer builds).
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

if(NOT DEFINED CMD)
    message(FATAL_ERROR "run_cli.cmake: CMD is required")
endif()

if(DEFINED CLEAN_DIR)
    file(REMOVE_RECURSE "${CLEAN_DIR}")
endif()

if(DEFINED COPY_FILES)
    list(LENGTH COPY_FILES copy_count)
    math(EXPR copy_last "${copy_count} - 1")
    foreach(copy_index RANGE 0 ${copy_last} 2)
        math(EXPR copy_next "${copy_index} + 1")
        list(GET COPY_FILES ${copy_index} copy_source)
        list(GET COPY_FILES ${copy_next} copy_destination)
        get_filename_component(copy_directory "${copy_destination}" DIRECTORY)
        file(MAKE_DIRECTORY "${copy_directory}")
        file(COPY_FILE "${copy_source}" "${copy_destination}")
    endforeach()
endif()

if(DEFINED SETUP_CMD)
    praenora_test_run(SETUP "setup" "${SETUP_CMD}")
    if(NOT SETUP_EXIT STREQUAL "0")
        praenora_test_fail("setup" "exit code ${SETUP_EXIT}, expected 0" "${SETUP_CMD}"
            "${SETUP_EXIT}" "${SETUP_OUT}" "${SETUP_ERR}")
    endif()
endif()

praenora_test_run(RUN "command" "${CMD}")

# Fails the test, showing what the command did.
function(_fail problem)
    praenora_test_fail("command" "${problem}" "${CMD}" "${RUN_EXIT}" "${RUN_OUT}" "${RUN_ERR}")
endfunction()

if(NOT DEFINED EXPECT_EXIT)
    set(EXPECT_EXIT 0)
endif()
if(NOT RUN_EXIT STREQUAL EXPECT_EXIT)
    _fail("exit code ${RUN_EXIT}, expected ${EXPECT_EXIT}")
endif()
if(DEFINED EXPECT_STDOUT_REGEX AND NOT RUN_OUT MATCHES "${EXPECT_STDOUT_REGEX}")
    _fail("stdout does not match '${EXPECT_STDOUT_REGEX}'")
endif()
if(DEFINED EXPECT_STDERR_REGEX AND NOT RUN_ERR MATCHES "${EXPECT_STDERR_REGEX}")
    _fail("stderr does not match '${EXPECT_STDERR_REGEX}'")
endif()
if(DEFINED EXPECT_STDOUT_FILE)
    file(READ "${EXPECT_STDOUT_FILE}" expected)
    if(NOT RUN_OUT STREQUAL expected)
        _fail("stdout differs from ${EXPECT_STDOUT_FILE}")
    endif()
endif()

# Checks that `document` (named `what` in messages: stdout or a file) is one JSON object with a
# value at each of the paths in the list variable `paths_var`, equal to the value at the same
# index of the list variable `values_var` when that is defined (* for any value); with
# `length_var` defined, the array or object at the first path has that many items.
function(_check_json what document paths_var values_var length_var)
    string(JSON root_type ERROR_VARIABLE json_error TYPE "${document}")
    if(json_error OR NOT root_type STREQUAL "OBJECT")
        _fail("${what} is not one JSON object (${json_error})")
    endif()
    list(LENGTH ${paths_var} path_count)
    math(EXPR last "${path_count} - 1")
    foreach(index RANGE ${last})
        list(GET ${paths_var} ${index} path)
        string(REPLACE "." ";" keys "${path}")
        string(JSON type ERROR_VARIABLE json_error TYPE "${document}" ${keys})
        if(json_error)
            _fail("${what} has no value at ${path} (${json_error})")
        endif()
        if(DEFINED ${values_var})
            list(GET ${values_var} ${index} expected)
            if(NOT expected STREQUAL "*")
                if(type STREQUAL "BOOLEAN")
                    string(JSON value GET "${document}" ${keys})
                    if(value)
                        set(value true)
                    else()
                        set(value false)
                    endif()
                elseif(type STREQUAL "NULL")
                    set(value null)
                else()
                    string(JSON value GET "${document}" ${keys})
                endif()
                if(NOT value STREQUAL expected)
                    _fail("the value at ${path} of ${what} is '${value}', expected '${expected}'")
                endif()
            endif()
        endif()
        if(index EQUAL 0 AND DEFINED ${length_var})
            string(JSON length LENGTH "${document}" ${keys})
            if(NOT length EQUAL ${length_var})
                _fail("${path} of ${what} has ${length} items, expected ${${length_var}}")
            endif()
        endif()
    endforeach()
endfunction()

if(DEFINED EXPECT_JSON_PATH)
    _check_json(stdout "${RUN_OUT}" EXPECT_JSON_PATH EXPECT_JSON_VALUE EXPECT_JSON_LENGTH)
endif()

if(DEFINED EXPECT_FILE AND NOT EXISTS "${EXPECT_FILE}")
    _fail("${EXPECT_FILE} does not exist")
endif()
if(DEFINED EXPECT_FILE_JSON_PATH)
    if(NOT DEFINED EXPECT_FILE)
        message(FATAL_ERROR "run_cli.cmake: EXPECT_FILE_JSON_PATH needs EXPECT_FILE")
    endif()
    file(READ "${EXPECT_FILE}" file_document)
    # No length check for a file: _no_length is never defined.
    _check_json("${EXPECT_FILE}" "${file_document}" EXPECT_FILE_JSON_PATH EXPECT_FILE_JSON_VALUE
        _no_length)
endif()

if(DEFINED AFTER_CMD)
    if(NOT DEFINED AFTER_EXPECT_EXIT)
        set(AFTER_EXPECT_EXIT 0)
    endif()
    praenora_test_run(AFTER "last command" "${AFTER_CMD}")
    if(NOT AFTER_EXIT STREQUAL AFTER_EXPECT_EXIT)
        praenora_test_fail("last command" "exit code ${AFTER_EXIT}, expected ${AFTER_EXPECT_EXIT}"
            "${AFTER_CMD}" "${AFTER_EXIT}" "${AFTER_OUT}" "${AFTER_ERR}")
    endif()
endif()
