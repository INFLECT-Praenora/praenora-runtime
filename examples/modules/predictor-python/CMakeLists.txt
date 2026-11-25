# example-predictor-python: capability example.predict@0.1.0, the example predictor as a Python
# module (example_predictor_python/__init__.py, with numpy), with the parameters, outputs and test
# vectors of example-predictor. Built with praenora_add_python_module(), as a Python module
# project outside this repository builds its modules, only with PRAENORA_WITH_PYTHON_ENVIRONMENT,
# which builds the provider python that runs it (examples/CMakeLists.txt); installed with the
# component Examples, as the other example modules are. The unit tests of its class,
# test_example_predictor_python.py (praenora.sdk's test kit, no runtime), are not among its
# sources: the test python.example_predictor runs them (tests/python_sdk/CMakeLists.txt).
praenora_example_install(example-predictor-python _praenora_install)
praenora_add_python_module(example_predictor_python
    NAME example-predictor-python
    VERSION 0.1.0
    CAPABILITIES example.predict@0.1.0
    CLASS example_predictor_python:Predictor
    PYTHON_SOURCES example_predictor_python
    MANIFEST_TEMPLATE example-predictor-python.pmm.json.in
    TEST_VECTORS vectors.json
    OUTPUT_DIRECTORY "${PRAENORA_EXAMPLE_MODULES_DIR}/example-predictor-python"
    ${_praenora_install})
praenora_example_module(example_predictor_python)
