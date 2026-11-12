# CLI tests: each runs the praenora tool through run_cli.cmake and checks its exit code and output.
# CTest names cli.<command>.<case>, label cli. They use the fixture modules, the test catalog and
# the configurations of tests/fixtures/configs/cli and, when the examples are built, the example
# modules and workflow. Runs write their output directories below the build tree.

set(_praenora_cli_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_cli.cmake")
set(_praenora_cli_tool "$<TARGET_FILE:praenora_cli>")
set(_praenora_cli_out "${CMAKE_CURRENT_BINARY_DIR}/out")
set(_praenora_fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures")
set(_praenora_cli_configs "${_praenora_fixtures}/configs/cli")
set(_praenora_example_config
    "${PROJECT_SOURCE_DIR}/examples/workflows/estimate-predict/workflow.yaml")
# The fixture modules and the test catalog.
set(_praenora_fixture_paths
    --module-path "${PRAENORA_TEST_MODULES_DIR}"
    --catalog-path "${_praenora_fixtures}/catalog")

# praenora_add_cli_test(<name> CMD <argument>... [ENVIRONMENT <name=value>...]
#                       [INTERRUPT <count> READY_FILE <status file>]
#                       [<driver variable> <value>...]...)
#
# Adds CTest test <name> (label cli) that runs the praenora tool with the CMD arguments through
# run_cli.cmake. The other keywords are the driver's variables (see there): SETUP_CMD and AFTER_CMD
# are argument lists like CMD; EXPECT_JSON_PATH, EXPECT_JSON_VALUE and COPY_FILES take several
# values.
# ENVIRONMENT sets environment variables (name=value) for the test. With INTERRUPT (POSIX only),
# a plain POSIX shell runs the CMD command in the foreground (exec, so it keeps SIGINT at its
# default disposition) while a background waiter waits, for at most 120 s, until the status file
# READY_FILE shows the run running, then sends the tool SIGINT `count` times, half a second
# apart; a second signal only when the tool is still alive, which the waiter records by creating
# <READY_FILE>.second-signal first. A tool that never gets ready, or still runs 30 s after the
# last signal, is killed. Such a test times out after 180 s.
function(praenora_add_cli_test name)
    set(commands CMD SETUP_CMD AFTER_CMD)
    set(lists EXPECT_JSON_PATH EXPECT_JSON_VALUE COPY_FILES)
    set(values EXPECT_EXIT EXPECT_STDOUT_REGEX EXPECT_STDERR_REGEX EXPECT_STDOUT_FILE
        EXPECT_JSON_LENGTH EXPECT_FILE CLEAN_DIR AFTER_EXPECT_EXIT INTERRUPT READY_FILE)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "${values}" "${commands};${lists};ENVIRONMENT")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "praenora_add_cli_test(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT DEFINED arg_CMD)
        message(FATAL_ERROR "praenora_add_cli_test(${name}): CMD is required")
    endif()
    set(definitions "")
    foreach(key IN LISTS commands lists)
        if(DEFINED arg_${key})
            set(value "${arg_${key}}")
            if(key IN_LIST commands)
                set(value "${_praenora_cli_tool};${value}")
            endif()
            if(key STREQUAL "CMD" AND DEFINED arg_INTERRUPT)
                # A plain POSIX shell script, without job control: a background waiter (its output
                # redirected, so nothing waits on it) waits for the run to be running, signals
                # the shell's pid, which the tool takes over with exec in the foreground, so it
                # starts with SIGINT at its default disposition. The waiter ends when the tool
                # does, and kills a tool that never gets ready or never ends. Lines are separated
                # by newlines: a ';' would split the list.
                set(script "ready=$1\nsignals=$2\nshift 2\n(\n  waited=0\n")
                string(APPEND script
                    "  until grep -q '\"state\":\"running\"' \"$ready\" 2>/dev/null\n  do\n"
                    "    kill -0 $$ 2>/dev/null || exit 0\n"
                    "    if [ $waited -ge 1200 ]\n    then\n      kill -KILL $$\n      exit 0\n"
                    "    fi\n    sleep 0.1\n    waited=$((waited + 1))\n  done\n"
                    "  sleep 0.5\n  kill -INT $$\n"
                    "  if [ \"$signals\" -ge 2 ]\n  then\n    sleep 0.5\n"
                    "    if kill -0 $$ 2>/dev/null\n    then\n"
                    "      : > \"$ready.second-signal\"\n      kill -INT $$\n    fi\n  fi\n"
                    "  waited=0\n  while kill -0 $$ 2>/dev/null\n  do\n"
                    "    if [ $waited -ge 300 ]\n    then\n      kill -KILL $$\n      exit 0\n"
                    "    fi\n    sleep 0.1\n    waited=$((waited + 1))\n  done\n"
                    ") >/dev/null 2>&1 &\nexec \"$@\"\n")
                set(value "/bin/sh;-c;${script};sh;${arg_READY_FILE};${arg_INTERRUPT};${value}")
            endif()
            # A list reaches the driver as one argument: its separators are escaped here.
            string(REPLACE ";" "\\;" value "${value}")
            list(APPEND definitions "-D${key}=${value}")
        endif()
    endforeach()
    list(REMOVE_ITEM values INTERRUPT READY_FILE)
    foreach(key IN LISTS values)
        if(DEFINED arg_${key})
            list(APPEND definitions "-D${key}=${arg_${key}}")
        endif()
    endforeach()
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}" ${definitions} -P "${_praenora_cli_driver}")
    set_tests_properties(${name} PROPERTIES LABELS cli)
    set(environment ${arg_ENVIRONMENT})
    # As for the GoogleTest tests: AddressSanitizer builds on macOS keep module libraries loaded
    # (tests/CMakeLists.txt says why).
    if(APPLE AND "address" IN_LIST PRAENORA_SANITIZERS)
        list(APPEND environment "PRAENORA_DEBUG_KEEP_LIBRARIES=1")
    endif()
    if(environment)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${environment}")
    endif()
    if(DEFINED arg_INTERRUPT)
        set_tests_properties(${name} PROPERTIES TIMEOUT 180)
    endif()
