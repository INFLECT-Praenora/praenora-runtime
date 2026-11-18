# Fixture modules for the contract, negative, lifecycle and performance tests. Each is built with
# praenora_add_module(), as an external module project would be, into
# ${PRAENORA_TEST_MODULES_DIR}/<name>/ (the library and its generated <name>.pmm.json), and
# declares the test capability test.echo@0.1.0 (fx-fail also test.quiet@0.1.0, whose output is
# optional). The C fixtures are C99; common/c_fixture.c holds the functions they share.
# fx-minimal-c, fx-self-test, fx-other-sdk, fx-other-protocol and fx-negative-zero-abi are correct
# modules, fx-create-fail fails create() and fx-self-test-fails its self-test on purpose, and the
# other C fixtures each break one ABI or describe() rule on purpose. The others use the C++
# adapter. All are compiled with the project's warnings.

# praenora_add_fixture_module(<name> SOURCES <source>... [LINK_LIBRARIES <library>...]
#                             [CAPABILITIES <id@version>...] [TEST_VECTORS <file>]
#                             [REQUIRES_SERVICES <service>...] [OPTIONAL_SERVICES <service>...])
# Target <name> with '-' replaced by '_'; manifest template <name>/<name>.pmm.json.in when that
# file exists, otherwise the manifest praenora_add_module() generates. The capabilities default to
# test.echo@0.1.0. TEST_VECTORS (relative to <name>/) is copied beside the manifest, whose template
# names it with @PRAENORA_MODULE_TEST_VECTORS@. REQUIRES_SERVICES and OPTIONAL_SERVICES are passed
# on to praenora_add_module().
function(praenora_add_fixture_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEST_VECTORS"
        "SOURCES;LINK_LIBRARIES;CAPABILITIES;REQUIRES_SERVICES;OPTIONAL_SERVICES")
    string(REPLACE "-" "_" target "${name}")
    set(options "")
    if(DEFINED arg_LINK_LIBRARIES)
        list(APPEND options LINK_LIBRARIES ${arg_LINK_LIBRARIES})
    endif()
    if(DEFINED arg_TEST_VECTORS)
        list(APPEND options TEST_VECTORS "${name}/${arg_TEST_VECTORS}")
    endif()
    foreach(services IN ITEMS REQUIRES_SERVICES OPTIONAL_SERVICES)
        if(DEFINED arg_${services})
            list(APPEND options ${services} ${arg_${services}})
        endif()
    endforeach()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/${name}.pmm.json.in")
        list(APPEND options MANIFEST_TEMPLATE "${name}/${name}.pmm.json.in")
    endif()
    set(capabilities test.echo@0.1.0)
    if(DEFINED arg_CAPABILITIES)
        set(capabilities ${arg_CAPABILITIES})
    endif()
    praenora_add_module(${target}
        NAME ${name}
        VERSION 0.1.0
        CAPABILITIES ${capabilities}
        SOURCES ${arg_SOURCES}
        OUTPUT_DIRECTORY "${PRAENORA_TEST_MODULES_DIR}/${name}"
        ${options})
    praenora_target_warnings(${target})
    add_dependencies(praenora_test_modules ${target})
endfunction()

# praenora_test_modules builds every fixture module; test executables that load them depend on it.
add_custom_target(praenora_test_modules)

set(c_fixture common/c_fixture.h common/c_fixture.c)

# C, with hand-written function tables.
praenora_add_fixture_module(fx-minimal-c SOURCES fx-minimal-c/fx_minimal_c.c ${c_fixture})
praenora_add_fixture_module(fx-no-entry SOURCES fx-no-entry/fx_no_entry.c)
praenora_add_fixture_module(fx-null-table SOURCES fx-null-table/fx_null_table.c)
praenora_add_fixture_module(fx-abi-major SOURCES fx-abi-major/fx_abi_major.c ${c_fixture})
praenora_add_fixture_module(fx-short-table SOURCES fx-short-table/fx_short_table.c ${c_fixture})
praenora_add_fixture_module(fx-missing-fn SOURCES fx-missing-fn/fx_missing_fn.c ${c_fixture})
praenora_add_fixture_module(fx-identity-mismatch
    SOURCES fx-identity-mismatch/fx_identity_mismatch.c ${c_fixture})
praenora_add_fixture_module(fx-create-fail SOURCES fx-create-fail/fx_create_fail.c ${c_fixture})
praenora_add_fixture_module(fx-reserved-field
    SOURCES fx-reserved-field/fx_reserved_field.c ${c_fixture})
praenora_add_fixture_module(fx-describe-not-json
    SOURCES fx-describe-not-json/fx_describe_not_json.c ${c_fixture})
praenora_add_fixture_module(fx-describe-invalid
    SOURCES fx-describe-invalid/fx_describe_invalid.c ${c_fixture})
praenora_add_fixture_module(fx-describe-overflow
    SOURCES fx-describe-overflow/fx_describe_overflow.c ${c_fixture})
# describe() and the manifest report another SDK than the one the module is built with; one source,
# the second built to report an SDK version that cannot be compared.
praenora_add_fixture_module(fx-other-sdk SOURCES fx-other-sdk/fx_other_sdk.c ${c_fixture})
praenora_add_fixture_module(fx-sdk-overflow SOURCES fx-other-sdk/fx_other_sdk.c ${c_fixture})
target_compile_definitions(fx_sdk_overflow PRIVATE FX_SDK_OVERFLOW)
# describe() and the manifest report another ABI minor and PMP version than the SDK's; one source,
# the second built to report a newer PMP version than its manifest, the third one that cannot be
# compared, the fourth to write its ABI 0.0 as -0 and -0 in describe().
praenora_add_fixture_module(fx-other-protocol
    SOURCES fx-other-protocol/fx_other_protocol.c ${c_fixture})
