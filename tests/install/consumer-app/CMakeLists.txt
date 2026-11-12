# An application outside praenora-runtime that uses the installed runtime library. The install
# test configures and builds it against an installation (tests/install/run_install_test.cmake).
cmake_minimum_required(VERSION 3.24)
project(praenora_consumer_app VERSION 0.1.0 LANGUAGES CXX)

find_package(Praenora 0.1 REQUIRED COMPONENTS Runtime)
# The Runtime component loads only the runtime library.
if(TARGET Praenora::ModuleSDK)
    message(FATAL_ERROR "find_package(Praenora COMPONENTS Runtime) defined Praenora::ModuleSDK")
endif()

add_executable(consumer_app main.cpp)
target_link_libraries(consumer_app PRIVATE Praenora::Runtime)
