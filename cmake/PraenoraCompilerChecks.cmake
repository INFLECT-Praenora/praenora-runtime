# Toolchain checks and platform settings.
#
# Configuration stops early below these compiler versions, which provide complete C++17 support,
# including <filesystem> without extra libraries: GCC 9, Clang 10, AppleClang 13, MSVC 19.29
# (Visual Studio 2019 16.10).

include_guard(GLOBAL)

function(_praenora_check_compiler_floor lang)
    set(id "${CMAKE_${lang}_COMPILER_ID}")
    set(version "${CMAKE_${lang}_COMPILER_VERSION}")
    if(id STREQUAL "GNU")
        set(floor 9)
    elseif(id STREQUAL "Clang")
        set(floor 10)
    elseif(id STREQUAL "AppleClang")
        set(floor 13)
    elseif(id STREQUAL "MSVC")
        set(floor 19.29)
    else()
        message(WARNING "Praenora: the ${lang} compiler '${id} ${version}' is not one of the "
            "supported compilers (GCC, Clang, AppleClang, MSVC); its version is not checked.")
        return()
    endif()
    if(version VERSION_LESS floor)
        message(FATAL_ERROR "Praenora requires ${id} ${floor} or newer as the ${lang} compiler; "
            "found ${id} ${version} (${CMAKE_${lang}_COMPILER}).")
    endif()
endfunction()

_praenora_check_compiler_floor(CXX)
_praenora_check_compiler_floor(C)
message(STATUS "Praenora: compilers ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (C++), "
    "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (C)")

# Windows 10 is the minimum Windows API level.
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# The platform being built for, in the vocabulary of module manifests:
# PRAENORA_PLATFORM_OS is linux|macos|windows, PRAENORA_PLATFORM_ARCH is x86_64|arm64, and
# PRAENORA_PLATFORM is "<os>-<arch>".
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PRAENORA_PLATFORM_OS "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PRAENORA_PLATFORM_OS "macos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PRAENORA_PLATFORM_OS "windows")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" PRAENORA_PLATFORM_OS)
endif()

list(LENGTH CMAKE_OSX_ARCHITECTURES _praenora_arch_count)
if(_praenora_arch_count GREATER 1)
    message(FATAL_ERROR "Praenora builds one architecture at a time; "
        "CMAKE_OSX_ARCHITECTURES is '${CMAKE_OSX_ARCHITECTURES}'.")
endif()
# The architecture comes from the first source that is set: the compiler's own report (always
# for MSVC), the Visual Studio platform, CMAKE_OSX_ARCHITECTURES, then the target processor.
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
    set(_praenora_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
elseif(CMAKE_VS_PLATFORM_NAME)
    set(_praenora_arch "${CMAKE_VS_PLATFORM_NAME}")
elseif(CMAKE_OSX_ARCHITECTURES)
    set(_praenora_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(_praenora_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_praenora_arch}" _praenora_arch)
if(_praenora_arch MATCHES "^(arm64|aarch64)$")
    set(PRAENORA_PLATFORM_ARCH "arm64")
elseif(_praenora_arch MATCHES "^(x86_64|amd64|x64)$")
    set(PRAENORA_PLATFORM_ARCH "x86_64")
else()
    set(PRAENORA_PLATFORM_ARCH "${_praenora_arch}")
endif()
set(PRAENORA_PLATFORM "${PRAENORA_PLATFORM_OS}-${PRAENORA_PLATFORM_ARCH}")
message(STATUS "Praenora: platform ${PRAENORA_PLATFORM}")
