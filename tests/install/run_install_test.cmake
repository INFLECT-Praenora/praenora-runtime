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
#   PROVIDERS                   the service providers a default installation includes (the
#                               roadrunner provider, in a build with PRAENORA_WITH_ROADRUNNER,
#                               the antimony provider, with PRAENORA_WITH_ANTIMONY, and the
#                               provider python, with PRAENORA_WITH_PYTHON_ENVIRONMENT)
#   LICENSE_DEPENDENCIES        the dependencies built in the tree (PRAENORA_FETCHED_DEPENDENCIES,
#                               cmake/PraenoraDependencies.cmake), whose license files the
#                               installation holds in licenses/<name>/
#   LICENSE_DIRS                PRAENORA_THIRD_PARTY_LICENSE_DIRS: the directories whose contents
#                               the installation holds in licenses/
#   PYTHON_ENVIRONMENT          with PRAENORA_WITH_PYTHON_ENVIRONMENT, the directory of the Python
#                               environment the build used, which a default installation holds;
#                               empty without it
#   PYTHON_MODULES_DIR          with it, the build tree's directory of the Python fixture modules
#                               (PRAENORA_TEST_PYTHON_MODULES_DIR), among them fx-py-env and
#                               fx-py-pool
#
# Steps, in order; the first failure ends the test, showing what ran and what it printed:
#   1. cmake --install <BUILD_DIR> --prefix <WORK_DIR>/prefix. Each kind of installed file must
#      be there, and nothing else of its kind: the tool, the runtime library, the headers (the
#      generated ones included; no template and no internal header), the package configuration,
#      the schemas and the catalog. The license files, below
#      <prefix>/<DATADIR>/praenora/licenses: this project's LICENSE at praenora-runtime/LICENSE,
#      byte for byte; at least one file in <name>/ for each of LICENSE_DEPENDENCIES; and every file
#      below each of LICENSE_DIRS at its path there, byte for byte. No file of the Python package
#      praenora.runtime is installed (the Python files of an installation with the environment are
#      the environment's, praenora.sdk's beside the Python core and those of Python modules), and
#      no example module unless INSTALL_EXAMPLES. With example modules built,
#      cmake --install ... --component Examples then installs them, with their manifests. The
#      installation's provider directory holds each of PROVIDERS, its manifest and library side by
#      side, and nothing else (no directory at all without PROVIDERS); the provider python's
#      directory holds the Python core and its files too (below). On POSIX hosts the module
#      host, <prefix>/libexec/praenora/praenora-module-host (beside <LIBDIR>:
#      <LIBDIR>/praenora/../..), is installed and executable. With PYTHON_ENVIRONMENT:
#      <prefix>/<LIBDIR>/praenora/python-environment holds the environment unchanged: its files
#      (praenora-runtime-pyenv.json among them) and nothing else, each byte for byte, its symbolic
#      links as links to the same targets and, on POSIX hosts, each file executable exactly when
#      the environment's is (with CMake 3.29 or later; before, its interpreter only);
#      <prefix>/<DATADIR>/praenora/licenses/python-environment holds the files of its licenses/,
#      byte for byte; and the provider python's directory,
#      <prefix>/<LIBDIR>/praenora/providers/python, has five entries: the provider's library and
#      manifest, the Python core's library, the environment file python-environment.json, which
#      names ../../python-environment and the core's file name, and python/, which holds the files
#      of the source tree's python/praenora/sdk (but __pycache__) below praenora/sdk, byte for byte.
#      Where otool (macOS) or readelf (Linux) is found, every RPATH of the core and of the
#      provider's library is relative to the library (@loader_path, $ORIGIN), and the core has one
#      (in a sanitizer build on macOS, the directory of the compiler's sanitizer runtime is
#      accepted too, and does not count as the core's one).
#   2. consumer-module, consumer-python-module and consumer-app are configured, with uninitialized
#      variables reported (one read in the installed PraenoraConfig.cmake, PraenoraModule.cmake,
#      PraenoraPythonModule.cmake or PraenoraPythonDigest.cmake fails; each configuration's output
#      is kept in
#      <WORK_DIR>/<consumer>-configure.log), and built, each against <prefix>;<PREFIX_PATH>; each
#      must have found Praenora in <prefix>. consumer-python-module builds a Python module with
#      the installed praenora_add_python_module(), finding no other package, whether or not the
#      installation holds the environment. A project that requires an unknown component must fail
#      to configure, with a reason that names it, also when the component is named like a CMake
#      false constant (Off, Frobnicator-NOTFOUND); one that names an unknown optional component
#      must find Praenora, with the components it requires and no other, and one that names no
#      component must get both. consumer-module then installs its module into
#      <WORK_DIR>/module-prefix through the installed praenora_add_module() (INSTALL_DESTINATION):
#      manifest and library side by side; consumer-python-module its module into
#      <WORK_DIR>/python-module-prefix: manifest, library and its Python sources
#      (consumer-python-echo.python/, byte for byte) side by side.
#   3. consumer-app runs with the installed module's directory and CATALOG_DIR: exit 0, and
#      consumer-echo's describe document on standard output. With PYTHON_ENVIRONMENT, on POSIX
#      hosts, it then runs the test catalog's test.single with fx-py-env (PYTHON_MODULES_DIR) in
#      its own process and in a module host of the installation: it sets no variable itself, so
#      fx-py-env sees no PYTHONHASHSEED in the first and 0 in the second, which the installation's
#      module host set, having found the environment file below the installation's <LIBDIR>.
#   4. No file below <prefix>/<LIBDIR>/cmake/Praenora and no installed *.pmm.json (the
#      installation's and the consumer modules'), *.provider.json or python-environment.json
#      mentions SOURCE_DIR, BUILD_DIR or PYTHON_ENVIRONMENT.
#   5. <prefix>/<BINDIR>/praenora version --json: exit 0 and a successful JSON document; the
#      tool prints the usage of serve and of status --server, and serve without --config-root is
#      refused (exit 2) before it listens; with PROVIDERS, praenora providers list --json, without
#      --provider-path, lists each of them, usable, from the installation's provider directory.
#      On POSIX hosts the installed tool runs the test catalog's test.single with consumer-echo
#      (step 2's) in a module host (--isolation process): the run completes, the installation's
#      module host started (its module_host.started record, at --log-level debug), and the run
#      record says the step ran in process mode. With PYTHON_ENVIRONMENT it runs test.single with
#      consumer-python-echo (step 2's) in its own process and, on POSIX hosts, in a module host:
#      each run completes, echoes its input, and its run record found the service
#      praenora.python@0 in the installation's provider directory.
#   6. With example modules: <prefix>/<BINDIR>/praenora modules list --json, without
#      --module-path, lists them (example-predictor-python too, with PYTHON_ENVIRONMENT) from the
#      installation's module directory, and praenora modules check passes for each installed
#      manifest (the module loads from the installation and passes its contract checks with its
#      installed test vectors). With PYTHON_ENVIRONMENT, the installed tool runs the example
#      workflow (examples/workflows/estimate-predict/workflow.yaml) and its variant whose predict
#      step is the Python module (workflow-python.yaml), with the installed modules: both complete,
#      with the same outputs, byte for byte but for the start of each envelope's id, which names
#      the run.
#   7. The installation is moved to another directory, whose name holds a space and a non-ASCII
#      character; there it still holds the license files of step 1, and from there,
#      steps 5 and 6 succeed again, with the modules found in their new place. With
#      PYTHON_ENVIRONMENT the moved installation's tool also runs test.single with fx-py-env and
#      with fx-py-pool of PYTHON_MODULES_DIR, in its own process and, on POSIX hosts, in a module
#      host: fx-py-env sees PYTHONHASHSEED 0, which the tool set, having found the environment file
#      below the moved installation's <LIBDIR>, and fx-py-pool's spawn pool starts its children
#      with the moved installation's interpreter.
# Without the tool or the example modules, the steps that need them are skipped, with a note.
# When every step passed, the installation (moved or not) is removed and the rest of WORK_DIR is
# kept, which is small; a failing run keeps everything, for inspection, until the next run empties
# WORK_DIR.
# Every command runs without the PRAENORA_* variables that the runtime reads (except
# PRAENORA_DEBUG_KEEP_LIBRARIES), without PYTHONHASHSEED and PYTHONHOME, and with PYTHON_ENVIRONMENT
# without the variables of its manifest's environment.paths, and a sanitizer report in its output
# fails the test.
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
                          OSX_DEPLOYMENT_TARGET WITH_CLI WITH_EXAMPLES INSTALL_EXAMPLES
                          PROVIDERS LICENSE_DEPENDENCIES LICENSE_DIRS PYTHON_ENVIRONMENT
                          PYTHON_MODULES_DIR)
    if(NOT DEFINED ${variable})
        set(${variable} "")
    endif()
