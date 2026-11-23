# Python tests (label python): CTest test python.unittest runs the unittest suite of tests/python
# with the interpreter the extension is built for, against the package in the build tree
# (PYTHONPATH). The environment names the fixture modules, the fixture providers, the test
# catalog, the example configuration and, when the examples are built, the example modules
# (tests/python/support.py reads them). It sets PRAENORA_LOG_LEVEL to off, so the output shows
# only the tests' own lines (a test that wants log records asks for them), unsets the other
# PRAENORA_* variables that the runtime reads, so the caller's environment cannot change the
# results, turns the OTLP export off (OTEL_SDK_DISABLED; the test of the export sets the OTEL_*
# variables it reads itself), and keeps Python from writing bytecode beside the tests. The tests
# write files only to temporary directories.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(_praenora_example_config
    "${PROJECT_SOURCE_DIR}/examples/workflows/estimate-predict/workflow.yaml")
set(_praenora_python_environment
    "PYTHONPATH=${PROJECT_BINARY_DIR}/python"
    "PYTHONDONTWRITEBYTECODE=1"
    "PRAENORA_LOG_LEVEL=off"
    "OTEL_SDK_DISABLED=true"
    "PRAENORA_TEST_MODULES_DIR=${PRAENORA_TEST_MODULES_DIR}"
    "PRAENORA_TEST_PROVIDERS_DIR=${PRAENORA_TEST_PROVIDERS_DIR}"
    "PRAENORA_TEST_CATALOG_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/catalog"
    "PRAENORA_EXAMPLE_CONFIG=${_praenora_example_config}")
if(PRAENORA_BUILD_EXAMPLES)
    list(APPEND _praenora_python_environment
        "PRAENORA_EXAMPLE_MODULES_DIR=${PRAENORA_EXAMPLE_MODULES_DIR}")
endif()
set(_praenora_python_unset
    "PRAENORA_MODULE_PATH=unset:"
    "PRAENORA_PROVIDER_PATH=unset:"
    "PRAENORA_CATALOG_PATH=unset:"
    "PRAENORA_LOG_FORMAT=unset:")

add_test(NAME python.unittest
    COMMAND "$<TARGET_FILE:Python3::Interpreter>" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}" -v)
set_tests_properties(python.unittest PROPERTIES
    LABELS python
    TIMEOUT 300
    ENVIRONMENT "${_praenora_python_environment}"
    ENVIRONMENT_MODIFICATION "${_praenora_python_unset}")

if(PRAENORA_SANITIZERS)
    # Built with sanitizers, the extension and the runtime library it links need the sanitizer
    # runtime, which an interpreter that is not instrumented itself does not provide: the import
    # fails unless that runtime is preloaded (DYLD_INSERT_LIBRARIES or LD_PRELOAD), and the
    # interpreter's own allocations are then checked and reported too. The test stays registered
    # but disabled, so sanitizer builds list it as not run. The C++ tests of the API still run.
    set_tests_properties(python.unittest PROPERTIES DISABLED TRUE)
endif()
