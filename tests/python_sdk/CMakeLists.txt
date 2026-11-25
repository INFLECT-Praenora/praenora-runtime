# The tests of praenora.sdk (label python): the unittest module test_sdk.py, against the sources
# of praenora.sdk in the source tree, run isolated (-I, so PYTHONPATH and the user site are
# ignored), without the site module (-S, so that nothing but the standard library can be
# imported: the package needs no other) and without writing bytecode (-B). The interpreter finds
# the package and status.h through the variables below, which test_sdk.py reads itself.
#   python.sdk                         with the Python environment's interpreter (with
#                                      PRAENORA_WITH_PYTHON_ENVIRONMENT);
#   python.sdk_bindings_interpreter    with the interpreter of the Python bindings (with them, and
#                                      without sanitizers, as python.unittest), which module
#                                      authors may test with: the package imports in any Python
#                                      3.10 or later.
set(_praenora_python_sdk_environment
    "PRAENORA_SDK_PYTHON_DIR=${PROJECT_SOURCE_DIR}/python"
    "PRAENORA_STATUS_HEADER=${PROJECT_SOURCE_DIR}/include/praenora/sdk/status.h")
set(_praenora_python_sdk_tests "")
if(PRAENORA_WITH_PYTHON_ENVIRONMENT)
    add_test(NAME python.sdk
        COMMAND "${PRAENORA_PYTHON_ENVIRONMENT_EXECUTABLE}" -I -S -B -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -p "test_sdk.py" -v)
    list(APPEND _praenora_python_sdk_tests python.sdk)
endif()
if(TARGET praenora_python_bindings AND NOT PRAENORA_SANITIZERS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME python.sdk_bindings_interpreter
        COMMAND "$<TARGET_FILE:Python3::Interpreter>" -I -S -B -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -p "test_sdk.py" -v)
    list(APPEND _praenora_python_sdk_tests python.sdk_bindings_interpreter)
endif()
foreach(_praenora_python_sdk_test IN LISTS _praenora_python_sdk_tests)
    set_tests_properties(${_praenora_python_sdk_test} PROPERTIES
        LABELS python
        TIMEOUT 120
        ENVIRONMENT "${_praenora_python_sdk_environment}")
endforeach()

# Test python.example_predictor (label python), with the examples: the unit tests of the example
# Python module's class (examples/modules/predictor-python/test_example_predictor_python.py), with
# praenora.sdk's test kit, without the runtime. The file is run as a script, isolated and without
# writing bytecode, as above; it finds praenora.sdk and the module's package through the variables
# below (discovery would import the package before the file names its directory).
if(PRAENORA_WITH_PYTHON_ENVIRONMENT AND PRAENORA_BUILD_EXAMPLES)
    set(_praenora_example_python_dir "${PROJECT_SOURCE_DIR}/examples/modules/predictor-python")
    add_test(NAME python.example_predictor
        COMMAND "${PRAENORA_PYTHON_ENVIRONMENT_EXECUTABLE}" -I -B
            "${_praenora_example_python_dir}/test_example_predictor_python.py" -v)
    set(_praenora_example_python_environment
        "PRAENORA_SDK_PYTHON_DIR=${PROJECT_SOURCE_DIR}/python"
        "PRAENORA_EXAMPLE_PYTHON_DIR=${_praenora_example_python_dir}")
    set_tests_properties(python.example_predictor PROPERTIES
        LABELS python
        TIMEOUT 120
        ENVIRONMENT "${_praenora_example_python_environment}")
endif()
