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
    endif()
endfunction()
