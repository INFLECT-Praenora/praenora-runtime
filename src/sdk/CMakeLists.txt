# The module SDK: the C ABI headers (abi.h, status.h, the generated version.h), the optional
# header-only C++ adapter (module.hpp) and the in-process test kit (testing.hpp). Module projects
# link Praenora::ModuleSDK and need only a C/C++ compiler.
add_library(praenora_module_sdk INTERFACE)
add_library(Praenora::ModuleSDK ALIAS praenora_module_sdk)
set_target_properties(praenora_module_sdk PROPERTIES EXPORT_NAME ModuleSDK)
target_include_directories(praenora_module_sdk INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# The C headers are C99; the adapter and the test kit need C++17.
target_compile_features(praenora_module_sdk INTERFACE c_std_99 cxx_std_17)

# Generated include/praenora/sdk/version.h, from the versions in cmake/PraenoraVersions.cmake.
if(NOT PRAENORA_SDK_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PRAENORA_SDK_VERSION '${PRAENORA_SDK_VERSION}' is not MAJOR.MINOR.PATCH")
endif()
set(PRAENORA_SDK_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PRAENORA_SDK_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PRAENORA_SDK_VERSION_PATCH "${CMAKE_MATCH_3}")
configure_file("${PROJECT_SOURCE_DIR}/include/praenora/sdk/version.h.in"
    "${PROJECT_BINARY_DIR}/include/praenora/sdk/version.h" @ONLY)

# Installed: the target (export set PraenoraModuleSDKTargets, cmake/PraenoraInstall.cmake) and
# the headers, the generated version.h included.
if(PRAENORA_INSTALL_RULES)
    install(TARGETS praenora_module_sdk EXPORT PraenoraModuleSDKTargets)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/praenora/sdk"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/praenora"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    install(FILES "${PROJECT_BINARY_DIR}/include/praenora/sdk/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/praenora/sdk")
endif()
