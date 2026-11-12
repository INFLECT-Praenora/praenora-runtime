# The examples: two modules and the workflow that connects them
# (examples/workflows/estimate-predict). The modules are built with praenora_add_module(), the way
# an external module project builds its modules, into ${PRAENORA_EXAMPLE_MODULES_DIR}/<name>/
# (the library, its manifest and its test vectors). Besides the module SDK they use nlohmann/json.
# They are compiled with -ffp-contract=off on GCC and Clang, so no multiply and add are fused into
# one rounding, which narrows the differences between platforms.

# praenora_example_modules builds every example module; the integration tests depend on it.
add_custom_target(praenora_example_modules)

# praenora_example_install(<name> <variable>): sets <variable> to the praenora_add_module()
# arguments that install example module <name>, or to nothing in a build without install rules.
# The helper then installs the library, its manifest and its test vectors into the installation's
# module directory, ${CMAKE_INSTALL_LIBDIR}/praenora/modules/<name>, in the install component
# Examples. A default installation includes them only with PRAENORA_INSTALL_EXAMPLES; without it,
# cmake --install <build> --component Examples installs them.
function(praenora_example_install name variable)
    set(arguments "")
    if(PRAENORA_INSTALL_RULES)
        set(arguments INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/praenora/modules/${name}"
            INSTALL_COMPONENT Examples)
        if(NOT PRAENORA_INSTALL_EXAMPLES)
            list(APPEND arguments INSTALL_EXCLUDE_FROM_ALL)
        endif()
    endif()
    set(${variable} "${arguments}" PARENT_SCOPE)
endfunction()

# praenora_example_module(<target>): the settings every example module shares.
function(praenora_example_module target)
    praenora_target_warnings(${target})
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
    add_dependencies(praenora_example_modules ${target})
endfunction()

add_subdirectory(modules/estimator)
add_subdirectory(modules/predictor)
