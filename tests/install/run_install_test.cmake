# Installs a build of praenora-runtime and uses the installation from the outside: the driver of
# the install test (tests/install/CMakeLists.txt). Script mode, cmake -P, with these variables
# (lists are ;-lists; the settings may be empty):
#
#   BUILD_DIR, SOURCE_DIR       the build tree to install and its source tree
#   WORK_DIR                    scratch directory, emptied first: the installation goes to
#                               <WORK_DIR>/prefix, the consumers build in <WORK_DIR>/<consumer>
#   TEST_DIR                    tests/install, which holds the consumer projects
#   CATALOG_DIR                 the test catalog, which defines test.echo for consumer-echo
#   GENERATOR, GENERATOR_PLATFORM, GENERATOR_TOOLSET, MULTI_CONFIG, MAKE_PROGRAM
#                               the build's generator; MULTI_CONFIG is true for a multi-config one
#   C_COMPILER, CXX_COMPILER    the build's compilers
#   BUILD_TYPE                  the configuration under test
#   PREFIX_PATH                 the build's dependency prefixes (its CMAKE_PREFIX_PATH)
#   C_FLAGS, CXX_FLAGS, EXE_LINKER_FLAGS, SHARED_LINKER_FLAGS, MODULE_LINKER_FLAGS
#                               the consumers' CMAKE_<LANG>_FLAGS and CMAKE_<KIND>_LINKER_FLAGS:
#                               the build's, with its sanitizer options
#   OSX_ARCHITECTURES, OSX_DEPLOYMENT_TARGET   the build's macOS settings
#   BINDIR, LIBDIR, INCLUDEDIR, DATADIR        the build's GNUInstallDirs directories
#   WITH_CLI, WITH_EXAMPLES     the build has the praenora tool, the example modules
#   INSTALL_EXAMPLES            a default installation includes the example modules
#
# Steps, in order; the first failure ends the test, showing what ran and what it printed:
#   1. cmake --install <BUILD_DIR> --prefix <WORK_DIR>/prefix. Each kind of installed file must
#      be there, and nothing else of its kind: the tool, the runtime library, the headers (the
#      generated ones included; no template and no internal header), the package configuration,
#      the schemas and the catalog. No file of the Python package is installed, and no example
#      module unless INSTALL_EXAMPLES. With example modules built, cmake --install ...
#      --component Examples then installs them, with their manifests.
#   2. consumer-module and consumer-app are configured, with uninitialized variables reported
#      (one read in the installed PraenoraConfig.cmake or PraenoraModule.cmake fails; each
#      configuration's output is kept in <WORK_DIR>/<consumer>-configure.log), and built, each
#      against <prefix>;<PREFIX_PATH>; each must have found Praenora in <prefix>. A project that
#      requires an unknown component must fail to configure, with a reason that names it.
#      consumer-module then installs its module into <WORK_DIR>/module-prefix through the
#      installed praenora_add_module() (INSTALL_DESTINATION): manifest and library side by side.
#   3. consumer-app runs with the installed module's directory and CATALOG_DIR: exit 0, and
#      consumer-echo's describe document on standard output.
#   4. No file below <prefix>/<LIBDIR>/cmake/Praenora and no installed *.pmm.json (the
#      installation's and consumer-module's) mentions SOURCE_DIR or BUILD_DIR.
#   5. <prefix>/<BINDIR>/praenora version --json: exit 0 and a successful JSON document.
#   6. With example modules: <prefix>/<BINDIR>/praenora modules list --json, without
#      --module-path, lists them from the installation's module directory, and
#      praenora modules check passes for each installed manifest (the module loads from the
#      installation and passes its contract checks with its installed test vectors).
#   7. The installation is moved to another directory, whose name holds a space and a non-ASCII
#      character; from there, steps 5 and 6 succeed again, with the modules found in their new
#      place.
# Without the tool or the example modules, the steps that need them are skipped, with a note.
# Every command runs without the PRAENORA_* variables that the runtime reads (except
# PRAENORA_DEBUG_KEEP_LIBRARIES), and a sanitizer report in its output fails the test.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS BUILD_DIR SOURCE_DIR WORK_DIR TEST_DIR CATALOG_DIR GENERATOR BINDIR
                          LIBDIR INCLUDEDIR DATADIR)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "run_install_test.cmake: ${variable} is required")
    endif()
