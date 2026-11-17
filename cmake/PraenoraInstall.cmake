# The install rules of the package that belong to no single directory: the CMake package
# configuration that find_package(Praenora) reads, the export sets of Praenora::Runtime and
# Praenora::ModuleSDK, the module helper PraenoraModule.cmake, and the schemas and the catalog.
# The top-level CMakeLists.txt includes this file after every target is defined, when
# PRAENORA_INSTALL_RULES is ON (never in a wheel build; cmake/PraenoraOptions.cmake). The targets,
# the headers and the example modules are installed by their own directories (src/runtime,
# src/sdk, src/cli, examples).
#
# The installation, relative to its prefix (GNUInstallDirs directories; on Windows the DLLs go to
# the bin directory and the import library to the lib directory):
#   ${CMAKE_INSTALL_BINDIR}/      praenora (Windows: and the DLLs it loads)
#   ${CMAKE_INSTALL_LIBDIR}/      the runtime library (soname links on Linux, install name
#                                 @rpath/libpraenora_runtime.<major>.<minor>.dylib on macOS)
#   ${CMAKE_INSTALL_INCLUDEDIR}/praenora/{runtime,sdk}/   the public headers, generated ones
#                                 (export.h, version.h) included
#   ${CMAKE_INSTALL_LIBDIR}/cmake/Praenora/   PraenoraConfig.cmake, PraenoraConfigVersion.cmake,
#                                 PraenoraRuntimeTargets*.cmake, PraenoraModuleSDKTargets*.cmake
#                                 and PraenoraModule.cmake
#   ${CMAKE_INSTALL_DATADIR}/praenora/{schemas,catalog}/   the documents embedded in the runtime
#   ${CMAKE_INSTALL_LIBDIR}/praenora/modules/<name>/   the example modules: install component
#                                 Examples, part of a default installation only with
#                                 PRAENORA_INSTALL_EXAMPLES; that is the installation's module
#                                 directory, the last place the runtime looks for modules
# Every installed CMake file names locations relative to the installation, so it can be moved.

include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

set(_praenora_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Praenora")

install(EXPORT PraenoraRuntimeTargets
    NAMESPACE Praenora::
    DESTINATION "${_praenora_package_dir}")
install(EXPORT PraenoraModuleSDKTargets
    NAMESPACE Praenora::
    DESTINATION "${_praenora_package_dir}")

# No check_required_components(): PraenoraConfig.cmake.in says why it sets Praenora_FOUND itself.
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/PraenoraConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/PraenoraConfig.cmake"
    INSTALL_DESTINATION "${_praenora_package_dir}"
    NO_CHECK_REQUIRED_COMPONENTS_MACRO)
# While the version is 0.y, a change of y may break compatibility.
write_basic_package_version_file("${PROJECT_BINARY_DIR}/cmake/PraenoraConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${PROJECT_BINARY_DIR}/cmake/PraenoraConfig.cmake"
    "${PROJECT_BINARY_DIR}/cmake/PraenoraConfigVersion.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/PraenoraModule.cmake"
    DESTINATION "${_praenora_package_dir}")

# The schemas and the catalog, as embedded in the runtime (cmake/PraenoraEmbedResources.cmake).
foreach(_praenora_documents IN ITEMS schemas catalog)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/spec/${_praenora_documents}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/praenora/${_praenora_documents}"
        FILES_MATCHING PATTERN "*.json")
endforeach()
