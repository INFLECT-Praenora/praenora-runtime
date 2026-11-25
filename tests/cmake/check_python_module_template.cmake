# Test python.module_template_check: praenora_add_python_module() (cmake/PraenoraPythonModule.cmake)
# stops the configuration when SELF_TEST is given and the manifest configured from
# MANIFEST_TEMPLATE does not declare features.self_test, and when it declares it without SELF_TEST;
# it accepts a template that declares it with SELF_TEST, and without a template generates a
# manifest that declares it; it stops for a CLASS whose top-level package is not among the
# sources, for sources with another top-level entry beside that package (a module or a
# directory), and for a template that is not JSON, saying so, with SELF_TEST or without. Each case
# configures the tiny project python-module-template with the build's generator and compiler. Run
# as
#   cmake -DPRAENORA_SOURCE_DIR=<repo> -DPRAENORA_WORK_DIR=<dir> -DGENERATOR=<generator>
#         [-DGENERATOR_PLATFORM=<p>] [-DGENERATOR_TOOLSET=<t>] [-DMAKE_PROGRAM=<program>]
#         [-DCXX_COMPILER=<compiler>] -P check_python_module_template.cmake

foreach(variable IN ITEMS PRAENORA_SOURCE_DIR PRAENORA_WORK_DIR GENERATOR)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "give -D${variable}")
    endif()
endforeach()

set(generator_args -G "${GENERATOR}")
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_args -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_args -T "${GENERATOR_TOOLSET}")
endif()
if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
    list(APPEND generator_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(DEFINED CXX_COMPILER AND NOT CXX_COMPILER STREQUAL "" AND NOT GENERATOR MATCHES "^Visual Studio")
    list(APPEND generator_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

# Configures the tiny project for `case`, in a directory of its own: `succeeds` TRUE or FALSE, and
# `expected`, text that its output must hold (its error, for a refused case). The build directory
# is <work dir>/<case>.
function(configure_case case succeeds expected)
    set(build "${PRAENORA_WORK_DIR}/${case}")
    file(REMOVE_RECURSE "${build}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${PRAENORA_SOURCE_DIR}/tests/cmake/python-module-template"
            -B "${build}" ${generator_args} "-DPRAENORA_SOURCE_DIR=${PRAENORA_SOURCE_DIR}"
            "-DCASE=${case}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        ENCODING UTF-8)
    # CMake wraps the lines of a message; the expected text is looked for with the line breaks and
    # the indentation of the wrapped lines taken out.
    string(REGEX REPLACE "\n +" " " joined "${out}${err}")
    string(REPLACE "\n" " " joined "${joined}")
    if(succeeds AND NOT result EQUAL 0)
        message(FATAL_ERROR "${case}: the configuration failed (${result}), it should not:\n"
            "${out}${err}")
    endif()
    if(NOT succeeds AND result EQUAL 0)
        message(FATAL_ERROR "${case}: the configuration succeeded, it should have failed:\n"
            "${out}${err}")
    endif()
    string(FIND "${joined}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${case}: the output does not say '${expected}':\n${out}${err}")
    endif()
    message(STATUS "${case}: ${result}, as expected")
    set(case_build "${build}" PARENT_SCOPE)
endfunction()

configure_case(self_test_lacking FALSE "SELF_TEST needs the manifest to declare")
configure_case(declared_without FALSE "declares features.self_test, but SELF_TEST is not given")
configure_case(class_not_in_sources FALSE "the top-level package of CLASS 'other.module:Tiny'")
configure_case(extra_module FALSE "PYTHON_SOURCES hold helpers.py beside tiny/")
configure_case(extra_directory FALSE "PYTHON_SOURCES hold other/ beside tiny/")
configure_case(template_not_json FALSE "is not JSON")
configure_case(template_not_json_without_self_test FALSE "is not JSON")
configure_case(self_test_declared TRUE "configured manifest:")
configure_case(self_test_generated TRUE "configured manifest:")
# The generated manifest declares the self test, and requires praenora.python@0.0 first.
file(GLOB_RECURSE manifests LIST_DIRECTORIES false "${case_build}/*tiny.pmm.json")
if(NOT manifests)
    message(FATAL_ERROR "self_test_generated: no manifest tiny.pmm.json below ${case_build}")
endif()
list(GET manifests 0 manifest)
file(READ "${manifest}" text)
string(JSON declared GET "${text}" features self_test)
string(JSON service GET "${text}" requires services 0 name)
if(NOT declared OR NOT service STREQUAL "praenora.python")
    message(FATAL_ERROR "self_test_generated: ${manifest} does not declare features.self_test or "
        "does not require praenora.python first:\n${text}")
endif()
message(STATUS "self_test_generated: ${manifest} declares the self test")
