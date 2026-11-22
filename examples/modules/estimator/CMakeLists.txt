# example-estimator: capability example.estimate@0.1.0 (src/estimator.cpp).
praenora_example_install(example-estimator _praenora_install)
praenora_add_module(example_estimator
    NAME example-estimator
    VERSION 0.1.0
    CAPABILITIES example.estimate@0.1.0
    SOURCES src/estimator.cpp
    MANIFEST_TEMPLATE example-estimator.pmm.json.in
    TEST_VECTORS vectors.json
    OUTPUT_DIRECTORY "${PRAENORA_EXAMPLE_MODULES_DIR}/example-estimator"
    ${_praenora_install}
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
praenora_example_module(example_estimator)
