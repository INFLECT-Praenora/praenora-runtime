# Fixture modules that use the Python service praenora.python@0, for the tests of Python modules
# (tests/integration/python_module_test.cpp, tests/contract/python_module_check_test.cpp,
# tests/python). Each is built into ${PRAENORA_TEST_PYTHON_MODULES_DIR}/<name>/, a search path of
# its own, so that the listings and counts of the fixture modules of tests/fixtures/modules stay as
# they are, and each declares the test capability test.echo@0.1.0. They build in every tree:
# a Python module's library needs no Python, and neither does fx-python-call's.
#
# The Python modules (praenora_add_python_module(), the class in <name>/):
#   fx-py-echo             copies in to out, with the parameter "add" merged in; test vectors
#   fx-py-fail             invoke raises a ValueError, two calls deep
#   fx-py-cancel           invoke polls the cancel flag and raises sdk.Cancelled
#   fx-py-ignore-cancel    invoke runs Python code for an hour without polling it
#   fx-py-print            invoke prints to sys.stdout and sys.stderr and logs a warning
#   fx-py-numpy            computes with numpy, seeded from the run's seed
#   fx-py-abort            invoke calls os.abort() (module hosts only)
#   fx-py-clash-a, -b      both ship a top-level package fx_py_clash
#   fx-py-pool             a spawn pool opened in initialize and closed in finalize; the
#                          interpreters its children ran, on request
#   fx-py-self-test        SELF_TEST, a self test that passes
#   fx-py-self-test-fail   SELF_TEST, a self test that reports a failure
#   fx-py-self-test-missing  SELF_TEST, a template that declares it, and a class without self_test
#   fx-py-concurrent       features.concurrency "instance" (a template), and a barrier of two
#   fx-py-large            a result of the numbers 0 to count - 1
#   fx-py-env              outputs PYTHONHASHSEED as its process sees it
#   fx-py-plain-class      names a class that does not derive from praenora.sdk.Module
#   fx-py-loaded           says which of the modules it is given its interpreter has loaded
#   fx-py-roadrunner       simulates an Antimony model with the environment's roadrunner package
#   fx-py-deep             recurses 900 calls deep (the parameter depth) through lru_cache
#   fx-py-thread-state     sets a context variable, numpy's error state and a threading.local
#                          value in initialize, and reads them in each invoke
# and fx-py-echo once more, from fx-py-echo-variant/, whose sources differ from fx-py-echo's in one
# line: the module fx-py-echo@0.1.0 again, built into
# ${PRAENORA_TEST_PYTHON_MODULES_VARIANT_DIR}/fx-py-echo/ (target fx_py_echo_variant).
#
# fx-python-call is a C++ module (the C++ adapter) that requires praenora.python@0.0 and calls a
# class of its own Python sources, fx-python-call/python/, which the build copies beside its
# library into python/ and whose digest it is compiled with.

# praenora_test_python_modules builds every one; the test executables that load them depend on it.
add_custom_target(praenora_test_python_modules)

# praenora_add_python_fixture_module(<name> CLASS <class> [SELF_TEST] [SOURCES_DIR <dir>]
#                                    [TARGET <target>] [OUTPUT_DIRECTORY <dir>])
# Target <name> with '-' replaced by '_' (or TARGET); the Python sources are every file of
# SOURCES_DIR (default <name>/) but its manifest template <name>.pmm.json.in, which is used when
# it exists, and its test vectors vectors.json, which are copied beside the manifest when they
# exist; the output directory is ${PRAENORA_TEST_PYTHON_MODULES_DIR}/<name> (or OUTPUT_DIRECTORY).
function(praenora_add_python_fixture_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "SELF_TEST" "CLASS;SOURCES_DIR;TARGET;OUTPUT_DIRECTORY"
        "")
    string(REPLACE "-" "_" target "${name}")
    if(DEFINED arg_TARGET)
        set(target "${arg_TARGET}")
    endif()
    set(sources_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    if(DEFINED arg_SOURCES_DIR)
        set(sources_dir "${CMAKE_CURRENT_SOURCE_DIR}/${arg_SOURCES_DIR}")
    endif()
    set(output_directory "${PRAENORA_TEST_PYTHON_MODULES_DIR}/${name}")
    if(DEFINED arg_OUTPUT_DIRECTORY)
        set(output_directory "${arg_OUTPUT_DIRECTORY}")
    endif()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS LIST_DIRECTORIES false "${sources_dir}/*")
    list(FILTER sources EXCLUDE REGEX "/__pycache__/|\\.pmm\\.json\\.in$|/vectors\\.json$")
    set(options "")
    if(arg_SELF_TEST)
        list(APPEND options SELF_TEST)
    endif()
    if(EXISTS "${sources_dir}/${name}.pmm.json.in")
        list(APPEND options MANIFEST_TEMPLATE "${sources_dir}/${name}.pmm.json.in")
    endif()
    if(EXISTS "${sources_dir}/vectors.json")
        list(APPEND options TEST_VECTORS "${sources_dir}/vectors.json")
    endif()
    praenora_add_python_module(${target}
        NAME ${name}
        VERSION 0.1.0
        CAPABILITIES test.echo@0.1.0
        CLASS "${arg_CLASS}"
        PYTHON_SOURCES ${sources}
        PYTHON_ROOT "${sources_dir}"
        OUTPUT_DIRECTORY "${output_directory}"
        ${options})
    praenora_target_warnings(${target})
    add_dependencies(praenora_test_python_modules ${target})