endforeach()
foreach(variable IN ITEMS PRAENORA_MODULE_PATH PRAENORA_PROVIDER_PATH PRAENORA_CATALOG_PATH
                          PRAENORA_LOG_LEVEL PRAENORA_LOG_FORMAT PYTHONHASHSEED PYTHONHOME)
    unset(ENV{${variable}})
endforeach()
# The variables that the installed programs set from the environment's manifest are unset, so
# that what the runs see is what the programs set.
if(NOT PYTHON_ENVIRONMENT STREQUAL "")
    if(PYTHON_MODULES_DIR STREQUAL "")
        message(FATAL_ERROR "run_install_test.cmake: PYTHON_ENVIRONMENT needs PYTHON_MODULES_DIR")
    endif()
    file(READ "${PYTHON_ENVIRONMENT}/praenora-runtime-pyenv.json" python_manifest)
    string(JSON python_paths_count ERROR_VARIABLE json_error LENGTH "${python_manifest}"
        environment paths)
    if(json_error)
        message(FATAL_ERROR "run_install_test.cmake: the manifest of ${PYTHON_ENVIRONMENT} has no "
            "environment.paths: ${json_error}")
    endif()
    if(python_paths_count GREATER 0)
        math(EXPR last "${python_paths_count} - 1")
        foreach(index RANGE ${last})
            string(JSON variable MEMBER "${python_manifest}" environment paths ${index})
            unset(ENV{${variable}})
        endforeach()
    endif()
    string(JSON python_executable GET "${python_manifest}" python executable)
endif()

set(exe "")
if(CMAKE_HOST_WIN32)
    set(exe ".exe")
endif()
set(prefix "${WORK_DIR}/prefix")
set(example_modules example-estimator example-predictor)
if(NOT PYTHON_ENVIRONMENT STREQUAL "")
    list(APPEND example_modules example-predictor-python)
endif()

# _run(<prefix> <what> <command>...): praenora_test_run() (tests/support/run_command.cmake), which
# passes each argument after <what> to the program, an empty one included, and the command must
# exit with 0; its outputs go to <prefix>_OUT and <prefix>_ERR.
function(_run prefix what)
    praenora_test_run(command "${what}" "${ARGN}")
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

# Fails unless the installed file `installed` is there and equals `source`, byte for byte; `step`
# and `what` name it in messages.
function(_expect_same_file step what installed source)
    if(NOT EXISTS "${installed}" OR IS_DIRECTORY "${installed}")
        message(FATAL_ERROR "${step}: ${what} was not installed at ${installed}")
    endif()
    file(SHA256 "${installed}" installed_hash)
    file(SHA256 "${source}" source_hash)
    if(NOT installed_hash STREQUAL source_hash)
        message(FATAL_ERROR "${step}: the installed ${installed} differs from ${source} "
            "(SHA-256 ${installed_hash}, expected ${source_hash})")
    endif()
endfunction()

# Fails unless the installation in `root` holds the license files that step 1 lists: this
# project's LICENSE, a file for each of LICENSE_DEPENDENCIES and the files of LICENSE_DIRS; `step`
# names the check in messages.
function(_expect_licenses root step)
    set(licenses "${root}/${DATADIR}/praenora/licenses")
    _expect_same_file("${step}" "this project's license" "${licenses}/praenora-runtime/LICENSE"
        "${SOURCE_DIR}/LICENSE")
    message(STATUS "${step}: this project's license is installed at "
        "${licenses}/praenora-runtime/LICENSE")
    foreach(name IN LISTS LICENSE_DEPENDENCIES)
        _files_below("${licenses}/${name}" files)
        if(NOT files)
            message(FATAL_ERROR "${step}: no license file of ${name}, which is built in the tree, "
                "was installed in ${licenses}/${name}")
        endif()
        list(JOIN files ", " files)
        message(STATUS "${step}: the license files of ${name}, built in the tree: ${files}")
    endforeach()
    foreach(entry IN LISTS LICENSE_DIRS)
        if(entry STREQUAL "")
            continue()
        endif()
        cmake_path(SET dir NORMALIZE "${entry}")
        string(REGEX REPLACE "(.)/+$" "\\1" dir "${dir}")
        _files_below("${dir}" files)
        foreach(file IN LISTS files)
            _expect_same_file("${step}"
                "${dir}/${file}, of a directory that PRAENORA_THIRD_PARTY_LICENSE_DIRS names,"
                "${licenses}/${file}" "${dir}/${file}")
        endforeach()
        list(LENGTH files count)
        message(STATUS "${step}: the ${count} files of ${dir} are installed in ${licenses}")
    endforeach()
endfunction()