endfunction()

praenora_add_cli_test(cli.version.json
    CMD version --json
    EXPECT_JSON_PATH ok result.runtime
    EXPECT_JSON_VALUE true ${PRAENORA_RUNTIME_VERSION})

praenora_add_cli_test(cli.validate.bad_parameters
    CMD validate "${_praenora_cli_configs}/bad-parameters/workflow.yaml" ${_praenora_fixture_paths}
    EXPECT_EXIT 3
    EXPECT_STDERR_REGEX "workflow\\.yaml:[0-9]+:[0-9]+: error\\[PARAMETERS_INVALID\\]")
praenora_add_cli_test(cli.validate.document_manifest
    CMD validate --document "${_praenora_fixtures}/documents/valid/module-manifest/full.pmm.json"
    EXPECT_STDOUT_REGEX "valid")

praenora_add_cli_test(cli.modules.check_identity_mismatch
    CMD modules check
        "${PRAENORA_TEST_MODULES_DIR}/fx-identity-mismatch/fx-identity-mismatch.pmm.json"
        ${_praenora_fixture_paths}
    EXPECT_EXIT 1
    EXPECT_STDOUT_REGEX "fail +describe\\.identity"
    EXPECT_STDERR_REGEX "error\\[FAILED\\]")

set(_praenora_committed_asyncapi
    "${PROJECT_SOURCE_DIR}/spec/asyncapi/example.estimate-predict-0.1.0.asyncapi.json")
# Test vectors given with --vectors.
praenora_add_cli_test(cli.modules.check_vectors
    CMD modules check "${PRAENORA_TEST_MODULES_DIR}/fx-echo/fx-echo.pmm.json"
        --vectors "${_praenora_cli_configs}/echo-vectors.json" ${_praenora_fixture_paths}
    EXPECT_STDOUT_REGEX "pass +invoke\\.expected_outputs +the outputs of 1 tick")

