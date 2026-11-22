# praenora_target_warnings(<target>)
#
# Adds the project's warning flags to the sources of <target> (never to its consumers).
# PRAENORA_WARNINGS_AS_ERRORS turns the warnings into errors.

include_guard(GLOBAL)

function(praenora_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /utf-8
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->
            $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
            $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
            $<$<BOOL:${PRAENORA_WARNINGS_AS_ERRORS}>:/WX>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<BOOL:${PRAENORA_WARNINGS_AS_ERRORS}>:-Werror>)
        # GCC 13's -Wdangling-reference (in -Wall) reports every reference bound to what a call
        # returns when the call was given a temporary, whatever the reference refers to: a
        # provider that load_provider() keeps loaded for the life of the process, an entry of a
        # report that a helper looks up by a string literal. It is turned off, for C++ only.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
           AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
            target_compile_options(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-dangling-reference>)
        endif()
    endif()
endfunction()