endforeach()
foreach(variable IN ITEMS GENERATOR_PLATFORM GENERATOR_TOOLSET MULTI_CONFIG MAKE_PROGRAM C_COMPILER
                          CXX_COMPILER BUILD_TYPE PREFIX_PATH C_FLAGS CXX_FLAGS EXE_LINKER_FLAGS
                          SHARED_LINKER_FLAGS MODULE_LINKER_FLAGS OSX_ARCHITECTURES
                          OSX_DEPLOYMENT_TARGET WITH_CLI WITH_EXAMPLES INSTALL_EXAMPLES)
    if(NOT DEFINED ${variable})
        set(${variable} "")
    endif()
endforeach()
foreach(variable IN ITEMS PRAENORA_MODULE_PATH PRAENORA_CATALOG_PATH PRAENORA_LOG_LEVEL
                          PRAENORA_LOG_FORMAT)
    unset(ENV{${variable}})
endforeach()

set(exe "")
if(CMAKE_HOST_WIN32)
    set(exe ".exe")
endif()
set(prefix "${WORK_DIR}/prefix")
set(example_modules example-estimator example-predictor)

# _run(<prefix> <what> <command>...): praenora_test_run() (tests/support/run_command.cmake), and
# the command must exit with 0; its outputs go to <prefix>_OUT and <prefix>_ERR.
function(_run prefix what)
    praenora_test_run(command "${what}" ${ARGN})
    if(NOT command_EXIT STREQUAL "0")
        praenora_test_fail("${what}" "exit code ${command_EXIT}, expected 0" "${ARGN}"
            "${command_EXIT}" "${command_OUT}" "${command_ERR}")
    endif()
    set(${prefix}_OUT "${command_OUT}" PARENT_SCOPE)
    set(${prefix}_ERR "${command_ERR}" PARENT_SCOPE)
endfunction()