# Invalid test vectors fail the checks (exit 1), naming the problem, even when no instance of the
# module can be created.
praenora_add_cli_test(cli.modules.check_invalid_vectors
    CMD modules check
        "${PRAENORA_TEST_MODULES_DIR}/fx-needs-parameter/fx-needs-parameter.pmm.json"
        ${_praenora_fixture_paths}
    EXPECT_EXIT 1
    EXPECT_STDOUT_REGEX
        "fail +invoke\\.result_schema +the test vectors [^\n]*test\\.nothing@0\\.1\\.0")

praenora_add_cli_test(cli.graph.asyncapi_matches_committed
    CMD graph --workflow example.estimate-predict@0.1.0 --format asyncapi
    EXPECT_STDOUT_FILE "${_praenora_committed_asyncapi}")

praenora_add_cli_test(cli.run.module_failure
    CMD run "${_praenora_cli_configs}/module-failure/workflow.yaml" ${_praenora_fixture_paths}
    EXPECT_EXIT 1
    EXPECT_STDERR_REGEX "error\\[STEP_FAILED\\]")
praenora_add_cli_test(cli.run.timeout
    CMD run "${_praenora_cli_configs}/timeout/workflow.yaml" ${_praenora_fixture_paths}
    EXPECT_EXIT 5
    EXPECT_STDERR_REGEX "error\\[STEP_TIMEOUT\\]")
# SIGINT stops a run: it ends as stopped, prints its summary and exits with 130.
if(NOT WIN32)
    praenora_add_cli_test(cli.run.interrupted
        CLEAN_DIR "${_praenora_cli_out}/interrupted"
        CMD run "${_praenora_cli_configs}/interrupted/workflow.yaml" ${_praenora_fixture_paths}
            --status-file "${_praenora_cli_out}/interrupted/status.json"
        INTERRUPT 1
        READY_FILE "${_praenora_cli_out}/interrupted/status.json"
        EXPECT_EXIT 130
        EXPECT_STDOUT_REGEX "state +stopped"
        EXPECT_STDERR_REGEX "error\\[RUN_STOPPED\\]")
    # A second SIGINT ends the process at once, while the run is held in a call that ignores the
    # first one: exit 130 and nothing printed, the tool still alive when the second signal went
    # out (the marker file), and no process left behind.
    praenora_add_cli_test(cli.run.second_signal_exits_at_once
        CLEAN_DIR "${_praenora_cli_out}/hung"
        CMD run "${_praenora_cli_configs}/hung/workflow.yaml" ${_praenora_fixture_paths}
            --status-file "${_praenora_cli_out}/hung/status.json"
        INTERRUPT 2
        READY_FILE "${_praenora_cli_out}/hung/status.json"
        EXPECT_EXIT 130
        EXPECT_STDOUT_REGEX "^$"
        EXPECT_FILE "${_praenora_cli_out}/hung/status.json.second-signal")
endif()

# Configuration, module and output directories whose names hold spaces and non-ASCII characters,
# laid out by each test in a directory of its own.
foreach(_praenora_command IN ITEMS validate run)
    set(_praenora_unicode "${_praenora_cli_out}/päths with spaces (${_praenora_command})")
    set(_praenora_unicode_files
        "${_praenora_cli_configs}/paths/workflow.yaml"
        "${_praenora_unicode}/cönfig dir/workflow.yaml"
        "${_praenora_cli_configs}/source.json" "${_praenora_unicode}/cönfig dir/source.json"
        "${PRAENORA_TEST_MODULES_DIR}/fx-echo/fx-echo.pmm.json"
        "${_praenora_unicode}/mödules ü/fx-echo/fx-echo.pmm.json"
        "$<TARGET_FILE:fx_echo>"
        "${_praenora_unicode}/mödules ü/fx-echo/$<TARGET_FILE_NAME:fx_echo>")
    set(_praenora_unicode_paths
        "${_praenora_unicode}/cönfig dir/workflow.yaml"
        --module-path "${_praenora_unicode}/mödules ü"
        --catalog-path "${_praenora_fixtures}/catalog")
    if(_praenora_command STREQUAL "validate")
        praenora_add_cli_test(cli.validate.unicode_paths
            CLEAN_DIR "${_praenora_unicode}"
            COPY_FILES ${_praenora_unicode_files}
            CMD validate ${_praenora_unicode_paths}
            EXPECT_STDOUT_REGEX "cönfig dir/workflow\\.yaml: valid")
    else()
        praenora_add_cli_test(cli.run.unicode_paths
            CLEAN_DIR "${_praenora_unicode}"
            COPY_FILES ${_praenora_unicode_files}
            CMD run ${_praenora_unicode_paths} --output-dir "${_praenora_unicode}/öutput ñ"
            EXPECT_STDOUT_REGEX "output directory +[^\n]*/öutput ñ\nstate +completed"
            EXPECT_FILE "${_praenora_unicode}/öutput ñ/run-record.json")
    endif()
