# Performance probe (label perf; report-only). praenora_perf (perf_probe.cpp) runs the test
# workflow test.noop-chain, four test.echo steps in a chain, each bound to the fixture module
# fx-noop, for a number of ticks, and prints one JSON object with what the runtime costs per
# module invocation and per tick. CTest runs it twice for 2000 ticks, with payload validation
# (perf.validation_strict) and without (perf.validation_off), so the two reports show what
# validation costs. run_perf.cmake fails a test only when the probe exits with a nonzero code or
# does not print its JSON object; timings never fail it, since shared machines are noisy.
# Turning validation off is an internal engine setting, so the probe links the runtime's object
# library, as the white-box tests do, and praenora_test_support for the locations of the fixture
# modules and the test catalog and for its scratch directory.
add_executable(praenora_perf perf_probe.cpp)
target_link_libraries(praenora_perf PRIVATE
    praenora_runtime_objects
    praenora_runtime_deps
    praenora_test_support)
target_compile_definitions(praenora_perf PRIVATE PRAENORA_RUNTIME_STATIC_DEFINE)
praenora_target_warnings(praenora_perf)
praenora_copy_runtime_dlls(praenora_perf)
add_dependencies(praenora_perf fx_noop)

foreach(_praenora_validation IN ITEMS strict off)
    set(_praenora_test perf.validation_${_praenora_validation})
    add_test(NAME ${_praenora_test}
        COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:praenora_perf>"
            -DTICKS=2000
            -DVALIDATION=${_praenora_validation}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_perf.cmake")
    # _praenora_test_environment (tests/CMakeLists.txt): what every test of a macOS
    # AddressSanitizer build needs.
    set_tests_properties(${_praenora_test} PROPERTIES
        LABELS perf
        TIMEOUT 600
        ${_praenora_test_environment})
endforeach()
