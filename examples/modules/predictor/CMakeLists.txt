# example-predictor: capability example.predict@0.1.0 (src/predictor.cpp).
praenora_example_install(example-predictor _praenora_install)
praenora_add_module(example_predictor
    NAME example-predictor
    VERSION 0.1.0
    CAPABILITIES example.predict@0.1.0
    SOURCES src/predictor.cpp
    MANIFEST_TEMPLATE example-predictor.pmm.json.in
    TEST_VECTORS vectors.json
    OUTPUT_DIRECTORY "${PRAENORA_EXAMPLE_MODULES_DIR}/example-predictor"
    ${_praenora_install}
    LINK_LIBRARIES nlohmann_json::nlohmann_json)
praenora_example_module(example_predictor)