# The files below `dir`, relative to it and sorted, in `out`.
function(_files_below dir out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(SORT files)
    set(${out} "${files}" PARENT_SCOPE)
endfunction()

# Fails unless the files below `installed` are exactly `expected` (relative paths).
function(_expect_files what installed expected)
    _files_below("${installed}" found)
    list(SORT expected)
    if(NOT found STREQUAL expected)
        string(REPLACE ";" "\n  " found "${found}")
        string(REPLACE ";" "\n  " expected "${expected}")
        message(FATAL_ERROR "step 1: the installed ${what} in ${installed} differ from the "
            "expected ones.\nexpected:\n  ${expected}\ninstalled:\n  ${found}\n")
    endif()
endfunction()

# Steps 5 and 6 for the installation in `root`; `step` names them in messages.
function(_check_tool root step)
    set(tool "${root}/${BINDIR}/praenora${exe}")
    _run(VERSION "${step}: praenora version" "${tool}" version --json)
    string(STRIP "${VERSION_OUT}" VERSION_OUT)
    string(JSON ok ERROR_VARIABLE json_error GET "${VERSION_OUT}" ok)
    if(json_error OR NOT ok)
        message(FATAL_ERROR "${step}: '${tool} version --json' did not print a successful JSON "
            "document:\n${VERSION_OUT}")
    endif()
    message(STATUS "${step}: ${tool} version --json:\n${VERSION_OUT}")

    if(NOT WITH_EXAMPLES)
        message(STATUS "${step}: modules list skipped (the build has no example modules)")
        return()
    endif()
    _run(LIST "${step}: praenora modules list" "${tool}" modules list --json)
    string(JSON count ERROR_VARIABLE json_error LENGTH "${LIST_OUT}" result)
    if(json_error)
        message(FATAL_ERROR "${step}: 'praenora modules list --json' did not print a list of "
            "modules:\n${LIST_OUT}")
    endif()
    file(REAL_PATH "${root}/${LIBDIR}/praenora/modules" modules_dir)
    set(listed "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON name GET "${LIST_OUT}" result ${index} name)
            string(JSON manifest GET "${LIST_OUT}" result ${index} manifest_path)
            file(TO_CMAKE_PATH "${manifest}" manifest)
            file(REAL_PATH "${manifest}" manifest)
            string(FIND "${manifest}" "${modules_dir}/" position)
            if(NOT position EQUAL 0)
                message(FATAL_ERROR "${step}: module ${name} was found at ${manifest}, outside "
                    "the installation's module directory ${modules_dir}")
            endif()
            list(APPEND listed "${name}")
        endforeach()
    endif()
    foreach(name IN LISTS example_modules)
        if(NOT name IN_LIST listed)
            message(FATAL_ERROR "${step}: the installed praenora tool does not list module "
                "${name} of the installation's module directory ${modules_dir}:\n${LIST_OUT}")
        endif()
    endforeach()
    list(JOIN listed ", " listed)
    message(STATUS "${step}: the installed tool lists ${listed} from ${modules_dir}")
    # Each installed example module loads, and passes its contract checks with its installed test
    # vectors.
    foreach(name IN LISTS example_modules)
        set(manifest "${modules_dir}/${name}/${name}.pmm.json")
        _run(CHECK "${step}: praenora modules check (${name})"
            "${tool}" modules check "${manifest}" --json)
        string(JSON passed ERROR_VARIABLE json_error GET "${CHECK_OUT}" result passed)
        if(json_error OR NOT passed)
            message(FATAL_ERROR "${step}: the contract checks of the installed module ${name} "
                "did not pass:\n${CHECK_OUT}")
        endif()
    endforeach()
    message(STATUS "${step}: the contract checks of ${listed} pass from the installation")
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(config_args "")
if(NOT BUILD_TYPE STREQUAL "")
    set(config_args --config "${BUILD_TYPE}")
endif()

# 1. The installation.
_run(INSTALL "step 1: cmake --install"
    "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" ${config_args})
file(GLOB_RECURSE expected_headers LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/include"
    "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/include/*.hpp")
list(APPEND expected_headers praenora/runtime/export.h praenora/sdk/version.h)
_expect_files(headers "${prefix}/${INCLUDEDIR}" "${expected_headers}")
foreach(kind IN ITEMS schemas catalog)
    file(GLOB_RECURSE expected LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/spec/${kind}"
        "${SOURCE_DIR}/spec/${kind}/*.json")
    _expect_files(${kind} "${prefix}/${DATADIR}/praenora/${kind}" "${expected}")
endforeach()
set(package_dir "${prefix}/${LIBDIR}/cmake/Praenora")
foreach(file IN ITEMS PraenoraConfig.cmake PraenoraConfigVersion.cmake PraenoraModule.cmake
                      PraenoraRuntimeTargets.cmake PraenoraModuleSDKTargets.cmake)
    if(NOT EXISTS "${package_dir}/${file}")
        message(FATAL_ERROR "step 1: ${package_dir}/${file} was not installed")
    endif()
endforeach()
file(GLOB runtime_library "${prefix}/${LIBDIR}/*praenora_runtime*")
if(NOT runtime_library)
    message(FATAL_ERROR "step 1: the runtime library was not installed in ${prefix}/${LIBDIR}")
endif()
if(WITH_CLI AND NOT EXISTS "${prefix}/${BINDIR}/praenora${exe}")
    message(FATAL_ERROR "step 1: the praenora tool was not installed in ${prefix}/${BINDIR}")
endif()
file(GLOB_RECURSE python_files LIST_DIRECTORIES false "${prefix}/*.py" "${prefix}/*_bindings*")
if(python_files)
    message(FATAL_ERROR "step 1: files of the Python package were installed: ${python_files}")
endif()
set(modules_dir "${prefix}/${LIBDIR}/praenora/modules")
# Fails unless every example module's manifest is installed; `how` names the installation.
function(_expect_example_modules how)
    foreach(name IN LISTS example_modules)
        if(NOT EXISTS "${modules_dir}/${name}/${name}.pmm.json")
            message(FATAL_ERROR "step 1: ${how} did not install module ${name} in ${modules_dir}")
        endif()
    endforeach()
endfunction()
if(NOT INSTALL_EXAMPLES AND EXISTS "${modules_dir}")
    _files_below("${modules_dir}" installed_modules)
    message(FATAL_ERROR "step 1: a default installation installed example modules, although "
        "PRAENORA_INSTALL_EXAMPLES is OFF: ${installed_modules}")
endif()
if(WITH_EXAMPLES)
    if(INSTALL_EXAMPLES)
        _expect_example_modules("a default installation (PRAENORA_INSTALL_EXAMPLES is ON)")
    endif()
    _run(EXAMPLES "step 1: cmake --install --component Examples"
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" ${config_args}
        --component Examples)
    _expect_example_modules("cmake --install --component Examples")
else()
    message(STATUS "step 1: the build has no example modules; the steps with them are skipped")
endif()
_files_below("${prefix}" installed_files)
list(LENGTH installed_files installed_count)
message(STATUS "step 1: ${installed_count} files installed in ${prefix}")

# 2. The consumers, configured from an initial cache with this build's settings.
set(settings "")
function(_setting name type value)
    set(settings "${settings}set(${name} [==[${value}]==] CACHE ${type} \"\")\n" PARENT_SCOPE)
endfunction()
set(prefix_path "${prefix}")
if(NOT PREFIX_PATH STREQUAL "")
    list(APPEND prefix_path ${PREFIX_PATH})
endif()
_setting(CMAKE_PREFIX_PATH STRING "${prefix_path}")
if(NOT GENERATOR MATCHES "^Visual Studio")
    foreach(language IN ITEMS C CXX)
        if(NOT ${language}_COMPILER STREQUAL "")
            _setting(CMAKE_${language}_COMPILER FILEPATH "${${language}_COMPILER}")
        endif()
    endforeach()
endif()
if(NOT MAKE_PROGRAM STREQUAL "")
    _setting(CMAKE_MAKE_PROGRAM FILEPATH "${MAKE_PROGRAM}")
endif()
if(NOT MULTI_CONFIG AND NOT BUILD_TYPE STREQUAL "")
    _setting(CMAKE_BUILD_TYPE STRING "${BUILD_TYPE}")
endif()
if(CMAKE_HOST_APPLE)
    if(NOT OSX_ARCHITECTURES STREQUAL "")
        _setting(CMAKE_OSX_ARCHITECTURES STRING "${OSX_ARCHITECTURES}")
    endif()
    if(NOT OSX_DEPLOYMENT_TARGET STREQUAL "")
        _setting(CMAKE_OSX_DEPLOYMENT_TARGET STRING "${OSX_DEPLOYMENT_TARGET}")
    endif()
endif()
# The build's flags, empty ones included: then the consumers get exactly them, whatever CFLAGS,
# CXXFLAGS or LDFLAGS the test's environment holds.
foreach(language IN ITEMS C CXX)
    _setting(CMAKE_${language}_FLAGS STRING "${${language}_FLAGS}")
endforeach()
foreach(kind IN ITEMS EXE SHARED MODULE)
    _setting(CMAKE_${kind}_LINKER_FLAGS STRING "${${kind}_LINKER_FLAGS}")
endforeach()
set(settings_file "${WORK_DIR}/consumer-settings.cmake")
file(WRITE "${settings_file}" "${settings}")
set(generator_args -G "${GENERATOR}")
if(NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_args -A "${GENERATOR_PLATFORM}")
endif()
if(NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_args -T "${GENERATOR_TOOLSET}")
endif()
# Uninitialized variables are reported for files outside the consumer's own trees, such as the
# installed ones, only with --check-system-vars (CMake's own modules then report many, which are
# not this test's business). CMake 4.4 renamed --warn-uninitialized to -Wuninitialized.
set(uninitialized_args -Wuninitialized --check-system-vars)
if(CMAKE_VERSION VERSION_LESS 4.4)
    set(uninitialized_args --warn-uninitialized --check-system-vars)
endif()
foreach(consumer IN ITEMS consumer-module consumer-app)
    set(consumer_dir "${WORK_DIR}/${consumer}")
    _run(CONFIGURE "step 2: configuring ${consumer}"
        "${CMAKE_COMMAND}" -S "${TEST_DIR}/${consumer}" -B "${consumer_dir}" -C "${settings_file}"
        ${generator_args} ${uninitialized_args})
    set(configure_log "${WORK_DIR}/${consumer}-configure.log")
    file(WRITE "${configure_log}" "${CONFIGURE_OUT}${CONFIGURE_ERR}")
    # The package files that this project writes; CMake generates the others. A warning names its
    # file as "at <file>:<line> (<command>):" or, for a read in an elseif() condition for example,
    # "in <file>:".
    string(REGEX MATCHALL "CMake Warning \\([a-z]+\\) (at|in) [^\n]*\n +uninitialized variable"
        uninitialized "${CONFIGURE_OUT}${CONFIGURE_ERR}")
    foreach(warning IN LISTS uninitialized)
        foreach(file IN ITEMS PraenoraConfig.cmake PraenoraModule.cmake)
            string(FIND "${warning}" "${package_dir}/${file}:" position)
            if(NOT position EQUAL -1)
                message(FATAL_ERROR "step 2: configuring ${consumer} reads an uninitialized "
                    "variable in the installed ${file}:\n${warning}\n(the whole output is in "
                    "${configure_log})")
            endif()
        endforeach()
    endforeach()
    file(STRINGS "${consumer_dir}/CMakeCache.txt" found_dir REGEX "^Praenora_DIR:")
    string(REGEX REPLACE "^Praenora_DIR:[A-Z]+=" "" found_dir "${found_dir}")
    if(NOT found_dir STREQUAL package_dir)
        message(FATAL_ERROR "step 2: ${consumer} found Praenora in '${found_dir}', not in the "
            "installation's ${package_dir}")
    endif()
    _run(BUILD "step 2: building ${consumer}"
        "${CMAKE_COMMAND}" --build "${consumer_dir}" ${config_args})
    message(STATUS "step 2: ${consumer} built against ${package_dir}")
endforeach()
# A project that requires an unknown component fails to configure, and the reason the package
# gives names that component and the package's components.
set(unknown_dir "${WORK_DIR}/unknown-component")
file(WRITE "${unknown_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
    "project(praenora_unknown_component LANGUAGES NONE)\n"
    "find_package(Praenora 0.1 REQUIRED COMPONENTS Runtime Frobnicator)\n")
praenora_test_run(UNKNOWN "step 2: an unknown component"
    "${CMAKE_COMMAND}" -S "${unknown_dir}" -B "${unknown_dir}/build" -C "${settings_file}"
    ${generator_args})
# CMake wraps the reason's lines.
string(REGEX REPLACE "[ \t\r\n]+" " " unknown_output "${UNKNOWN_OUT}${UNKNOWN_ERR}")
string(FIND "${unknown_output}" "Reason given by package: " reason_position)
if(reason_position EQUAL -1)
    set(unknown_reason "")
else()
    string(SUBSTRING "${unknown_output}" ${reason_position} -1 unknown_reason)
endif()
if(UNKNOWN_EXIT STREQUAL "0" OR NOT unknown_reason MATCHES "Frobnicator"
   OR NOT unknown_reason MATCHES "Runtime" OR NOT unknown_reason MATCHES "ModuleSDK")
    set(problem "it did not fail with a reason that names the component Frobnicator and the ")
    string(APPEND problem "components Runtime and ModuleSDK")
    praenora_test_fail("step 2: requiring an unknown component" "${problem}"
        "${CMAKE_COMMAND};-S;${unknown_dir}" "${UNKNOWN_EXIT}" "${UNKNOWN_OUT}" "${UNKNOWN_ERR}")
endif()
message(STATUS "step 2: an unknown component is not found, and the reason names it")
# consumer-module installs its module with the installed praenora_add_module()
# (INSTALL_DESTINATION), into a prefix of its own: its manifest and library, side by side.
set(module_prefix "${WORK_DIR}/module-prefix")
_run(MODULE_INSTALL "step 2: cmake --install consumer-module"
    "${CMAKE_COMMAND}" --install "${WORK_DIR}/consumer-module" --prefix "${module_prefix}"
    ${config_args})
file(GLOB_RECURSE module_manifest LIST_DIRECTORIES false "${module_prefix}/*.pmm.json")
list(LENGTH module_manifest module_manifest_count)
if(NOT module_manifest_count EQUAL 1)
    message(FATAL_ERROR "step 2: consumer-module installed ${module_manifest_count} manifests, "
        "expected 1: ${module_manifest}")
endif()
get_filename_component(module_dir "${module_manifest}" DIRECTORY)
file(GLOB module_library LIST_DIRECTORIES false "${module_dir}/consumer-echo.*")
list(REMOVE_ITEM module_library "${module_manifest}")
if(NOT module_library)
    message(FATAL_ERROR "step 2: consumer-module installed no library beside ${module_manifest}")
endif()
message(STATUS "step 2: consumer-module installed consumer-echo into ${module_dir}")

# 3. The application loads the installed module.
set(app "${WORK_DIR}/consumer-app/consumer_app${exe}")
if(MULTI_CONFIG)
    set(app "${WORK_DIR}/consumer-app/${BUILD_TYPE}/consumer_app${exe}")
endif()
if(CMAKE_HOST_WIN32)
    # Windows has no RPATH: the runtime library and the DLLs it needs are in <prefix>/<BINDIR>.
    file(TO_NATIVE_PATH "${prefix}/${BINDIR}" native_bindir)
    set(ENV{PATH} "${native_bindir};$ENV{PATH}")
endif()
_run(APP "step 3: consumer_app" "${app}" "${module_dir}" "${CATALOG_DIR}")
string(STRIP "${APP_OUT}" APP_OUT)
string(JSON name ERROR_VARIABLE json_error GET "${APP_OUT}" name)
if(json_error OR NOT name STREQUAL "consumer-echo")
    message(FATAL_ERROR "step 3: consumer_app did not print the describe document of "
        "consumer-echo:\n${APP_OUT}")
endif()
message(STATUS "step 3: consumer_app loaded the installed consumer-echo: ${APP_OUT}")

# 4. Nothing installed names the source or build tree.
file(GLOB_RECURSE scanned LIST_DIRECTORIES false "${package_dir}/*")
file(GLOB_RECURSE manifests LIST_DIRECTORIES false "${prefix}/*.pmm.json")
list(APPEND manifests "${module_manifest}")
list(APPEND scanned ${manifests})
set(forbidden "")
foreach(directory IN ITEMS "${BUILD_DIR}" "${SOURCE_DIR}")
    file(REAL_PATH "${directory}" real)
    file(TO_NATIVE_PATH "${directory}" native)
    list(APPEND forbidden "${directory}" "${real}" "${native}")
endforeach()
list(REMOVE_DUPLICATES forbidden)
foreach(file IN LISTS scanned)
    file(READ "${file}" text)
    foreach(directory IN LISTS forbidden)
        string(FIND "${text}" "${directory}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "step 4: ${file} mentions ${directory}, which is the build or "
                "the source tree; installed files must not")
        endif()
    endforeach()
endforeach()
list(LENGTH scanned scanned_count)
list(LENGTH manifests manifest_count)
message(STATUS "step 4: ${scanned_count} files scanned (${manifest_count} manifests); none names "
    "the build or the source tree")

# 5 and 6. The installed tool.
if(NOT WITH_CLI)
    message(STATUS "steps 5 to 7: skipped (the build has no praenora tool)")
    return()
endif()
_check_tool("${prefix}" "steps 5 and 6")

# 7. The installation, moved.
set(moved "${WORK_DIR}/moved prëfix")
file(RENAME "${prefix}" "${moved}")
_check_tool("${moved}" "step 7")
message(STATUS "step 7: the installation works from ${moved}")