# _expect_relative_rpaths(<step> [WITH_RPATH <file>...] [FILES <file>...])
# Fails unless every RPATH of each file is relative to the file (@loader_path or @executable_path
# on macOS, $ORIGIN elsewhere), as otool -l or readelf -d shows them, and each file of WITH_RPATH
# has at least one (so that output this function no longer reads cannot pass for none); without
# the program, says so and checks nothing.
# One exception: when the C or C++ flags enable a sanitizer (-fsanitize=), an absolute RPATH that
# is exactly the directory holding the compiler's sanitizer runtime, as the compiler reports it
# (-print-file-name=), is accepted: the compiler adds it to every binary it links with the
# sanitizer, and a moved sanitizer installation still loads the runtime from the toolchain. Any
# other absolute RPATH fails, and without a sanitizer none is accepted.
function(_expect_relative_rpaths step)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "WITH_RPATH;FILES")
    if(CMAKE_HOST_APPLE)
        find_program(rpath_program otool)
        set(arguments -l)
    elseif(CMAKE_HOST_UNIX)
        find_program(rpath_program readelf)
        set(arguments -d)
    else()
        return()
    endif()
    if(NOT rpath_program)
        message(STATUS "${step}: the RPATHs are not checked (no otool or readelf found)")
        return()
    endif()
    set(runtime_dirs "")
    if(CMAKE_HOST_APPLE AND "${C_FLAGS} ${CXX_FLAGS}" MATCHES "-fsanitize=")
        set(compiler "${CXX_COMPILER}")
        if(compiler STREQUAL "")
            set(compiler "${C_COMPILER}")
        endif()
        foreach(runtime IN ITEMS asan tsan ubsan)
            set(runtime_file "libclang_rt.${runtime}_osx_dynamic.dylib")
            _run(RUNTIME "${step}: where ${compiler} finds ${runtime_file}" "${compiler}"
                "-print-file-name=${runtime_file}")
            string(STRIP "${RUNTIME_OUT}" runtime_path)
            if(IS_ABSOLUTE "${runtime_path}")
                get_filename_component(runtime_dir "${runtime_path}" DIRECTORY)
                list(APPEND runtime_dirs "${runtime_dir}")
            endif()
        endforeach()
    endif()
    foreach(file IN LISTS arg_WITH_RPATH arg_FILES)
        _run(RPATHS "${step}: the RPATHs of ${file}" "${rpath_program}" ${arguments} "${file}")
        if(CMAKE_HOST_APPLE)
            string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n +path [^\n]*" entries "${RPATHS_OUT}")
            set(paths "")
            foreach(entry IN LISTS entries)
                string(REGEX REPLACE ".*\n +path (.*) \\(offset [0-9]+\\)$" "\\1" path "${entry}")
                list(APPEND paths "${path}")
            endforeach()
            set(relative "^@(loader|executable)_path(/|$)")
        else()
            string(REGEX MATCHALL "\\((RPATH|RUNPATH)\\)[^\n]*\\[[^]\n]*\\]" entries
                "${RPATHS_OUT}")
            set(paths "")
            foreach(entry IN LISTS entries)
                string(REGEX REPLACE ".*\\[([^]\n]*)\\]$" "\\1" value "${entry}")
                string(REPLACE ":" ";" value "${value}")
                list(APPEND paths ${value})
            endforeach()
            set(relative "^\\$ORIGIN(/|$)")
        endif()
        set(relative_paths "")
        set(runtime_paths "")
        foreach(path IN LISTS paths)
            if(path MATCHES "${relative}")
                list(APPEND relative_paths "${path}")
            elseif(path IN_LIST runtime_dirs)
                list(APPEND runtime_paths "${path}")
            else()
                message(FATAL_ERROR "${step}: ${file} has the RPATH '${path}', which is not "
                    "relative to the library; an installation that is moved would load another "
                    "file, or none")
            endif()
        endforeach()
        if(runtime_paths)
            list(JOIN runtime_paths ", " runtime_list)
            message(STATUS "${step}: ${file} also has the sanitizer runtime's directory: "
                "${runtime_list}")
        endif()
        if(relative_paths)
            list(JOIN relative_paths ", " relative_list)
            message(STATUS "${step}: the RPATHs of ${file} are relative: ${relative_list}")
        elseif(runtime_paths AND NOT file IN_LIST arg_WITH_RPATH)
            message(STATUS "${step}: ${file} has no relative RPATH")
        elseif(file IN_LIST arg_WITH_RPATH)
            message(FATAL_ERROR "${step}: ${file} has no RPATH that ${rpath_program} shows, "
                "expected one relative to it:\n${RPATHS_OUT}")
        else()
            message(STATUS "${step}: ${file} has no RPATH")
        endif()
    endforeach()
endfunction()

# The files of praenora.sdk in the source tree (python/praenora/sdk), relative to it, but those
# below __pycache__.
function(_sdk_files out)
    _files_below("${SOURCE_DIR}/python/praenora/sdk" files)
    list(FILTER files EXCLUDE REGEX "(^|/)__pycache__/")
    set(${out} "${files}" PARENT_SCOPE)
endfunction()

