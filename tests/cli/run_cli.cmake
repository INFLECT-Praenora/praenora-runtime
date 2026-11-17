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

if(DEFINED EXPECT_JSON_PATH)
    string(JSON root_type ERROR_VARIABLE json_error TYPE "${RUN_OUT}")
    if(json_error OR NOT root_type STREQUAL "OBJECT")
        _fail("stdout is not one JSON object (${json_error})")
    endif()
    list(LENGTH EXPECT_JSON_PATH path_count)
    math(EXPR last "${path_count} - 1")
    foreach(index RANGE ${last})
        list(GET EXPECT_JSON_PATH ${index} path)
        string(REPLACE "." ";" keys "${path}")
        string(JSON type ERROR_VARIABLE json_error TYPE "${RUN_OUT}" ${keys})
        if(json_error)
            _fail("stdout has no value at ${path} (${json_error})")
        endif()
        if(DEFINED EXPECT_JSON_VALUE)
            list(GET EXPECT_JSON_VALUE ${index} expected)
            if(NOT expected STREQUAL "*")
                if(type STREQUAL "BOOLEAN")
                    string(JSON value GET "${RUN_OUT}" ${keys})
                    if(value)
                        set(value true)
                    else()
                        set(value false)
                    endif()
                elseif(type STREQUAL "NULL")
                    set(value null)
                else()
                    string(JSON value GET "${RUN_OUT}" ${keys})
                endif()
                if(NOT value STREQUAL expected)
                    _fail("the value at ${path} is '${value}', expected '${expected}'")
                endif()
            endif()
        endif()
        if(index EQUAL 0 AND DEFINED EXPECT_JSON_LENGTH)
            string(JSON length LENGTH "${RUN_OUT}" ${keys})
            if(NOT length EQUAL EXPECT_JSON_LENGTH)
                _fail("${path} has ${length} items, expected ${EXPECT_JSON_LENGTH}")
            endif()
        endif()
    endforeach()
endif()

if(DEFINED EXPECT_FILE AND NOT EXISTS "${EXPECT_FILE}")
    _fail("${EXPECT_FILE} does not exist")
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