endforeach()

# A missing input file exits 74 from every command.
set(_praenora_missing "${_praenora_cli_out}/missing")
praenora_add_cli_test(cli.validate.missing_file
    CMD validate "${_praenora_missing}/workflow.yaml"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.run.missing_file
    CMD run "${_praenora_missing}/workflow.yaml"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.graph.missing_file
    CMD graph "${_praenora_missing}/workflow.yaml"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.status.missing_file
    CMD status "${_praenora_missing}/status.json"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.modules.check_missing_manifest
    CMD modules check "${_praenora_missing}/module.pmm.json"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.modules.check_missing_vectors
    CMD modules check "${PRAENORA_TEST_MODULES_DIR}/fx-echo/fx-echo.pmm.json"
        --vectors "${_praenora_missing}/vectors.json" ${_praenora_fixture_paths}
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "cannot read the test vectors")

praenora_add_cli_test(cli.catalog.list
    CMD catalog list
    EXPECT_STDOUT_REGEX "workflow-definition +example\\.estimate-predict +0\\.1\\.0")
praenora_add_cli_test(cli.catalog.show_example
    CMD catalog show example.estimate-predict@0.1.0 --json
    EXPECT_JSON_PATH result.kind result.name
    EXPECT_JSON_VALUE workflow-definition example.estimate-predict)

praenora_add_cli_test(cli.schema.list
    CMD schema list --json
    EXPECT_JSON_PATH result
    EXPECT_JSON_LENGTH 16)
praenora_add_cli_test(cli.schema.show_manifest
    CMD schema show module-manifest
    EXPECT_STDOUT_REGEX "\"\\$id\": \"urn:praenora:schema:module-manifest:0\\.1\"")

praenora_add_cli_test(cli.usage.unknown_command
    CMD frobnicate
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "unknown command 'frobnicate'")
praenora_add_cli_test(cli.usage.command_help
    CMD run --help
    EXPECT_STDOUT_REGEX "usage: praenora run <config.yaml>")
praenora_add_cli_test(cli.usage.invalid_log_format
    CMD catalog list --log-format xml
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "--log-format 'xml' is not a log format")
praenora_add_cli_test(cli.usage.invalid_log_level
    CMD catalog list --log-level loud
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "--log-level")

# --help with --json: the usage is the result of the usual success document.
praenora_add_cli_test(cli.json.help
    CMD run --help --json
    EXPECT_JSON_PATH ok result
    EXPECT_JSON_VALUE true *
    EXPECT_STDOUT_REGEX "^\\{\"ok\":true,\"result\":\"usage: praenora run <config.yaml>")
praenora_add_cli_test(cli.json.help_command
    CMD help modules --json
    EXPECT_JSON_PATH ok result
    EXPECT_JSON_VALUE true *
    EXPECT_STDOUT_REGEX "usage: praenora modules list")

praenora_add_cli_test(cli.json.error_shape
    CMD catalog show no.such-entry@0.1.0 --json
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name error.code diagnostics
    EXPECT_JSON_VALUE false CATALOG_ENTRY_NOT_FOUND 405 *)