# Step 1's checks of the Python environment, its license files and the provider python's directory
# in the installation in `root`.
function(_expect_python_installation root)
    set(environment "${root}/${LIBDIR}/praenora/python-environment")
    if(NOT EXISTS "${environment}/praenora-runtime-pyenv.json")
        message(FATAL_ERROR "step 1: the Python environment was not installed: "
            "${environment}/praenora-runtime-pyenv.json is missing")
    endif()
    _files_below("${PYTHON_ENVIRONMENT}" expected)
    _files_below("${environment}" found)
    if(NOT found STREQUAL expected)
        set(missing ${expected})
        list(REMOVE_ITEM missing ${found})
        set(extra ${found})
        list(REMOVE_ITEM extra ${expected})
        string(REPLACE ";" "\n  " missing "${missing}")
        string(REPLACE ";" "\n  " extra "${extra}")
        message(FATAL_ERROR "step 1: the installed Python environment ${environment} differs "
            "from ${PYTHON_ENVIRONMENT}.\nmissing:\n  ${missing}\nnot in the environment:\n  "
            "${extra}\n")
    endif()
    set(compare_executables FALSE)
    if(NOT CMAKE_HOST_WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        set(compare_executables TRUE)
    endif()
    set(links 0)
    foreach(file IN LISTS expected)
        set(source "${PYTHON_ENVIRONMENT}/${file}")
        set(copy "${environment}/${file}")
        if(IS_SYMLINK "${source}")
            file(READ_SYMLINK "${source}" source_target)
            if(NOT IS_SYMLINK "${copy}")
                message(FATAL_ERROR "step 1: the installed ${copy} is not a symbolic link; the "
                    "environment's ${source} is one, to ${source_target}")
            endif()
            file(READ_SYMLINK "${copy}" copy_target)
            if(NOT copy_target STREQUAL source_target)
                message(FATAL_ERROR "step 1: the installed link ${copy} names '${copy_target}', "
                    "the environment's '${source_target}'")
            endif()
            math(EXPR links "${links} + 1")
            continue()
        endif()
        if(IS_SYMLINK "${copy}")
            message(FATAL_ERROR "step 1: the installed ${copy} is a symbolic link; the "
                "environment's ${source} is a file")
        endif()
        _expect_same_file("step 1" "the Python environment's ${file}" "${copy}" "${source}")
        if(compare_executables)
            set(source_executable FALSE)
            set(copy_executable FALSE)
            if(IS_EXECUTABLE "${source}")
                set(source_executable TRUE)
            endif()
            if(IS_EXECUTABLE "${copy}")
                set(copy_executable TRUE)
            endif()
            if(NOT source_executable STREQUAL copy_executable)
                message(FATAL_ERROR "step 1: the installed ${copy} is executable: "
                    "${copy_executable}; the environment's: ${source_executable}")
            endif()
        endif()
    endforeach()
    if(NOT CMAKE_HOST_WIN32)
        execute_process(COMMAND test -x "${environment}/${python_executable}"
            RESULT_VARIABLE executable ENCODING UTF-8)
        if(NOT executable EQUAL 0)
            message(FATAL_ERROR "step 1: the installed interpreter "
                "${environment}/${python_executable} is not executable")
        endif()
    endif()
    list(LENGTH expected count)
    message(STATUS "step 1: the Python environment is installed unchanged in ${environment}: "
        "${count} files, ${links} of them symbolic links")

    # Its license files.
    set(licenses "${root}/${DATADIR}/praenora/licenses/python-environment")
    _files_below("${PYTHON_ENVIRONMENT}/licenses" expected)
    if(NOT expected)
        message(FATAL_ERROR "step 1: the Python environment ${PYTHON_ENVIRONMENT} has no license "
            "files in licenses/")
    endif()
    _expect_files("license files of the Python environment" "${licenses}" "${expected}")
    foreach(file IN LISTS expected)
        _expect_same_file("step 1" "the Python environment's license file ${file}"
            "${licenses}/${file}" "${PYTHON_ENVIRONMENT}/licenses/${file}")
    endforeach()
    list(LENGTH expected count)
    message(STATUS "step 1: the ${count} license files of the Python environment are installed in "
        "${licenses}")

    # The provider python's directory: five entries.
    set(provider "${root}/${LIBDIR}/praenora/providers/python")
    file(GLOB library LIST_DIRECTORIES false RELATIVE "${provider}" "${provider}/python.*")
    list(REMOVE_ITEM library python.provider.json)
    file(GLOB core LIST_DIRECTORIES false RELATIVE "${provider}"
        "${provider}/praenora-python-core.*")
    list(LENGTH library library_count)
    list(LENGTH core core_count)
    if(NOT library_count EQUAL 1 OR NOT core_count EQUAL 1)
        message(FATAL_ERROR "step 1: ${provider} holds the provider libraries '${library}' and the "
            "core libraries '${core}', expected one of each")
    endif()
    file(GLOB entries LIST_DIRECTORIES true RELATIVE "${provider}" "${provider}/*")
    list(SORT entries)
    set(expected_entries ${library} python.provider.json ${core} python-environment.json python)
    list(SORT expected_entries)
    if(NOT entries STREQUAL expected_entries OR NOT IS_DIRECTORY "${provider}/python")
        message(FATAL_ERROR "step 1: ${provider} holds '${entries}', expected the directory python "
            "and the files of '${expected_entries}'")
    endif()
    _sdk_files(sdk_files)
    _files_below("${provider}/python" found)
    list(TRANSFORM sdk_files PREPEND "praenora/sdk/" OUTPUT_VARIABLE expected)
    if(NOT found STREQUAL expected)
        message(FATAL_ERROR "step 1: ${provider}/python holds '${found}', expected the files of "
            "praenora.sdk: '${expected}'")
    endif()
    foreach(file IN LISTS sdk_files)
        _expect_same_file("step 1" "praenora.sdk's ${file}"
            "${provider}/python/praenora/sdk/${file}" "${SOURCE_DIR}/python/praenora/sdk/${file}")
    endforeach()
    file(READ "${provider}/python-environment.json" text)
    string(JSON environment_entry ERROR_VARIABLE json_error GET "${text}" environment)
    string(JSON core_entry ERROR_VARIABLE core_error GET "${text}" core)
    if(json_error OR core_error OR NOT environment_entry STREQUAL "../../python-environment"
       OR NOT core_entry STREQUAL core)
        message(FATAL_ERROR "step 1: ${provider}/python-environment.json holds:\n${text}\n"
            "expected the environment ../../python-environment and the core ${core}, both "
            "relative to the file")
    endif()
    list(JOIN expected_entries ", " listed_entries)
    message(STATUS "step 1: ${provider} holds ${listed_entries}; python-environment.json names "
        "${environment_entry} and ${core_entry}")
    # The core finds the environment's libpython through its RPATH; the provider links nothing of
    # the installation's.
    _expect_relative_rpaths("step 1" WITH_RPATH "${provider}/${core}"
        FILES "${provider}/${library}")
endfunction()

# Writes <dir>/workflow.yaml and <dir>/source.json: the test catalog's test.single, two ticks, its
# step a bound to `module`, with `settings` (YAML flow-mapping entries of the step after its module;
# may be empty), and its input {"value": 42}.
function(_write_single_config dir module settings)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/source.json" "{\"value\": 42}\n")
    if(NOT settings STREQUAL "")
        set(settings ", ${settings}")
    endif()
    file(WRITE "${dir}/workflow.yaml" "kind: workflow-config\n"
        "format_version: \"0.1.0\"\n"
        "workflow: { name: test.single, version: \"0.1.0\" }\n"
        "run: { ticks: 2 }\n"
        "inputs:\n"
        "  source: { source: json-document, path: source.json }\n"
        "steps:\n"
        "  a: { module: { name: ${module}, version: \"0.1.0\" }${settings} }\n")
endfunction()

# The payload of the last record of <out_dir>/outputs/result.jsonl, in `out`.
function(_last_result out_dir out)
    file(READ "${out_dir}/outputs/result.jsonl" text)
    string(REGEX MATCH "[^\n]+\n?$" last "${text}")
    string(JSON data ERROR_VARIABLE json_error GET "${last}" data)
    if(json_error)
        message(FATAL_ERROR "${out_dir}/outputs/result.jsonl ends in no record with data:\n"
            "${text}")
    endif()
    set(${out} "${data}" PARENT_SCOPE)
endfunction()

# Runs `tool` run <dir>/workflow.yaml --isolation <mode> into <dir>/out-<mode>, with ARGN after
# it, and fails unless the run completes; `step` and `what` name it. Sets <prefix>_DATA to the
# payload of the last record of its output result and <prefix>_RECORD to its run record.
function(_run_single prefix step what tool dir mode)
    set(out_dir "${dir}/out-${mode}")
    file(REMOVE_RECURSE "${out_dir}")
    _run(SINGLE "${step}: ${what} (${mode})" "${tool}" run "${dir}/workflow.yaml"
        --isolation "${mode}" --output-dir "${out_dir}" --json ${ARGN})
    string(JSON state ERROR_VARIABLE json_error GET "${SINGLE_OUT}" result state)
    if(json_error OR NOT state STREQUAL "completed")
        message(FATAL_ERROR "${step}: ${what} (${mode}) did not complete:\n${SINGLE_OUT}\n"
            "${SINGLE_ERR}")
    endif()
    _last_result("${out_dir}" data)
    file(READ "${out_dir}/run-record.json" record)
    set(${prefix}_DATA "${data}" PARENT_SCOPE)
    set(${prefix}_RECORD "${record}" PARENT_SCOPE)
endfunction()

# Where a Python step runs: in the tool's process, and, on POSIX hosts, in a module host.
set(python_modes in-process)
if(NOT CMAKE_HOST_WIN32)
    list(APPEND python_modes process)
endif()

# Step 5's runs of consumer-python-echo (step 2's) with the installed tool `tool`.
function(_check_python_module tool step)
    _write_single_config("${WORK_DIR}/python-echo" consumer-python-echo "")
    foreach(mode IN LISTS python_modes)
        _run_single(ECHO "${step}" "praenora run with consumer-python-echo" "${tool}"
            "${WORK_DIR}/python-echo" "${mode}" --module-path "${python_module_dir}"
            --catalog-path "${CATALOG_DIR}")
        string(JSON echoed ERROR_VARIABLE echoed_error GET "${ECHO_DATA}" echoed_by)
        string(JSON value ERROR_VARIABLE value_error GET "${ECHO_DATA}" value)
        if(echoed_error OR value_error OR NOT echoed STREQUAL "consumer-python-echo"
           OR NOT value EQUAL 42)
            message(FATAL_ERROR "${step}: consumer-python-echo (${mode}) did not echo its input "
                "{\"value\": 42}: ${ECHO_DATA}")
        endif()
        set(location execution manifests services "praenora.python@0")
        string(JSON origin ERROR_VARIABLE origin_error GET "${ECHO_RECORD}" ${location}
            search_path origin)
        string(JSON manifest ERROR_VARIABLE manifest_error GET "${ECHO_RECORD}" ${location} file)
        if(origin_error OR manifest_error OR NOT origin STREQUAL "installation"
           OR NOT manifest STREQUAL "python/python.provider.json")
            message(FATAL_ERROR "${step}: the run record of consumer-python-echo (${mode}) did not "
                "find praenora.python@0 in the installation's provider directory: "
                "${origin_error}${manifest_error}${origin} ${manifest}")
        endif()
        message(STATUS "${step}: the installed tool ran consumer-python-echo (${mode}) with the "
            "installation's provider python: ${ECHO_DATA}")
    endforeach()
endfunction()

# Step 6's runs of the example workflow and of its variant with the Python predictor, with the
# installed tool `tool` and the installed example modules: the same outputs, byte for byte.
function(_check_python_example tool step)
    set(example "${SOURCE_DIR}/examples/workflows/estimate-predict")
    foreach(variant IN ITEMS workflow workflow-python)
        set(out_dir "${WORK_DIR}/example/${variant}")
        file(REMOVE_RECURSE "${out_dir}")
        _run(EXAMPLE "${step}: praenora run ${variant}.yaml" "${tool}" run
            "${example}/${variant}.yaml" --output-dir "${out_dir}" --json)
        string(JSON state ERROR_VARIABLE json_error GET "${EXAMPLE_OUT}" result state)
        if(json_error OR NOT state STREQUAL "completed")
            message(FATAL_ERROR "${step}: the example's ${variant}.yaml did not complete:\n"
                "${EXAMPLE_OUT}\n${EXAMPLE_ERR}")
        endif()
    endforeach()
    # Each envelope's id starts with the name of its run (16 hexadecimal digits), which differ.
    foreach(output IN ITEMS predictions state)
        set(cpp "${WORK_DIR}/example/workflow/outputs/${output}.jsonl")
        set(python "${WORK_DIR}/example/workflow-python/outputs/${output}.jsonl")
        foreach(variant IN ITEMS cpp python)
            file(READ "${${variant}}" ${variant}_text)
            string(REGEX REPLACE "\"id\":\"[0-9a-f]+:" "\"id\":\"<run>:" ${variant}_text
                "${${variant}_text}")
        endforeach()
        if(cpp_text STREQUAL "" OR NOT python_text STREQUAL cpp_text)
            message(FATAL_ERROR "${step}: the output ${output} of the example with the Python "
                "predictor, ${python}, differs from the one with the C++ predictor, ${cpp}, "
                "beyond the names of their runs")
        endif()
    endforeach()
    message(STATUS "${step}: the example workflow gives the same outputs with "
        "example-predictor-python as with example-predictor, byte for byte but for the names of "
        "the runs in the envelopes' ids")
endfunction()

# Step 7's runs of the Python fixture modules fx-py-env and fx-py-pool (PYTHON_MODULES_DIR) with
# the tool of the installation in `root`.
function(_check_python_fixtures root step)
    set(tool "${root}/${BINDIR}/praenora${exe}")
    file(REAL_PATH "${root}" real_root)
    _write_single_config("${WORK_DIR}/python-env" fx-py-env "")
    _write_single_config("${WORK_DIR}/python-pool" fx-py-pool
        "parameters: { report_executables: true }")
    foreach(mode IN LISTS python_modes)
        _run_single(ENV "${step}" "praenora run with fx-py-env" "${tool}" "${WORK_DIR}/python-env"
            "${mode}" --module-path "${PYTHON_MODULES_DIR}" --catalog-path "${CATALOG_DIR}")
        string(JSON seed ERROR_VARIABLE json_error GET "${ENV_DATA}" PYTHONHASHSEED)
        if(json_error OR NOT seed STREQUAL "0")
            message(FATAL_ERROR "${step}: fx-py-env (${mode}) saw PYTHONHASHSEED '${seed}', not "
                "0: the installed tool did not find its environment file: ${ENV_DATA}")
        endif()
        _run_single(POOL "${step}" "praenora run with fx-py-pool" "${tool}"
            "${WORK_DIR}/python-pool" "${mode}" --module-path "${PYTHON_MODULES_DIR}"
            --catalog-path "${CATALOG_DIR}")
        string(JSON start_method ERROR_VARIABLE json_error GET "${POOL_DATA}" start_method)
        string(JSON count ERROR_VARIABLE count_error LENGTH "${POOL_DATA}" executables)
        if(json_error OR count_error OR NOT start_method STREQUAL "spawn" OR NOT count GREATER 0)
            message(FATAL_ERROR "${step}: fx-py-pool (${mode}) did not report a spawn pool and "
                "its children's interpreters: ${POOL_DATA}")
        endif()
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON executable GET "${POOL_DATA}" executables ${index})
            file(TO_CMAKE_PATH "${executable}" executable)
            file(REAL_PATH "${executable}" real_executable)
            string(FIND "${real_executable}" "${real_root}/" position)
            if(NOT position EQUAL 0)
                message(FATAL_ERROR "${step}: a child of fx-py-pool's pool (${mode}) ran "
                    "${executable}, which is not in the installation ${real_root}")
            endif()
        endforeach()
        message(STATUS "${step}: fx-py-env (${mode}) saw PYTHONHASHSEED 0, and fx-py-pool's "
            "children (${mode}) ran ${POOL_DATA}")
    endforeach()
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

    # The control API's commands: their usage, and serve refusing a missing --config-root before
    # it listens. Listening itself is tested in the build tree (label server): this driver cannot
    # run a server beside a client.
    _run(SERVE_HELP "${step}: praenora serve --help" "${tool}" serve --help)
    if(NOT SERVE_HELP_OUT MATCHES "^usage: praenora serve --listen ADDR --config-root D")
        message(FATAL_ERROR "${step}: 'praenora serve --help' did not print its usage:\n"
            "${SERVE_HELP_OUT}")
    endif()
    _run(STATUS_HELP "${step}: praenora status --help" "${tool}" status --help)
    if(NOT STATUS_HELP_OUT MATCHES "\n +praenora status --server ADDR \\[--run ID\\]")
        message(FATAL_ERROR "${step}: 'praenora status --help' did not print the usage of "
            "status --server:\n${STATUS_HELP_OUT}")
    endif()
    set(serve_command "${tool}" serve --listen 127.0.0.1:0 --output-root
        "${WORK_DIR}/serve-out")
    praenora_test_run(SERVE "${step}: praenora serve without --config-root" "${serve_command}")
    if(NOT SERVE_EXIT STREQUAL "2" OR NOT SERVE_ERR MATCHES "serve needs --config-root D")
        praenora_test_fail("${step}: praenora serve without --config-root"
            "expected exit code 2 and \"serve needs --config-root D\" on stderr"
            "${serve_command}" "${SERVE_EXIT}" "${SERVE_OUT}" "${SERVE_ERR}")
    endif()
    message(STATUS "${step}: the installed tool prints the usage of serve and status --server, "
        "and serve refuses a missing --config-root")

    # A step in a module host, the installation's own, which the runtime finds beside its library.
    if(NOT CMAKE_HOST_WIN32)
        set(isolation_out "${WORK_DIR}/isolation/out")
        file(REMOVE_RECURSE "${isolation_out}")
        _run(ISOLATION "${step}: praenora run --isolation process" "${tool}" run
            "${isolation_config}" --module-path "${module_dir}" --catalog-path "${CATALOG_DIR}"
            --isolation process --output-dir "${isolation_out}" --log-level debug --json)
        string(JSON state ERROR_VARIABLE json_error GET "${ISOLATION_OUT}" result state)
        if(json_error OR NOT state STREQUAL "completed")
            message(FATAL_ERROR "${step}: the run with --isolation process did not complete:\n"
                "${ISOLATION_OUT}\n${ISOLATION_ERR}")
        endif()
        if(NOT ISOLATION_ERR MATCHES "module host started for module consumer-echo@0\\.1\\.0")
            message(FATAL_ERROR "${step}: the run with --isolation process logged no start of a "
                "module host:\n${ISOLATION_ERR}")
        endif()
        file(READ "${isolation_out}/run-record.json" record)
        string(JSON mode ERROR_VARIABLE json_error GET "${record}" execution isolation a mode)
        if(json_error OR NOT mode STREQUAL "process")
            message(FATAL_ERROR "${step}: the run record does not say that step a ran in a module "
                "host: ${json_error}${mode}")
        endif()
        message(STATUS "${step}: the installed tool ran a step in its module host")
    endif()

    # The providers of the installation, found in its provider directory.
    if(PROVIDERS)
        _run(PROVIDERS_LIST "${step}: praenora providers list" "${tool}" providers list --json)
        string(JSON count ERROR_VARIABLE json_error LENGTH "${PROVIDERS_LIST_OUT}" result)
        if(json_error)
            message(FATAL_ERROR "${step}: 'praenora providers list --json' did not print a list "
                "of providers:\n${PROVIDERS_LIST_OUT}")
        endif()
        file(REAL_PATH "${root}/${LIBDIR}/praenora/providers" providers_dir)
        set(usable_providers "")
        if(count GREATER 0)
            math(EXPR last "${count} - 1")
            foreach(index RANGE ${last})
                string(JSON name GET "${PROVIDERS_LIST_OUT}" result ${index} name)
                string(JSON usable GET "${PROVIDERS_LIST_OUT}" result ${index} usable)
                string(JSON manifest GET "${PROVIDERS_LIST_OUT}" result ${index} manifest)
                file(TO_CMAKE_PATH "${manifest}" manifest)
                file(REAL_PATH "${manifest}" manifest)
                string(FIND "${manifest}" "${providers_dir}/" position)
                if(usable AND position EQUAL 0)
                    list(APPEND usable_providers "${name}")
                endif()
            endforeach()
        endif()
        foreach(name IN LISTS PROVIDERS)
            if(NOT name IN_LIST usable_providers)
                message(FATAL_ERROR "${step}: the installed praenora tool does not list provider "
                    "${name}, usable, from the installation's provider directory "
                    "${providers_dir}:\n${PROVIDERS_LIST_OUT}")
            endif()
        endforeach()
        list(JOIN PROVIDERS ", " listed_providers)
        message(STATUS "${step}: the installed tool lists ${listed_providers}, usable, from "
            "${providers_dir}")
    endif()

    # A Python module built outside this build (step 2), with the installation's provider python.
    if(NOT PYTHON_ENVIRONMENT STREQUAL "")
        _check_python_module("${tool}" "${step}")
    endif()

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
    if(NOT PYTHON_ENVIRONMENT STREQUAL "")
        _check_python_example("${tool}" "${step}")
    endif()
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
if(NOT CMAKE_HOST_WIN32)
    cmake_path(SET module_host NORMALIZE
        "${prefix}/${LIBDIR}/praenora/../../libexec/praenora/praenora-module-host")
    if(NOT EXISTS "${module_host}" OR IS_DIRECTORY "${module_host}")
        message(FATAL_ERROR "step 1: the module host was not installed at ${module_host}")
    endif()
    execute_process(COMMAND test -x "${module_host}" RESULT_VARIABLE executable ENCODING UTF-8)
    if(NOT executable EQUAL 0)
        message(FATAL_ERROR "step 1: the installed module host ${module_host} is not executable")
    endif()
    message(STATUS "step 1: the module host is installed at ${module_host}")
