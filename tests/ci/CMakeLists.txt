# Tests of the CI tooling (label ci). ci.fetch_runtime_deps (check_fetch_runtime_deps.cmake) runs
# cmake/praenora_fetch_runtime_deps.cmake, the script that downloads, checks and unpacks a pinned
# dependency bundle, on fake bundle archives that it downloads from file:// URLs, with a pin file
# of the test's own, and checks the bundle directory it prints, the GITHUB_ENV line, what a second
# run reuses and its errors. ci.fetch_runtime_pyenv (check_fetch_runtime_pyenv.cmake) does the
# same for cmake/praenora_fetch_runtime_pyenv.cmake, the script for a pinned Python environment
# bundle, which shares its implementation (cmake/praenora_fetch_bundle.cmake) with the first; its
# driver takes every name of the script it checks from the arguments below, so it can check either
# script.
# ci.workflow_consistency (check_workflow_consistency.cmake) reads the workflow
# .github/workflows/build.yml as text and checks that its matrix keys are the platforms of
# cmake/PraenoraRuntimeDepsPin.cmake and of cmake/PraenoraRuntimePyenvPin.cmake, each with its
# ci-<key> presets in CMakePresets.json, that its runner labels are the fixed ones of each
# platform, and that it pins each action to a commit. They run only CMake and need no network.
add_test(NAME ci.fetch_runtime_deps
    COMMAND "${CMAKE_COMMAND}"
        "-DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/praenora_fetch_runtime_deps.cmake"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fetch-runtime-deps"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_fetch_runtime_deps.cmake")
set_tests_properties(ci.fetch_runtime_deps PROPERTIES
    LABELS ci
    TIMEOUT 120)

add_test(NAME ci.fetch_runtime_pyenv
    COMMAND "${CMAKE_COMMAND}"
        -DNAME=ci.fetch_runtime_pyenv
        "-DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/praenora_fetch_runtime_pyenv.cmake"
        -DPIN_NAME=PraenoraRuntimePyenvPin.cmake
        -DPREFIX=PRAENORA_RUNTIME_PYENV
        -DMANIFEST=praenora-runtime-pyenv.json
        -DSTEM=praenora-runtime-pyenv
        -DOTHER_PREFIX=PRAENORA_RUNTIME_DEPS
        -DOTHER_MANIFEST=praenora-runtime-deps.json
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fetch-runtime-pyenv"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_fetch_runtime_pyenv.cmake")
set_tests_properties(ci.fetch_runtime_pyenv PROPERTIES
    LABELS ci
    TIMEOUT 120)

add_test(NAME ci.workflow_consistency
    COMMAND "${CMAKE_COMMAND}"
        "-DWORKFLOW=${PROJECT_SOURCE_DIR}/.github/workflows/build.yml"
        "-DPIN_FILE=${PROJECT_SOURCE_DIR}/cmake/PraenoraRuntimeDepsPin.cmake"
        "-DPYENV_PIN_FILE=${PROJECT_SOURCE_DIR}/cmake/PraenoraRuntimePyenvPin.cmake"
        "-DPRESETS=${PROJECT_SOURCE_DIR}/CMakePresets.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_workflow_consistency.cmake")
set_tests_properties(ci.workflow_consistency PROPERTIES
    LABELS ci
    TIMEOUT 60)
