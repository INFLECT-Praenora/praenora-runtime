cmake_minimum_required(VERSION 3.24)

# The macOS deployment target must be known before project() runs the compiler checks. It is
# ignored on other platforms.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version targeted by the build")

project(praenora_runtime VERSION 0.2.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(PraenoraOptions)
include(PraenoraVersions)
include(PraenoraCompilerChecks)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(PRAENORA_TOP_LEVEL)
    # Executables and DLLs share one directory, so tests and tools find the runtime library.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
endif()

# Modules built in this tree: test fixtures below PRAENORA_TEST_MODULES_DIR and examples below
# PRAENORA_EXAMPLE_MODULES_DIR, one directory per module holding the library and its manifest.
# Fixture service providers go below PRAENORA_TEST_PROVIDERS_DIR the same way. Multi-config
# generators put the configuration first (test-modules/<config>/<name>), so one directory per
# configuration holds all of its modules.
get_property(_praenora_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(PRAENORA_TEST_MODULES_DIR "${PROJECT_BINARY_DIR}/test-modules")
set(PRAENORA_EXAMPLE_MODULES_DIR "${PROJECT_BINARY_DIR}/modules")
set(PRAENORA_TEST_PROVIDERS_DIR "${PROJECT_BINARY_DIR}/test-providers")
if(_praenora_multi_config)
    string(APPEND PRAENORA_TEST_MODULES_DIR "/$<CONFIG>")
    string(APPEND PRAENORA_EXAMPLE_MODULES_DIR "/$<CONFIG>")
    string(APPEND PRAENORA_TEST_PROVIDERS_DIR "/$<CONFIG>")
endif()

include(GNUInstallDirs)
include(PraenoraWarnings)
include(PraenoraSanitizers)
include(PraenoraModule)
include(PraenoraProvider)
include(PraenoraEmbedResources)
include(PraenoraDependencies)
praenora_find_dependencies()
if(PRAENORA_WITH_ROADRUNNER)
    include(PraenoraRoadrunner)
    praenora_find_roadrunner()
endif()
if(PRAENORA_WITH_ANTIMONY)
    include(PraenoraAntimony)
    praenora_find_antimony()
endif()

add_subdirectory(src/sdk)
add_subdirectory(src/runtime)
# The module host, in which a step's module instance runs in a process of its own: POSIX only.
if(NOT WIN32)
    add_subdirectory(src/module_host)
endif()
# Service providers shipped with the runtime: the libRoadRunner provider, built only with
# PRAENORA_WITH_ROADRUNNER, and the libAntimony provider, built only with PRAENORA_WITH_ANTIMONY.
if(PRAENORA_WITH_ROADRUNNER)
    add_subdirectory(src/providers/roadrunner)
else()
    # A tree configured with the provider before must not keep finding it.
    praenora_remove_build_tree_provider(roadrunner)
endif()
if(PRAENORA_WITH_ANTIMONY)
    add_subdirectory(src/providers/antimony)
else()
    praenora_remove_build_tree_provider(antimony)
endif()
# What the two front ends below share (header-only, internal).
if(PRAENORA_BUILD_CLI OR PRAENORA_BUILD_PYTHON)
    add_subdirectory(src/frontend)
endif()
if(PRAENORA_BUILD_CLI)
    # The control API, a third front end (static, internal), for the praenora tool.
    add_subdirectory(src/server)
    add_subdirectory(src/cli)
endif()
if(PRAENORA_BUILD_PYTHON)
    add_subdirectory(src/python)
endif()
if(PRAENORA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(PRAENORA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The package's install rules (each directory above installs its own targets and files) and, at
# top level, the target package, which archives an installation (cmake/PraenoraPackage.cmake).
if(PRAENORA_INSTALL_RULES)
    include(PraenoraInstall)
    if(PRAENORA_TOP_LEVEL)
        include(PraenoraPackage)
    endif()
endif()