endif()
file(GLOB_RECURSE expected_headers LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/include"
    "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/include/*.hpp")
list(APPEND expected_headers praenora/runtime/export.h praenora/sdk/version.h)
_expect_files(headers "${prefix}/${INCLUDEDIR}" "${expected_headers}")
foreach(kind IN ITEMS schemas catalog)
    file(GLOB_RECURSE expected LIST_DIRECTORIES false RELATIVE "${SOURCE_DIR}/spec/${kind}"
        "${SOURCE_DIR}/spec/${kind}/*.json")
    _expect_files(${kind} "${prefix}/${DATADIR}/praenora/${kind}" "${expected}")
endforeach()
_expect_licenses("${prefix}" "step 1")
set(package_dir "${prefix}/${LIBDIR}/cmake/Praenora")
foreach(file IN ITEMS PraenoraConfig.cmake PraenoraConfigVersion.cmake PraenoraModule.cmake
                      PraenoraPythonModule.cmake PraenoraPythonDigest.cmake
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
# No file of the Python package praenora.runtime (a wheel's) is installed. With the environment, the
# installation's Python files are the environment's, praenora.sdk's in the Python core's
# directory, and those of Python modules, in <name>.python/ beside their manifests.
file(GLOB_RECURSE python_files LIST_DIRECTORIES false RELATIVE "${prefix}" "${prefix}/*.py"
    "${prefix}/*_bindings*")
if(NOT PYTHON_ENVIRONMENT STREQUAL "")
    string(REGEX REPLACE "([][+.*()^$?|])" "\\\\\\1" libdir_pattern "${LIBDIR}")
    list(FILTER python_files EXCLUDE REGEX
        "^${libdir_pattern}/praenora/(python-environment|providers/python/python/praenora/sdk)/")
    list(FILTER python_files EXCLUDE REGEX
        "^${libdir_pattern}/praenora/modules/[^/]+/[^/]+[.]python/")
endif()
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
set(providers_dir "${prefix}/${LIBDIR}/praenora/providers")
if(PROVIDERS)
    set(expected_providers "")
    foreach(name IN LISTS PROVIDERS)
        list(APPEND expected_providers "${name}/${name}.provider.json")
        file(GLOB provider_library LIST_DIRECTORIES false RELATIVE "${providers_dir}"
            "${providers_dir}/${name}/${name}.*")
        list(REMOVE_ITEM provider_library "${name}/${name}.provider.json")
        if(NOT provider_library)
            message(FATAL_ERROR "step 1: provider ${name}'s library was not installed beside "
                "its manifest in ${providers_dir}/${name}")
        endif()
        list(APPEND expected_providers ${provider_library})
        # The provider python lies in the Python core's directory, with the core, its environment
        # file and praenora.sdk (_expect_python_installation() checks them).
        if(name STREQUAL "python")
            file(GLOB core_library LIST_DIRECTORIES false RELATIVE "${providers_dir}"
                "${providers_dir}/python/praenora-python-core.*")
            _sdk_files(sdk_files)
            list(TRANSFORM sdk_files PREPEND "python/python/praenora/sdk/")
            list(APPEND expected_providers ${core_library} python/python-environment.json
                ${sdk_files})
        endif()
    endforeach()
    _expect_files(providers "${providers_dir}" "${expected_providers}")
elseif(EXISTS "${providers_dir}")
    _files_below("${providers_dir}" installed_providers)
    message(FATAL_ERROR "step 1: a provider was installed, although the build has none: "
        "${installed_providers}")
endif()
if(NOT PYTHON_ENVIRONMENT STREQUAL "")
    if(NOT "python" IN_LIST PROVIDERS)
        message(FATAL_ERROR "step 1: PYTHON_ENVIRONMENT is given, but PROVIDERS does not name the "
            "provider python")
    endif()
    _expect_python_installation("${prefix}")
elseif(EXISTS "${prefix}/${LIBDIR}/praenora/python-environment")
    message(FATAL_ERROR "step 1: a Python environment was installed, although the build has none")
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
foreach(consumer IN ITEMS consumer-module consumer-python-module consumer-app)
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
        foreach(file IN ITEMS PraenoraConfig.cmake PraenoraModule.cmake
                              PraenoraPythonModule.cmake PraenoraPythonDigest.cmake)
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
# These projects enable no language: a compiler check fails in the one whose directory name ends in
# -NOTFOUND (with the Visual Studio generator), and CMake searches <prefix>/lib64/cmake only once a
# language is enabled. So they name the installed package's directory (Praenora_DIR, with the
# build's own LIBDIR) instead of leaving it to a search of the prefix, which would not find an
# installation made to lib64, as GNUInstallDirs does on Fedora, RHEL, Rocky and openSUSE. The
# consumers above search the prefix.
# A project that requires an unknown component fails to configure, and the reason the package
# gives names that component and the package's components. The unknown component is the last one
# each project names. Off and Frobnicator-NOTFOUND are named like CMake false constants: if()
# reads a variable that holds just Off, or a list that ends in -NOTFOUND, as false, and takes an
# argument that ends in -NOTFOUND for a false constant without reading the variable it names.
foreach(components IN ITEMS "Runtime Frobnicator" "Off" "Runtime Frobnicator-NOTFOUND")
    string(REGEX MATCH "[^ ]+$" unknown "${components}")
    set(unknown_dir "${WORK_DIR}/unknown-component-${unknown}")
    file(WRITE "${unknown_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
        "project(praenora_unknown_component LANGUAGES NONE)\n"
        "find_package(Praenora 0.2 REQUIRED COMPONENTS ${components})\n")
    # The command is run and, on a failure, shown from this one list.
    set(unknown_command "${CMAKE_COMMAND}" -S "${unknown_dir}" -B "${unknown_dir}/build"
        -C "${settings_file}" "-DPraenora_DIR=${package_dir}" ${generator_args})
    praenora_test_run(UNKNOWN "step 2: an unknown component (${components})"
        "${unknown_command}")
    # CMake wraps the reason's lines.
    string(REGEX REPLACE "[ \t\r\n]+" " " unknown_output "${UNKNOWN_OUT}${UNKNOWN_ERR}")
    string(FIND "${unknown_output}" "Reason given by package: " reason_position)
    if(reason_position EQUAL -1)
        set(unknown_reason "")
    else()
        string(SUBSTRING "${unknown_output}" ${reason_position} -1 unknown_reason)
    endif()
    if(UNKNOWN_EXIT STREQUAL "0" OR NOT unknown_reason MATCHES "${unknown}"
       OR NOT unknown_reason MATCHES "Runtime" OR NOT unknown_reason MATCHES "ModuleSDK")
        set(problem "it did not fail with a reason that names the component ${unknown} and the ")
        string(APPEND problem "components Runtime and ModuleSDK")
        praenora_test_fail("step 2: requiring ${components}" "${problem}" "${unknown_command}"
            "${UNKNOWN_EXIT}" "${UNKNOWN_OUT}" "${UNKNOWN_ERR}")
    endif()
    message(STATUS "step 2: requiring ${components}: ${unknown} is not found, and the reason "
        "names it")
endforeach()
# A project that names an unknown optional component finds Praenora, with the components it
# requires and no other.
set(optional_dir "${WORK_DIR}/unknown-optional-component")
file(WRITE "${optional_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
    "project(praenora_unknown_optional_component LANGUAGES NONE)\n"
    "find_package(Praenora 0.2 REQUIRED COMPONENTS Runtime\n"
    "    OPTIONAL_COMPONENTS Frobnicator-NOTFOUND)\n"
    "if(NOT TARGET Praenora::Runtime OR TARGET Praenora::ModuleSDK\n"
    "   OR Praenora_Frobnicator-NOTFOUND_FOUND)\n"
    "    message(FATAL_ERROR \"expected Praenora::Runtime alone and no Frobnicator-NOTFOUND\")\n"
    "endif()\n")
_run(OPTIONAL "step 2: an unknown optional component"
    "${CMAKE_COMMAND}" -S "${optional_dir}" -B "${optional_dir}/build" -C "${settings_file}"
    "-DPraenora_DIR=${package_dir}" ${generator_args})
message(STATUS "step 2: requiring Runtime, with the optional component Frobnicator-NOTFOUND: "
    "Praenora is found, with Praenora::Runtime alone")
# A project that names no component gets both.
set(none_dir "${WORK_DIR}/no-component")
file(WRITE "${none_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\n"
    "project(praenora_no_component LANGUAGES NONE)\n"
    "find_package(Praenora 0.2 REQUIRED)\n"
    "if(NOT TARGET Praenora::Runtime OR NOT TARGET Praenora::ModuleSDK)\n"
    "    message(FATAL_ERROR \"expected Praenora::Runtime and Praenora::ModuleSDK\")\n"
    "endif()\n")
_run(NONE "step 2: no component"
    "${CMAKE_COMMAND}" -S "${none_dir}" -B "${none_dir}/build" -C "${settings_file}"
    "-DPraenora_DIR=${package_dir}" ${generator_args})
message(STATUS "step 2: naming no component gives Praenora::Runtime and Praenora::ModuleSDK")
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
# consumer-python-module installs its Python module with the installed
# praenora_add_python_module(): its manifest, library and Python sources, side by side.
set(python_module_prefix "${WORK_DIR}/python-module-prefix")
_run(PYTHON_MODULE_INSTALL "step 2: cmake --install consumer-python-module"
    "${CMAKE_COMMAND}" --install "${WORK_DIR}/consumer-python-module"
    --prefix "${python_module_prefix}" ${config_args})
file(GLOB_RECURSE python_module_manifest LIST_DIRECTORIES false
    "${python_module_prefix}/*.pmm.json")
list(LENGTH python_module_manifest python_module_manifest_count)
if(NOT python_module_manifest_count EQUAL 1)
    message(FATAL_ERROR "step 2: consumer-python-module installed "
        "${python_module_manifest_count} manifests, expected 1: ${python_module_manifest}")
endif()
get_filename_component(python_module_dir "${python_module_manifest}" DIRECTORY)
file(GLOB python_module_library LIST_DIRECTORIES false
    "${python_module_dir}/consumer-python-echo.*")
list(REMOVE_ITEM python_module_library "${python_module_manifest}")
if(NOT python_module_library)
    message(FATAL_ERROR "step 2: consumer-python-module installed no library beside "
        "${python_module_manifest}")
endif()
set(python_sources "${python_module_dir}/consumer-python-echo.python")
_files_below("${python_sources}" installed_sources)
if(NOT installed_sources STREQUAL "consumer_python_echo/__init__.py")
    message(FATAL_ERROR "step 2: consumer-python-module installed '${installed_sources}' in "
        "${python_sources}, expected its Python sources, consumer_python_echo/__init__.py")
endif()
_expect_same_file("step 2" "consumer-python-echo's Python source"
    "${python_sources}/consumer_python_echo/__init__.py"
    "${TEST_DIR}/consumer-python-module/consumer_python_echo/__init__.py")
message(STATUS "step 2: consumer-python-module installed consumer-python-echo, with its Python "
    "sources, into ${python_module_dir}")

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
# An application sets no variable of the Python environment: what fx-py-env sees in a module host
# of the installation is what that module host set.
if(NOT PYTHON_ENVIRONMENT STREQUAL "" AND NOT CMAKE_HOST_WIN32)
    _write_single_config("${WORK_DIR}/app-in-process" fx-py-env "")
    _write_single_config("${WORK_DIR}/app-process" fx-py-env "isolation: { mode: process }")
    foreach(mode IN ITEMS in-process process)
        set(out_dir "${WORK_DIR}/app-${mode}/out")
        _run(APP_RUN "step 3: consumer_app runs fx-py-env (${mode})" "${app}"
            "${PYTHON_MODULES_DIR}" "${CATALOG_DIR}" "${WORK_DIR}/app-${mode}/workflow.yaml"
            "${out_dir}")
        string(STRIP "${APP_RUN_OUT}" APP_RUN_OUT)
        if(NOT APP_RUN_OUT STREQUAL "completed")
            message(FATAL_ERROR "step 3: consumer_app's run of fx-py-env (${mode}) ended "
                "'${APP_RUN_OUT}':\n${APP_RUN_ERR}")
        endif()
        _last_result("${out_dir}" data)
        string(JSON seed_type ERROR_VARIABLE json_error TYPE "${data}" PYTHONHASHSEED)
        if(mode STREQUAL "process")
            string(JSON seed ERROR_VARIABLE seed_error GET "${data}" PYTHONHASHSEED)
            set(expected "the module host set it to 0")
            set(seen_expected FALSE)
            if(NOT json_error AND NOT seed_error AND seed STREQUAL "0")
                set(seen_expected TRUE)
            endif()
        else()
            set(expected "the application did not set it")
            set(seen_expected FALSE)
            if(NOT json_error AND seed_type STREQUAL "NULL")
                set(seen_expected TRUE)
            endif()
        endif()
        if(NOT seen_expected)
            message(FATAL_ERROR "step 3: fx-py-env, run by consumer_app (${mode}), saw "
                "PYTHONHASHSEED as ${data}; expected that ${expected}")
        endif()
        message(STATUS "step 3: consumer_app's run of fx-py-env (${mode}): ${data}")
    endforeach()
endif()

# 4. Nothing installed names the source or build tree.
file(GLOB_RECURSE scanned LIST_DIRECTORIES false "${package_dir}/*")
file(GLOB_RECURSE manifests LIST_DIRECTORIES false "${prefix}/*.pmm.json"
    "${prefix}/*.provider.json" "${prefix}/python-environment.json")
list(APPEND manifests "${module_manifest}" "${python_module_manifest}")
list(APPEND scanned ${manifests})
set(forbidden "")
foreach(directory IN ITEMS "${BUILD_DIR}" "${SOURCE_DIR}" "${PYTHON_ENVIRONMENT}")
    if(directory STREQUAL "")
        continue()
    endif()
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
message(STATUS "step 4: ${scanned_count} files scanned (${manifest_count} manifests and "
    "environment files); none names the build or the source tree, or the Python environment")

# A passing run removes the installation it made, which with the Python environment is about a
# gigabyte, and keeps the rest of WORK_DIR (the consumers' builds and installations, the runs'
# outputs and logs: a few megabytes). A failing run stops before this, keeping everything.
function(_remove_installation installation)
    file(REMOVE_RECURSE "${installation}")
    message(STATUS "the test passed: the installation ${installation} is removed; the rest of "
        "${WORK_DIR} is kept (a failing run keeps the installation too)")
endfunction()

# 5 and 6. The installed tool. Its run in a module host uses this configuration of the test
# catalog's test.single, bound to consumer-echo.
if(NOT WITH_CLI)
    message(STATUS "steps 5 to 7: skipped (the build has no praenora tool)")
    _remove_installation("${prefix}")
    return()
endif()
set(isolation_config "${WORK_DIR}/isolation/workflow.yaml")
file(WRITE "${WORK_DIR}/isolation/source.json" "{\"value\": 42}\n")
file(WRITE "${isolation_config}" "kind: workflow-config\n"
    "format_version: \"0.1.0\"\n"
    "workflow: { name: test.single, version: \"0.1.0\" }\n"
    "run: { ticks: 2 }\n"
    "inputs:\n"
    "  source: { source: json-document, path: source.json }\n"
    "steps:\n"
    "  a: { module: { name: consumer-echo, version: \"0.1.0\" } }\n")
_check_tool("${prefix}" "steps 5 and 6")

# 7. The installation, moved.
set(moved "${WORK_DIR}/moved prëfix")
file(RENAME "${prefix}" "${moved}")
_expect_licenses("${moved}" "step 7")
_check_tool("${moved}" "step 7")
if(NOT PYTHON_ENVIRONMENT STREQUAL "")
    _check_python_fixtures("${moved}" "step 7")
endif()
message(STATUS "step 7: the installation works from ${moved}")
_remove_installation("${moved}")
