# Install test (label install). CTest test install.consumer runs run_install_test.cmake, which
# installs this build into a new prefix below this directory's build tree and uses it from the
# outside: two projects that are not part of this build are configured and built against the
# installation with find_package(Praenora), consumer-module (COMPONENTS ModuleSDK: a module built
# with praenora_add_module()) and consumer-app (COMPONENTS Runtime: an application that loads
# that module), and the installed files (the service providers included) and the installed
# praenora tool are checked. The driver lists every step. The consumers get this build's
# generator, compilers, build type, macOS settings, dependency prefixes (CMAKE_PREFIX_PATH) and
# compile and link flags, sanitizer options included, since they link the installed (in sanitizer
# builds, instrumented) runtime library.

get_property(_praenora_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
# The consumers' flags: this build's CMAKE_<LANG>_FLAGS and CMAKE_<KIND>_LINKER_FLAGS, plus its
# sanitizer options (cmake/PraenoraSanitizers.cmake), which reach this build's own targets as
# directory options instead.
list(JOIN PRAENORA_SANITIZER_COMPILE_OPTIONS " " _praenora_sanitizer_compile)
list(JOIN PRAENORA_SANITIZER_LINK_OPTIONS " " _praenora_sanitizer_link)
foreach(_praenora_language IN ITEMS C CXX)
    string(STRIP "${CMAKE_${_praenora_language}_FLAGS} ${_praenora_sanitizer_compile}"
        _praenora_${_praenora_language}_flags)
endforeach()
foreach(_praenora_kind IN ITEMS EXE SHARED MODULE)
    string(STRIP "${CMAKE_${_praenora_kind}_LINKER_FLAGS} ${_praenora_sanitizer_link}"
        _praenora_${_praenora_kind}_linker_flags)
endforeach()
# The driver's variables that come from this build's settings (empty when a setting is not
# defined). Each reaches the driver as one argument: list separators are escaped.
set(_praenora_install_settings "")
foreach(_praenora_pair IN ITEMS PREFIX_PATH:CMAKE_PREFIX_PATH
                                OSX_ARCHITECTURES:CMAKE_OSX_ARCHITECTURES
                                C_FLAGS:_praenora_C_flags CXX_FLAGS:_praenora_CXX_flags
                                EXE_LINKER_FLAGS:_praenora_EXE_linker_flags
                                SHARED_LINKER_FLAGS:_praenora_SHARED_linker_flags
                                MODULE_LINKER_FLAGS:_praenora_MODULE_linker_flags)
    string(REPLACE ":" ";" _praenora_pair "${_praenora_pair}")
    list(GET _praenora_pair 0 _praenora_name)
    list(GET _praenora_pair 1 _praenora_setting)
    set(_praenora_value "")
    if(DEFINED ${_praenora_setting})
        string(REPLACE ";" "\\;" _praenora_value "${${_praenora_setting}}")
    endif()
    list(APPEND _praenora_install_settings "-D${_praenora_name}=${_praenora_value}")
endforeach()

# The service providers that a default installation includes. The list reaches the driver as one
# argument because that argument is quoted; escaping its separators would keep the backslashes, and
# the driver would read one name.
set(_praenora_installed_providers "")
if(PRAENORA_WITH_ROADRUNNER)
    list(APPEND _praenora_installed_providers roadrunner)
endif()
if(PRAENORA_WITH_ANTIMONY)
    list(APPEND _praenora_installed_providers antimony)
endif()

add_test(NAME install.consumer
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/work"
        "-DTEST_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCATALOG_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/catalog"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DMULTI_CONFIG=${_praenora_multi_config}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DC_COMPILER=${CMAKE_C_COMPILER}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DBUILD_TYPE=$<CONFIG>"
        "-DOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        ${_praenora_install_settings}
        "-DBINDIR=${CMAKE_INSTALL_BINDIR}"
        "-DLIBDIR=${CMAKE_INSTALL_LIBDIR}"
        "-DINCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
        "-DDATADIR=${CMAKE_INSTALL_DATADIR}"
        "-DWITH_CLI=${PRAENORA_BUILD_CLI}"
        "-DWITH_EXAMPLES=${PRAENORA_BUILD_EXAMPLES}"
        "-DINSTALL_EXAMPLES=${PRAENORA_INSTALL_EXAMPLES}"
        "-DPROVIDERS=${_praenora_installed_providers}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_install_test.cmake")
# _praenora_test_environment (tests/CMakeLists.txt): what every test of a macOS
# AddressSanitizer build needs.
set_tests_properties(install.consumer PROPERTIES
    LABELS install
    TIMEOUT 600
    ${_praenora_test_environment})
