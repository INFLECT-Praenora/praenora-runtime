# Fixture service providers for the provider tests. Each is built with praenora_add_provider()
# into ${PRAENORA_TEST_PROVIDERS_DIR}/<name>/ (the library and its generated <name>.provider.json).
# test-echo is the correct provider of the test service test.echo@0.0 (tests/support/test_echo.h).
# test-simulation is the reference provider of the simulation service praenora.simulation@0.0
# (include/praenora/sdk/simulation.h) for the toy model format "test-decay"; the service checks
# (tests/contract/simulation_service_checks.hpp) run against it. The others are built from
# variants/variant_provider.c: those with a FIXTURE_* definition, all but test-reordered, break one
# rule of the provider interface on purpose; test-conflict, test-retry and test-order have none and
# are correct. test-simulation is C++17 (with nlohmann/json); the others are C99. All are compiled
# with the project's warnings.

# praenora_add_fixture_provider(<name> SOURCES <source>... [SERVICES <service>@<major>.<minor>...]
#                               [DEFINITIONS <definition>...] [LINK_LIBRARIES <library>...])
# Target <name> with '-' replaced by '_', version 0.1.0, engine {"name":"<name>-engine",
# "version":"1.0.0"}. The services default to test.echo@0.0.
function(praenora_add_fixture_provider name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;SERVICES;DEFINITIONS;LINK_LIBRARIES")
    string(REPLACE "-" "_" target "${name}")
    set(services test.echo@0.0)
    if(DEFINED arg_SERVICES)
        set(services ${arg_SERVICES})
    endif()
    set(options "")
    if(DEFINED arg_LINK_LIBRARIES)
        list(APPEND options LINK_LIBRARIES ${arg_LINK_LIBRARIES})
    endif()
    praenora_add_provider(${target}
        NAME ${name}
        VERSION 0.1.0
        SERVICES ${services}
        ENGINE_JSON "{\"name\": \"${name}-engine\", \"version\": \"1.0.0\"}"
        SOURCES ${arg_SOURCES} common/provider_fixture.h common/provider_fixture.c
        OUTPUT_DIRECTORY "${PRAENORA_TEST_PROVIDERS_DIR}/${name}"
        ${options})
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${PROJECT_SOURCE_DIR}/tests/support") # test_echo.h
    if(DEFINED arg_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${arg_DEFINITIONS})
    endif()
    praenora_target_warnings(${target})
    add_dependencies(praenora_test_providers ${target})
endfunction()

# praenora_test_providers builds every fixture provider; test executables that load them depend on
# it.
add_custom_target(praenora_test_providers)

praenora_add_fixture_provider(test-echo SOURCES test-echo/test_echo_provider.c)
praenora_add_fixture_provider(test-simulation
    SOURCES test-simulation/test_simulation_provider.cpp
    SERVICES praenora.simulation@0.0
    LINK_LIBRARIES nlohmann_json::nlohmann_json)

set(variant variants/variant_provider.c)
praenora_add_fixture_provider(test-null-table SOURCES ${variant} DEFINITIONS FIXTURE_NULL_TABLE)
praenora_add_fixture_provider(test-table-abi-major
    SOURCES ${variant} DEFINITIONS FIXTURE_TABLE_ABI_MAJOR)
praenora_add_fixture_provider(test-table-reserved
    SOURCES ${variant} DEFINITIONS FIXTURE_TABLE_RESERVED)
praenora_add_fixture_provider(test-table-short SOURCES ${variant} DEFINITIONS FIXTURE_TABLE_SHORT)
praenora_add_fixture_provider(test-table-null-start
    SOURCES ${variant} DEFINITIONS FIXTURE_TABLE_NULL_START)
praenora_add_fixture_provider(test-identity-mismatch
    SOURCES ${variant} DEFINITIONS FIXTURE_IDENTITY_MISMATCH)
praenora_add_fixture_provider(test-describe-not-json
    SOURCES ${variant} DEFINITIONS FIXTURE_DESCRIBE_NOT_JSON)
praenora_add_fixture_provider(test-start-fails SOURCES ${variant} DEFINITIONS FIXTURE_START_FAILS)
praenora_add_fixture_provider(test-missing-service
    SOURCES ${variant} DEFINITIONS FIXTURE_MISSING_SERVICE
    SERVICES test.echo@0.0 test.absent@0.0)
praenora_add_fixture_provider(test-reordered
    SOURCES ${variant} DEFINITIONS FIXTURE_REORDERED
    SERVICES test.echo@0.0 test.other@1.2)
# Correct providers, each used by one test of what a library that failed a check leaves behind.
praenora_add_fixture_provider(test-conflict SOURCES ${variant})
praenora_add_fixture_provider(test-retry SOURCES ${variant})
# A correct provider used only by the test that a run starts its providers before any module.
praenora_add_fixture_provider(test-order SOURCES ${variant})
