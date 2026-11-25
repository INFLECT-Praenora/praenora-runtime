# praenora_frontend: what the praenora tool and the Python extension share (frontend.hpp,
# namespace praenora::frontend). Header-only, so each front end compiles it with its own settings
# (the extension with hidden visibility); internal, never installed or exported. The directory also
# holds python_environment.hpp, which the tool and the module host use first thing (the module
# host includes it by this directory, without this target, which links the shared runtime).
add_library(praenora_frontend INTERFACE)
target_include_directories(praenora_frontend INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(praenora_frontend INTERFACE Praenora::Runtime nlohmann_json::nlohmann_json)
