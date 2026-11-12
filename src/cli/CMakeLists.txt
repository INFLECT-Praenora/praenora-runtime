# The praenora command-line tool: a thin layer over the public C++ API. It uses nlohmann/json
# (header-only) to write its JSON output, and praenora_frontend (src/frontend, header-only) for
# what it shares with the Python extension; the runtime itself stays its only library.
add_executable(praenora_cli
    main.cpp
    args.hpp
    args.cpp
    output.hpp
    output.cpp
    signals.hpp
    signals.cpp
    table.hpp
    commands/commands.hpp
    commands/catalog.cpp
    commands/graph.cpp
    commands/modules.cpp
    commands/run.cpp
    commands/schema.cpp
    commands/status.cpp
    commands/validate.cpp
    commands/version.cpp)
set_target_properties(praenora_cli PROPERTIES OUTPUT_NAME praenora)
target_include_directories(praenora_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(praenora_cli PRIVATE
    Praenora::Runtime
    praenora_frontend
    nlohmann_json::nlohmann_json
    Threads::Threads)
praenora_target_warnings(praenora_cli)
praenora_copy_runtime_dlls(praenora_cli)

# Installed into the bin directory. It finds the runtime library through an RPATH relative to
# itself, so the installation can be moved. Windows has no RPATH: there the runtime's DLL and the
# DLLs it loads are installed into the bin directory, beside the tool (src/runtime).
if(PRAENORA_INSTALL_RULES)
    if(APPLE)
        set_target_properties(praenora_cli PROPERTIES
            INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
        set_target_properties(praenora_cli PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    install(TARGETS praenora_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