praenora_add_cli_test(cli.json.validation_failure
    CMD validate "${_praenora_cli_configs}/bad-parameters/workflow.yaml" ${_praenora_fixture_paths}
        --json
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name diagnostics.0.name diagnostics.0.location.pointer
    EXPECT_JSON_VALUE false PARAMETERS_INVALID PARAMETERS_INVALID /steps/a/parameters/sleep_ms)

# The logging flags beat the environment: the tool puts them into its own environment before the
# runtime reads it, so an invalid PRAENORA_LOG_LEVEL does not matter when --log-level is given.
praenora_add_cli_test(cli.logging.flag_beats_environment
    CMD catalog list --log-level warn
    ENVIRONMENT "PRAENORA_LOG_LEVEL=loud")
praenora_add_cli_test(cli.logging.invalid_environment
    CMD catalog list
    ENVIRONMENT "PRAENORA_LOG_LEVEL=loud"
    EXPECT_EXIT 3
    EXPECT_STDERR_REGEX "PRAENORA_LOG_LEVEL")

if(PRAENORA_BUILD_EXAMPLES)
    set(_praenora_example_modules --module-path "${PRAENORA_EXAMPLE_MODULES_DIR}")

    praenora_add_cli_test(cli.validate.example_ok
        CMD validate "${_praenora_example_config}" ${_praenora_example_modules}
        EXPECT_STDOUT_REGEX "valid")

    praenora_add_cli_test(cli.modules.list_json
        CMD modules list ${_praenora_example_modules} --json
        EXPECT_JSON_PATH ok
        EXPECT_JSON_VALUE true
        EXPECT_STDOUT_REGEX "\"name\":\"example-estimator\"")
    praenora_add_cli_test(cli.modules.inspect_load
        CMD modules inspect example-estimator --load ${_praenora_example_modules} --json
        EXPECT_JSON_PATH result.describe.capabilities result.library_sha256)
    praenora_add_cli_test(cli.modules.check_example
        CMD modules check
            "${PRAENORA_EXAMPLE_MODULES_DIR}/example-estimator/example-estimator.pmm.json"
        EXPECT_STDOUT_REGEX "pass +invoke\\.expected_outputs")

    praenora_add_cli_test(cli.graph.dot
        CMD graph "${_praenora_example_config}" --format dot
        EXPECT_STDOUT_REGEX "estimate.*->.*predict")

    praenora_add_cli_test(cli.run.example
        CLEAN_DIR "${_praenora_cli_out}/example"
        CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/example"
        EXPECT_STDOUT_REGEX "fingerprint +[0-9a-f]+\nticks +12\noutput directory +[^\n]*example"
        EXPECT_FILE "${_praenora_cli_out}/example/run-record.json")
    # An output directory that holds an earlier run is refused (IO_ERROR, exit 74), unless the
    # run may overwrite it.
    praenora_add_cli_test(cli.run.existing_output_dir
        CLEAN_DIR "${_praenora_cli_out}/existing"
        SETUP_CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/existing"
        CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/existing"
        EXPECT_EXIT 74
        EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]"
        AFTER_CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/existing" --overwrite
        AFTER_EXPECT_EXIT 0)

    praenora_add_cli_test(cli.status.reads_file
        CLEAN_DIR "${_praenora_cli_out}/status"
        SETUP_CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/status"
            --status-file "${_praenora_cli_out}/status/status.json"
        CMD status "${_praenora_cli_out}/status/status.json" --json
        EXPECT_JSON_PATH result.kind result.workflow.name result.workflow.state
        EXPECT_JSON_VALUE status-snapshot example.estimate-predict completed)
endif()

# Unit tests of the tool's own helpers (src/cli) and of what it shares with the Python extension
# (praenora_frontend, src/frontend), all header-only.
praenora_add_gtest(praenora_cli_tests LABEL cli
    SOURCES table_test.cpp frontend_test.cpp
    LINK praenora_frontend)
target_include_directories(praenora_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/cli")
