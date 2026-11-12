# Fixture modules for the contract, negative, lifecycle and performance tests. Each is built with
# praenora_add_module(), as an external module project would be, into
# ${PRAENORA_TEST_MODULES_DIR}/<name>/ (the library and its generated <name>.pmm.json), and
# declares the test capability test.echo@0.1.0 (fx-fail also test.quiet@0.1.0, whose output is
# optional). The C fixtures are C99; common/c_fixture.c holds the functions they share.
# fx-minimal-c is a correct module, fx-create-fail fails create() on purpose, and the other C
# fixtures each break one ABI or describe() rule on purpose. The others use the C++ adapter. All
# are compiled with the project's warnings.

# praenora_add_fixture_module(<name> SOURCES <source>... [LINK_LIBRARIES <library>...]
#                             [CAPABILITIES <id@version>...] [TEST_VECTORS <file>])
# Target <name> with '-' replaced by '_'; manifest template <name>/<name>.pmm.json.in. The
# capabilities default to test.echo@0.1.0. TEST_VECTORS (relative to <name>/) is copied beside the
# manifest, whose template names it with @PRAENORA_MODULE_TEST_VECTORS@.
function(praenora_add_fixture_module name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEST_VECTORS" "SOURCES;LINK_LIBRARIES;CAPABILITIES")
    string(REPLACE "-" "_" target "${name}")
    set(link_libraries "")
    if(DEFINED arg_LINK_LIBRARIES)
        set(link_libraries LINK_LIBRARIES ${arg_LINK_LIBRARIES})
    endif()
    set(test_vectors "")
    if(DEFINED arg_TEST_VECTORS)
        set(test_vectors TEST_VECTORS "${name}/${arg_TEST_VECTORS}")
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
        MANIFEST_TEMPLATE "${name}/${name}.pmm.json.in"
        OUTPUT_DIRECTORY "${PRAENORA_TEST_MODULES_DIR}/${name}"
        ${link_libraries}
        ${test_vectors})
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
