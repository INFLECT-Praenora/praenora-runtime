# Install test (label install). CTest test install.consumer runs run_install_test.cmake, which
# installs this build into a new prefix below this directory's build tree and uses it from the
# outside: three projects that are not part of this build are configured and built against the
# installation with find_package(Praenora), consumer-module (COMPONENTS ModuleSDK: a module built
# with praenora_add_module()), consumer-python-module (COMPONENTS ModuleSDK: a Python module built
# with praenora_add_python_module(), without Python) and consumer-app (COMPONENTS Runtime: an
# application that loads the first one, and runs a workflow), and the installed files (the
# service providers, the Python environment and the license files included) and the installed
# praenora tool are checked. The driver lists every step. The consumers get this build's
# generator, compilers, build type, macOS settings, dependency prefixes (CMAKE_PREFIX_PATH) and
# compile and link flags, sanitizer options included, since they link the installed (in sanitizer
# builds, instrumented) runtime library. install.package, below, checks the archive of an
# installation that the target package makes. With PRAENORA_WITH_PYTHON_ENVIRONMENT an
# installation holds the environment, about a gigabyte, so both take longer.

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
# With the Python environment, the provider python, the environment the build uses, which the
# installation holds, and the Python fixture modules, which the installed tool runs. The
# environment, about a gigabyte, is copied, compared and archived: the tests get more time.
set(_praenora_python_environment "")
set(_praenora_python_modules "")
set(_praenora_install_timeout 600)
if(PRAENORA_WITH_PYTHON_ENVIRONMENT)
    list(APPEND _praenora_installed_providers python)
    set(_praenora_python_environment "${PRAENORA_PYTHON_ENVIRONMENT_DIR}")
    set(_praenora_python_modules "${PRAENORA_TEST_PYTHON_MODULES_DIR}")
    set(_praenora_install_timeout 1800)
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
        # The license files that a default installation holds besides this project's LICENSE
        # (cmake/PraenoraInstall.cmake): those of the dependencies built in this tree, and the
        # contents of the directories that PRAENORA_THIRD_PARTY_LICENSE_DIRS names. Each list
        # reaches the driver as one quoted argument, as the providers' list does.
        "-DLICENSE_DEPENDENCIES=${PRAENORA_FETCHED_DEPENDENCIES}"
        "-DLICENSE_DIRS=${PRAENORA_THIRD_PARTY_LICENSE_DIRS}"
        "-DPYTHON_ENVIRONMENT=${_praenora_python_environment}"
        "-DPYTHON_MODULES_DIR=${_praenora_python_modules}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_install_test.cmake")
# _praenora_test_environment (tests/CMakeLists.txt): what every test of a macOS
# AddressSanitizer build needs. Each installation of this build writes install_manifest*.txt into
# the build tree, so install.consumer and install.package never run at the same time.
set_tests_properties(install.consumer PROPERTIES
    LABELS install
    TIMEOUT ${_praenora_install_timeout}
    RESOURCE_LOCK praenora_install
    ${_praenora_test_environment}
    ${_praenora_no_telemetry})

# The archive of an installation (the target package, cmake/PraenoraPackage.cmake, which only the
# top-level project has): install.package makes one with cpack and checks its name, its .sha256
# file and its files against an installation, and that there is no source package
# (check_package.cmake). It runs no installed program. Its scratch directory, pkg/ at the top of
# the build tree, is named so that cpack's staging paths there are no longer than those of the
# target package in package/ (Windows limits a path to 260 characters).
if(PRAENORA_TOP_LEVEL)
    add_test(NAME install.package
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DWORK_DIR=${PROJECT_BINARY_DIR}/pkg"
            "-DCPACK=${CMAKE_CPACK_COMMAND}"
            "-DCONFIG=$<CONFIG>"
            "-DVERSION=${PROJECT_VERSION}"
            "-DLIBDIR=${CMAKE_INSTALL_LIBDIR}"
            "-DDATADIR=${CMAKE_INSTALL_DATADIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_package.cmake")
    set_tests_properties(install.package PROPERTIES
        LABELS install
        TIMEOUT ${_praenora_install_timeout}
        RESOURCE_LOCK praenora_install
        ${_praenora_no_telemetry})
endif()