endfunction()

praenora_add_python_fixture_module(fx-py-echo CLASS fx_py_echo:Echo)
praenora_add_python_fixture_module(fx-py-echo
    CLASS fx_py_echo:Echo
    SOURCES_DIR fx-py-echo-variant
    TARGET fx_py_echo_variant
    OUTPUT_DIRECTORY "${PRAENORA_TEST_PYTHON_MODULES_VARIANT_DIR}/fx-py-echo")
praenora_add_python_fixture_module(fx-py-fail CLASS fx_py_fail:Fail)
praenora_add_python_fixture_module(fx-py-cancel CLASS fx_py_cancel:Cancel)
praenora_add_python_fixture_module(fx-py-ignore-cancel CLASS fx_py_ignore_cancel:IgnoreCancel)
praenora_add_python_fixture_module(fx-py-print CLASS fx_py_print:Print)
praenora_add_python_fixture_module(fx-py-numpy CLASS fx_py_numpy:Numpy)
praenora_add_python_fixture_module(fx-py-abort CLASS fx_py_abort:Abort)
praenora_add_python_fixture_module(fx-py-clash-a CLASS fx_py_clash:Echo)
praenora_add_python_fixture_module(fx-py-clash-b CLASS fx_py_clash:Echo)
praenora_add_python_fixture_module(fx-py-pool CLASS fx_py_pool:Pool)
praenora_add_python_fixture_module(fx-py-self-test CLASS fx_py_self_test:SelfTest SELF_TEST)
praenora_add_python_fixture_module(fx-py-self-test-fail
    CLASS fx_py_self_test_fail:SelfTestFail SELF_TEST)
praenora_add_python_fixture_module(fx-py-self-test-missing
    CLASS fx_py_self_test_missing:NoSelfTest SELF_TEST)
praenora_add_python_fixture_module(fx-py-concurrent CLASS fx_py_concurrent:Concurrent)
praenora_add_python_fixture_module(fx-py-large CLASS fx_py_large:Large)
praenora_add_python_fixture_module(fx-py-env CLASS fx_py_env:Env)
praenora_add_python_fixture_module(fx-py-plain-class CLASS fx_py_plain:Plain)
praenora_add_python_fixture_module(fx-py-loaded CLASS fx_py_loaded:Loaded)
praenora_add_python_fixture_module(fx-py-roadrunner CLASS fx_py_roadrunner:Roadrunner)
praenora_add_python_fixture_module(fx-py-deep CLASS fx_py_deep:Deep)
praenora_add_python_fixture_module(fx-py-thread-state CLASS fx_py_thread_state:ThreadState)

# fx-python-call: its Python sources' digest, compiled in, and their copy beside the library, made
# by the build step of praenora_add_python_module() (_praenora_python_copy_sources(), internal,
# cmake/PraenoraPythonModule.cmake). The digest is computed when the project is configured: a file
# added or removed configures it again (CONFIGURE_DEPENDS), and so does an edit
# (CMAKE_CONFIGURE_DEPENDS), so that the library never carries the digest of older sources.
set(_praenora_call_sources "${CMAKE_CURRENT_SOURCE_DIR}/fx-python-call/python")
file(GLOB_RECURSE _praenora_call_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${_praenora_call_sources}/*")
list(FILTER _praenora_call_files EXCLUDE REGEX "/__pycache__/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_praenora_call_files})
praenora_python_directory_digest(_praenora_call_digest
    ROOT "${_praenora_call_sources}" FILES ${_praenora_call_files})
praenora_add_module(fx_python_call
    NAME fx-python-call
    VERSION 0.1.0
    CAPABILITIES test.echo@0.1.0
    SOURCES fx-python-call/fx_python_call.cpp
    REQUIRES_SERVICES praenora.python@0.0
    OUTPUT_DIRECTORY "${PRAENORA_TEST_PYTHON_MODULES_DIR}/fx-python-call"
    LINK_LIBRARIES nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_definitions(fx_python_call PRIVATE
    "FX_PYTHON_CALL_SOURCES_SHA256=\"${_praenora_call_digest}\"")
praenora_target_warnings(fx_python_call)
get_target_property(_praenora_call_dir fx_python_call PRAENORA_MODULE_DIR)
_praenora_python_copy_sources(fx_python_call
    ROOT "${_praenora_call_sources}"
    FILES ${_praenora_call_files}
    DESTINATION "${_praenora_call_dir}/python")
add_dependencies(praenora_test_python_modules fx_python_call)