praenora_add_fixture_module(fx-newer-pmp SOURCES fx-other-protocol/fx_other_protocol.c ${c_fixture})
target_compile_definitions(fx_newer_pmp PRIVATE FX_NEWER_PMP)
praenora_add_fixture_module(fx-pmp-overflow
    SOURCES fx-other-protocol/fx_other_protocol.c ${c_fixture})
target_compile_definitions(fx_pmp_overflow PRIVATE FX_PMP_OVERFLOW)
praenora_add_fixture_module(fx-negative-zero-abi
    SOURCES fx-other-protocol/fx_other_protocol.c ${c_fixture})
target_compile_definitions(fx_negative_zero_abi PRIVATE FX_NEGATIVE_ZERO_ABI)
# Correct modules with self_test; one source, the second built to report a failure.
praenora_add_fixture_module(fx-self-test SOURCES fx-self-test/fx_self_test.c ${c_fixture})
praenora_add_fixture_module(fx-self-test-fails SOURCES fx-self-test/fx_self_test.c ${c_fixture})
target_compile_definitions(fx_self_test_fails PRIVATE FX_SELF_TEST_FAILS)

# C++, with the adapter.
praenora_add_fixture_module(fx-echo
    SOURCES fx-echo/fx_echo.cpp LINK_LIBRARIES nlohmann_json::nlohmann_json)
praenora_add_fixture_module(fx-fail
    SOURCES fx-fail/fx_fail.cpp LINK_LIBRARIES nlohmann_json::nlohmann_json
    CAPABILITIES test.echo@0.1.0 test.quiet@0.1.0)
praenora_add_fixture_module(fx-slow
    SOURCES fx-slow/fx_slow.cpp LINK_LIBRARIES nlohmann_json::nlohmann_json)
praenora_add_fixture_module(fx-hang
    SOURCES fx-hang/fx_hang.cpp LINK_LIBRARIES nlohmann_json::nlohmann_json)
target_include_directories(fx_hang PRIVATE "${PROJECT_SOURCE_DIR}/tests/support") # test_gate.h
praenora_add_fixture_module(fx-lifecycle SOURCES fx-lifecycle/fx_lifecycle.cpp)
praenora_add_fixture_module(fx-noop SOURCES fx-noop/fx_noop.cpp)
praenora_add_fixture_module(fx-nondeterministic
    SOURCES fx-nondeterministic/fx_nondeterministic.cpp
    LINK_LIBRARIES nlohmann_json::nlohmann_json
    TEST_VECTORS vectors.json)
# fx-echo's code with a manifest whose parameters_schema requires a parameter, and test vectors
# whose case uses a capability the module does not implement.
praenora_add_fixture_module(fx-needs-parameter
    SOURCES fx-echo/fx_echo.cpp LINK_LIBRARIES nlohmann_json::nlohmann_json
    TEST_VECTORS invalid-vectors.json)

# Modules that use runtime services, all built from fx-service's code (the services
# test.echo@0.0, which the fixture provider test-echo offers, and test.other@1.2, which
# test-reordered offers too): fx-service requires test.echo@0.0 and can use test.other@1.0
# (manifest template with @PRAENORA_MODULE_REQUIRES_JSON@); fx-service-optional can use
# test.echo@0.0; fx-service-minor requires test.echo@0.7, a minor no fixture provider offers (both
# with the generated manifest); fx-service-format-0-1 has a manifest of format 0.1, which declares
# no services.
set(service_fixture SOURCES fx-service/fx_service.cpp LINK_LIBRARIES nlohmann_json::nlohmann_json)
praenora_add_fixture_module(fx-service ${service_fixture}
    REQUIRES_SERVICES test.echo@0.0 OPTIONAL_SERVICES test.other@1.0)
praenora_add_fixture_module(fx-service-optional ${service_fixture}
    OPTIONAL_SERVICES test.echo@0.0)
praenora_add_fixture_module(fx-service-minor ${service_fixture}
    REQUIRES_SERVICES test.echo@0.7)
praenora_add_fixture_module(fx-service-format-0-1 ${service_fixture})
# test_echo.h
foreach(target IN ITEMS fx_service fx_service_optional fx_service_minor fx_service_format_0_1)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
endforeach()

# simulating-module requires the simulation service praenora.simulation@0.0, which the fixture
# provider test-simulation offers for the toy model format "test-decay"; its test vectors run the
# service through the contract checks.
praenora_add_fixture_module(simulating-module
    SOURCES simulating-module/simulating_module.cpp
    LINK_LIBRARIES nlohmann_json::nlohmann_json
    TEST_VECTORS vectors.json
    REQUIRES_SERVICES praenora.simulation@0.0)

# converting-module requires the model conversion service praenora.model-conversion@0.0, which the
# fixture provider test-conversion offers for the toy formats "test-decay" and "test-decay-text",
# and can use the simulation service to simulate what it converted; its test vectors run both
# services through the contract checks.
praenora_add_fixture_module(converting-module
    SOURCES converting-module/converting_module.cpp
    LINK_LIBRARIES nlohmann_json::nlohmann_json
    TEST_VECTORS vectors.json
    REQUIRES_SERVICES praenora.model-conversion@0.0
    OPTIONAL_SERVICES praenora.simulation@0.0)
