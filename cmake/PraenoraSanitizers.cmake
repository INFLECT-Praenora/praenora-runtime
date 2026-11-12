# PRAENORA_SANITIZERS, for example "address;undefined" or "thread", becomes global compile and link
# flags, so every target of the build, modules included, is instrumented the same way. With
# "undefined", the first UndefinedBehaviorSanitizer report ends the process with an error
# (-fno-sanitize-recover=undefined), as the other sanitizers' reports do, so a test that triggers
# undefined behaviour fails. The flags are also published: PRAENORA_SANITIZER_COMPILE_OPTIONS and
# PRAENORA_SANITIZER_LINK_OPTIONS (lists, empty without sanitizers), which the install test passes
# on to the projects it builds against the installation.

include_guard(GLOBAL)

set(PRAENORA_SANITIZER_COMPILE_OPTIONS "")
set(PRAENORA_SANITIZER_LINK_OPTIONS "")
if(PRAENORA_SANITIZERS)
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "PRAENORA_SANITIZERS needs GCC or Clang; "
            "the C++ compiler is ${CMAKE_CXX_COMPILER_ID}.")
    endif()
    if("thread" IN_LIST PRAENORA_SANITIZERS
       AND ("address" IN_LIST PRAENORA_SANITIZERS OR "leak" IN_LIST PRAENORA_SANITIZERS))
        message(FATAL_ERROR "The thread sanitizer cannot be combined with the address or leak "
            "sanitizers (PRAENORA_SANITIZERS is '${PRAENORA_SANITIZERS}').")
    endif()
    list(JOIN PRAENORA_SANITIZERS "," _praenora_sanitizer_list)
    set(PRAENORA_SANITIZER_COMPILE_OPTIONS
        -fsanitize=${_praenora_sanitizer_list} -fno-omit-frame-pointer)
    if("undefined" IN_LIST PRAENORA_SANITIZERS)
        list(APPEND PRAENORA_SANITIZER_COMPILE_OPTIONS -fno-sanitize-recover=undefined)
    endif()
    set(PRAENORA_SANITIZER_LINK_OPTIONS -fsanitize=${_praenora_sanitizer_list})
    add_compile_options(${PRAENORA_SANITIZER_COMPILE_OPTIONS})
    add_link_options(${PRAENORA_SANITIZER_LINK_OPTIONS})
    message(STATUS "Praenora: sanitizers ${_praenora_sanitizer_list}")
endif()
