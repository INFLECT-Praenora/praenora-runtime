# A module project outside praenora-runtime, as a module team writes one: it needs the installed
# module SDK and a C++ compiler, nothing else. The install test configures and builds it against
# an installation (tests/install/run_install_test.cmake).
cmake_minimum_required(VERSION 3.24)
project(praenora_consumer_module VERSION 0.1.0 LANGUAGES CXX)

find_package(Praenora 0.2 REQUIRED COMPONENTS ModuleSDK)
# The ModuleSDK component loads only the SDK: it looks for no other package and defines no
# runtime target.
get_property(found_packages GLOBAL PROPERTY PACKAGES_FOUND)
get_property(missing_packages GLOBAL PROPERTY PACKAGES_NOT_FOUND)
if(NOT found_packages STREQUAL "Praenora" OR missing_packages)
    message(FATAL_ERROR "find_package(Praenora COMPONENTS ModuleSDK) looked for other packages; "
        "found: ${found_packages}; not found: ${missing_packages}")
endif()
if(TARGET Praenora::Runtime)
    message(FATAL_ERROR "find_package(Praenora COMPONENTS ModuleSDK) defined Praenora::Runtime")
endif()

# The library and its manifest are built into <build>/consumer-echo/ (with a multi-config
# generator, <build>/consumer-echo/<config>/), and installed together into
# <prefix>/<libdir>/praenora/modules/consumer-echo/: in the prefix of a Praenora installation,
# that is the installation's module directory, where the runtime finds modules by itself.
include(GNUInstallDirs)
praenora_add_module(consumer_echo
    NAME consumer-echo
    VERSION 0.1.0
    CAPABILITIES test.echo@0.1.0
    SOURCES consumer_echo.cpp
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/praenora/modules/consumer-echo")
