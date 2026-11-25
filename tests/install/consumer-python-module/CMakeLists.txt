# A Python module project outside praenora-runtime, as a module team writes one: it needs the
# installed module SDK, CMake and a C++ compiler, and neither Python nor the runtime's Python
# environment. The install test configures and builds it against an installation, which need not
# hold the environment, and runs the module with the installed tool when it does
# (tests/install/run_install_test.cmake).
cmake_minimum_required(VERSION 3.24)
project(praenora_consumer_python_module VERSION 0.1.0 LANGUAGES CXX)

find_package(Praenora 0.2 REQUIRED COMPONENTS ModuleSDK)
# The ModuleSDK component loads only the SDK, praenora_add_python_module() included: it looks for
# no other package, no Python and no Python environment among them.
get_property(found_packages GLOBAL PROPERTY PACKAGES_FOUND)
get_property(missing_packages GLOBAL PROPERTY PACKAGES_NOT_FOUND)
if(NOT found_packages STREQUAL "Praenora" OR missing_packages)
    message(FATAL_ERROR "find_package(Praenora COMPONENTS ModuleSDK) looked for other packages; "
        "found: ${found_packages}; not found: ${missing_packages}")
endif()
if(NOT COMMAND praenora_add_python_module)
    message(FATAL_ERROR "find_package(Praenora COMPONENTS ModuleSDK) defined no "
        "praenora_add_python_module()")
endif()

# The library, its manifest and its Python sources (consumer-python-echo.python/) are built into
# <build>/consumer-python-echo/ (with a multi-config generator, <build>/consumer-python-echo/
# <config>/), and installed together into <prefix>/<libdir>/praenora/modules/consumer-python-echo/.
include(GNUInstallDirs)
praenora_add_python_module(consumer_python_echo
    NAME consumer-python-echo
    VERSION 0.1.0
    CAPABILITIES test.echo@0.1.0
    CLASS consumer_python_echo:Echo
    PYTHON_SOURCES consumer_python_echo
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/praenora/modules/consumer-python-echo")
