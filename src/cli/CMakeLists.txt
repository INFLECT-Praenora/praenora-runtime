# The praenora command-line tool: a thin layer over the public C++ API. It uses nlohmann/json
# (header-only) to write its JSON output, praenora_frontend (src/frontend, header-only) for what it
# shares with the Python extension, and praenora_server (src/server, static) for the control API
# (run --listen, serve, status --server); the runtime itself stays its only shared library.
add_executable(praenora_cli
    main.cpp
    args.hpp
    args.cpp
    output.hpp
    output.cpp
    serving.hpp
    serving.cpp
    signals.hpp
    signals.cpp
    table.hpp
    commands/commands.hpp
    commands/catalog.cpp
    commands/graph.cpp
    commands/modules.cpp
    commands/providers.cpp
    commands/run.cpp
    commands/schema.cpp
    commands/serve.cpp
    commands/status.cpp
    commands/validate.cpp
    commands/version.cpp)
set_target_properties(praenora_cli PROPERTIES OUTPUT_NAME praenora)
target_include_directories(praenora_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(praenora_cli PRIVATE
    Praenora::Runtime
    praenora_frontend
    praenora_server
    nlohmann_json::nlohmann_json
    Threads::Threads)
praenora_target_warnings(praenora_cli)
praenora_copy_runtime_dlls(praenora_cli)
# Where the installed tool finds the provider python's environment file, below the installation's
# library directory (src/frontend/python_environment.hpp).
praenora_python_environment_file_definition(praenora_cli "${CMAKE_INSTALL_BINDIR}")
# On Windows the tool's application manifest lets it, and the Python environment it may run, open
# paths of more than 260 characters, where the system allows them (LongPathsEnabled): an
# installation's environment holds deep paths. The tool needs no UTF-8 code page of the process:
# it takes its arguments as UTF-16 (wmain) and calls the wide functions of the system.
if(WIN32)
    target_sources(praenora_cli PRIVATE praenora.manifest)
endif()

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
