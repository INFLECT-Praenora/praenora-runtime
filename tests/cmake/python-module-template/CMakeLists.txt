# The tiny project of python.module_template_check (check_python_module_template.cmake): one call
# of praenora_add_python_module() from the source tree's cmake/, chosen by CASE, with what it needs
# of the module SDK's package defined here (the versions and an interface target
# Praenora::ModuleSDK). The project is only configured, never built.
#   self_test_lacking     SELF_TEST, a template without features.self_test: refused
#   declared_without      no SELF_TEST, a template that declares features.self_test: refused
#   self_test_declared    SELF_TEST, a template that declares it: accepted
#   self_test_generated   SELF_TEST, no template: accepted, the manifest declares it
#   class_not_in_sources  a CLASS whose top-level package is not among the sources: refused
#   extra_module          the sources hold a module helpers.py beside the package tiny: refused
#   extra_directory       the sources hold a directory other/ beside the package tiny: refused
#   template_not_json     a template that is not JSON, with SELF_TEST: refused, saying so
#   template_not_json_without_self_test  the same without SELF_TEST: refused, saying so
cmake_minimum_required(VERSION 3.24)
project(praenora_python_module_template LANGUAGES CXX)

if(NOT PRAENORA_SOURCE_DIR OR NOT CASE)
    message(FATAL_ERROR "give -DPRAENORA_SOURCE_DIR and -DCASE")
endif()
set(PRAENORA_ABI_MAJOR 0)
set(PRAENORA_ABI_MINOR 0)
set(PRAENORA_PMP_VERSION 0.1.0)
set(PRAENORA_SDK_VERSION 0.2.0)
set(PRAENORA_MODULE_MANIFEST_FORMAT_VERSION 0.2.0)
add_library(Praenora::ModuleSDK INTERFACE IMPORTED)
include("${PRAENORA_SOURCE_DIR}/cmake/PraenoraPythonModule.cmake")

set(arguments NAME tiny VERSION 0.1.0 CAPABILITIES test.echo@0.1.0 PYTHON_SOURCES tiny
    PYTHON_ROOT src)
if(CASE STREQUAL "self_test_lacking")
    praenora_add_python_module(tiny ${arguments} CLASS tiny:Tiny SELF_TEST
        MANIFEST_TEMPLATE lacks.pmm.json.in)
elseif(CASE STREQUAL "declared_without")
    praenora_add_python_module(tiny ${arguments} CLASS tiny:Tiny
        MANIFEST_TEMPLATE declares.pmm.json.in)
elseif(CASE STREQUAL "self_test_declared")
    praenora_add_python_module(tiny ${arguments} CLASS tiny:Tiny SELF_TEST
        MANIFEST_TEMPLATE declares.pmm.json.in)
elseif(CASE STREQUAL "self_test_generated")
    praenora_add_python_module(tiny ${arguments} CLASS tiny:Tiny SELF_TEST)
elseif(CASE STREQUAL "class_not_in_sources")
    praenora_add_python_module(tiny ${arguments} CLASS other.module:Tiny)
elseif(CASE STREQUAL "extra_module")
    praenora_add_python_module(tiny NAME tiny VERSION 0.1.0 CAPABILITIES test.echo@0.1.0
        PYTHON_SOURCES tiny helpers.py PYTHON_ROOT src CLASS tiny:Tiny)
elseif(CASE STREQUAL "extra_directory")
    praenora_add_python_module(tiny NAME tiny VERSION 0.1.0 CAPABILITIES test.echo@0.1.0
        PYTHON_SOURCES tiny other PYTHON_ROOT src CLASS tiny:Tiny)
elseif(CASE STREQUAL "template_not_json")
    praenora_add_python_module(tiny ${arguments} CLASS tiny:Tiny SELF_TEST
        MANIFEST_TEMPLATE not-json.pmm.json.in)
elseif(CASE STREQUAL "template_not_json_without_self_test")
    praenora_add_python_module(tiny ${arguments} CLASS tiny:Tiny
        MANIFEST_TEMPLATE not-json.pmm.json.in)
else()
    message(FATAL_ERROR "unknown CASE ${CASE}")
endif()
get_target_property(configured tiny PRAENORA_MODULE_CONFIGURED_MANIFEST)
message(STATUS "configured manifest: ${configured}")
