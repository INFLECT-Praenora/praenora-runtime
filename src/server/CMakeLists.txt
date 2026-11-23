# praenora_server: the control API, a front end over the public C++ API beside the praenora tool
# and the Python extension (namespace praenora::server). Its logic is plain functions over plain
# request and response types (http_types.hpp), testable without sockets: addresses, scopes and
# tokens, the routes and their answers (ControlApi), the runs of both modes (RunRegistry,
# SingleRun), the Prometheus text and the OpenAPI document. Over them, the listener (listener.cpp,
# cpp-httplib's server on a Unix socket or a TCP address) and the client of `praenora status
# --server` (client.cpp, cpp-httplib's client), the only two sources that include httplib.h.
# Static and internal: never installed or exported. It links the runtime's shared library,
# praenora_frontend, nlohmann/json and cpp-httplib's include directory (praenora_httplib, with
# ws2_32 on Windows and CPPHTTPLIB_SEND_FLAGS on other POSIX systems than macOS).
add_library(praenora_server STATIC
    http_types.hpp
    address.hpp
    address.cpp
    clock.hpp
    clock.cpp
    tokens.hpp
    tokens.cpp
    run_source.hpp
    run_registry.hpp
    run_registry.cpp
    prometheus.hpp
    prometheus.cpp
    openapi.hpp
    openapi.cpp
    control_api.hpp
    control_api.cpp
    listener.hpp
    listener.cpp
    client.hpp
    client.cpp)
target_include_directories(praenora_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(praenora_server
    PUBLIC Praenora::Runtime
    PRIVATE praenora_frontend nlohmann_json::nlohmann_json praenora_httplib Threads::Threads)
praenora_target_warnings(praenora_server)
