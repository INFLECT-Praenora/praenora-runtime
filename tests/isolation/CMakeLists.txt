# Isolation tests (label isolation): child processes and their channel, and module instances in a
# module host (praenora-module-host), white-box, with the fixture modules; the service providers
# of their modules, loaded in the module host, with the fixture providers (provider_test.cpp); and
# the compiled models their module hosts share (compiled_model_test.cpp); writes to a module host
# that stops reading (deaf_module_host_test.cpp); and the sandbox profiles of a module host
# (sandbox_test.cpp). On Windows, where process isolation is not available in this version, only
# the check that it is refused is built.
#
# Four helper programs, built beside the tests: praenora_isolation_parent_probe starts a module
# host and is killed by Isolation.AModuleHostWhoseRuntimeDiesEnds; praenora_fake_module_host is put
# where the runtime looks for its module host, to answer the handshake as no module host of this
# build does; praenora_isolation_short_host_probe starts test-store, in a process of its own, with a
# provider host too short for the shared compiled models
# (Isolation.AProviderHostTooShortForTheStoreOffersNone); and praenora_isolation_restricted_probe,
# started in a sandbox of its own, loads a module in a module host under the restricted profile,
# which a sandboxed process cannot apply (Isolation.ARestrictedModuleHostOfASandboxedRuntime...).
# The tests find them through compile definitions.
if(WIN32)
    praenora_add_gtest(praenora_isolation_tests LABEL isolation WHITEBOX
        SOURCES isolation_test.cpp isolation_support.hpp)
    add_dependencies(praenora_isolation_tests praenora_test_modules)
    return()
endif()

add_executable(praenora_isolation_parent_probe parent_probe.cpp)
target_link_libraries(praenora_isolation_parent_probe PRIVATE
    praenora_runtime_objects
    praenora_runtime_deps
    praenora_whitebox_support
    praenora_test_support)
target_compile_definitions(praenora_isolation_parent_probe PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_isolation_parent_probe)

add_executable(praenora_fake_module_host fake_module_host.cpp)
target_link_libraries(praenora_fake_module_host PRIVATE
    praenora_runtime_objects
    praenora_runtime_deps)
target_compile_definitions(praenora_fake_module_host PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_fake_module_host)

add_executable(praenora_isolation_short_host_probe short_host_probe.cpp)
target_link_libraries(praenora_isolation_short_host_probe PRIVATE
    praenora_runtime_objects
    praenora_runtime_deps
    praenora_test_support
    ${CMAKE_DL_LIBS})
target_compile_definitions(praenora_isolation_short_host_probe PRIVATE
    PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_isolation_short_host_probe)

add_executable(praenora_isolation_restricted_probe restricted_probe.cpp)
target_link_libraries(praenora_isolation_restricted_probe PRIVATE
    praenora_runtime_objects
    praenora_runtime_deps
    praenora_whitebox_support
    praenora_test_support)
target_compile_definitions(praenora_isolation_restricted_probe PRIVATE
    PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_isolation_restricted_probe)

praenora_add_gtest(praenora_isolation_tests LABEL isolation WHITEBOX SANDBOX_SKIP
    SOURCES
        child_process_test.cpp
        compiled_model_test.cpp
        deaf_module_host_test.cpp
        isolation_test.cpp
        provider_test.cpp
        sandbox_test.cpp
        isolation_support.hpp)
target_compile_definitions(praenora_isolation_tests PRIVATE
    "PRAENORA_TEST_PARENT_PROBE=\"$<TARGET_FILE:praenora_isolation_parent_probe>\""
    "PRAENORA_TEST_FAKE_MODULE_HOST=\"$<TARGET_FILE:praenora_fake_module_host>\""
    "PRAENORA_TEST_SHORT_HOST_PROBE=\"$<TARGET_FILE:praenora_isolation_short_host_probe>\""
    "PRAENORA_TEST_RESTRICTED_PROBE=\"$<TARGET_FILE:praenora_isolation_restricted_probe>\"")
add_dependencies(praenora_isolation_tests
    praenora_test_modules
    praenora_test_providers
    praenora_module_host
    praenora_isolation_parent_probe
    praenora_fake_module_host
    praenora_isolation_short_host_probe
    praenora_isolation_restricted_probe)
# With the roadrunner provider: its saved states in the shared compiled models
# (compiled_model_test.cpp). PRAENORA_TEST_ROADRUNNER_MANIFEST names its manifest.
if(PRAENORA_WITH_ROADRUNNER)
    set(_praenora_roadrunner_manifest
        "$<TARGET_PROPERTY:praenora_provider_roadrunner,PRAENORA_PROVIDER_MANIFEST>")
    target_compile_definitions(praenora_isolation_tests PRIVATE PRAENORA_TEST_WITH_ROADRUNNER
        "PRAENORA_TEST_ROADRUNNER_MANIFEST=\"${_praenora_roadrunner_manifest}\"")
    add_dependencies(praenora_isolation_tests praenora_provider_roadrunner)
endif()
