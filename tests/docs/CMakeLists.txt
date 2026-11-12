# Documentation test (label docs): docs.consistency runs check_docs.cmake, a CMake script that
# reads only the source tree: every error code is in the error taxonomy, every schema file in the
# protocol specification, every ADR in the ADR index, every relative link in README.md, AGENTS.md
# and docs/ resolves, and the open-questions register has its rows. It builds nothing and needs no
# network.
add_test(NAME docs.consistency
    COMMAND "${CMAKE_COMMAND}"
        "-DPRAENORA_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_docs.cmake")
set_tests_properties(docs.consistency PROPERTIES
    LABELS docs
    TIMEOUT 120)
