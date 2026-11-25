# Makes the archive of an installation with cpack and checks it: the driver of the test
# install.package (tests/install/CMakeLists.txt). Script mode, cmake -P, with these variables:
#
#   BUILD_DIR, SOURCE_DIR   the build tree to package and its source tree
#   WORK_DIR                scratch directory, emptied first
#   CPACK                   the cpack program that came with this build's CMake
#   CONFIG                  the configuration under test, which cpack -C names
#   VERSION                 the runtime package's version (PROJECT_VERSION)
#   LIBDIR, DATADIR         the build's GNUInstallDirs directories
#
# Steps, in order; the first failure ends the test, showing what ran and what it printed:
#   1. No source package: <BUILD_DIR> has no CPackSourceConfig.cmake at its top, so no target
#      package_source, and cpack --config <BUILD_DIR>/cmake/CPackSourceConfig.cmake -B
#      <WORK_DIR>/s stops, saying that there is no source package, and writes no archive.
#   2. cpack --config <BUILD_DIR>/CPackConfig.cmake -C <CONFIG> -B <WORK_DIR>. When CONFIG is not
#      Release or Debug (in any case), cpack must stop instead, saying that an archive holds one
#      configuration, Release or Debug, and write no archive; the test ends there.
#   3. <WORK_DIR> holds one archive, <name>.zip on a Windows host and <name>.tar.xz elsewhere,
#      <name> being praenora-runtime-<VERSION>-<os>-<arch>-<compiler>-<config> (os linux, macos or
#      windows; arch x86_64 or arm64; compiler appleclang, clang, gcc or msvc and a number; config
#      CONFIG in lower case), and <archive>.sha256 beside it, which holds exactly
#      "<SHA-256>  <archive>\n", the archive's SHA-256 in the format of sha256sum.
#   4. Unpacked, the archive holds one directory, <name>, whose files are those of
#      cmake --install <BUILD_DIR> --config <CONFIG> (into <WORK_DIR>/i) without the example
#      modules (<LIBDIR>/praenora/modules/), byte for byte, among them this project's LICENSE at
#      <DATADIR>/praenora/licenses/praenora-runtime/LICENSE; a symbolic link of the installation
#      (the Python environment has some) is a link in the archive, to the same target.
# When every step passed, the installation (<WORK_DIR>/i), the unpacked archive (<WORK_DIR>/u) and
# cpack's staging (<WORK_DIR>/_CPack_Packages, <WORK_DIR>/s), which with the Python environment
# hold about a gigabyte each, are removed; the archive and its .sha256 file are kept. A failing run
# keeps everything, for inspection, until the next run empties WORK_DIR.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS BUILD_DIR SOURCE_DIR WORK_DIR CPACK VERSION LIBDIR DATADIR)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "check_package.cmake: ${variable} is required")
    endif()
endforeach()
if(NOT DEFINED CONFIG)
    set(CONFIG "")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# _expect_refusal(<step> <command> <text> <dir>): the command, a cpack run whose output goes to
# <dir> (its -B), must fail with <text> in its output (CMake's wrapping undone) and write no
# archive there.
function(_expect_refusal step command text dir)
    praenora_test_run(cpack "${step}: cpack" "${command}")
    string(REGEX REPLACE "[ \n]+" " " output "${cpack_OUT} ${cpack_ERR}")
    if(cpack_EXIT STREQUAL "0" OR NOT output MATCHES "${text}")
        praenora_test_fail("${step}: cpack" "expected a failure that says '${text}'"
            "${command}" "${cpack_EXIT}" "${cpack_OUT}" "${cpack_ERR}")
    endif()
    file(GLOB archives LIST_DIRECTORIES false "${dir}/*.zip" "${dir}/*.tar.xz")
    if(archives)
        message(FATAL_ERROR "${step}: cpack stopped, but wrote ${archives}")
    endif()
endfunction()

# Step 1.
if(EXISTS "${BUILD_DIR}/CPackSourceConfig.cmake")
    message(FATAL_ERROR "step 1: ${BUILD_DIR}/CPackSourceConfig.cmake exists, so the build tree "
        "has a target package_source")
endif()
set(command "${CPACK}" --config "${BUILD_DIR}/cmake/CPackSourceConfig.cmake"
    -B "${WORK_DIR}/s")
_expect_refusal("step 1" "${command}" "there is no source package" "${WORK_DIR}/s")
message(STATUS "step 1: no target package_source, and cpack stops for the source configuration")

# Step 2.
set(command "${CPACK}" --config "${BUILD_DIR}/CPackConfig.cmake" -C "${CONFIG}" -B "${WORK_DIR}")
string(TOLOWER "${CONFIG}" label)
if(NOT label MATCHES "^(release|debug)$")
    set(refusal "an archive holds one configuration, Release or Debug")
    _expect_refusal("step 2" "${command}" "${refusal}" "${WORK_DIR}")
    message(STATUS "step 2: cpack -C '${CONFIG}' stops, writing no archive: ${refusal}")
    return()
endif()
praenora_test_run(cpack "step 2: cpack" "${command}")
if(NOT cpack_EXIT STREQUAL "0")
    praenora_test_fail("step 2: cpack" "exit code ${cpack_EXIT}, expected 0" "${command}"
        "${cpack_EXIT}" "${cpack_OUT}" "${cpack_ERR}")
endif()
message(STATUS "step 2: cpack -C ${CONFIG} made the archive")

# Step 3.
if(CMAKE_HOST_WIN32)
    set(extension ".zip")
else()
    set(extension ".tar.xz")
endif()
file(GLOB archives LIST_DIRECTORIES false RELATIVE "${WORK_DIR}"
    "${WORK_DIR}/*.zip" "${WORK_DIR}/*.tar.xz")
list(LENGTH archives count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "step 3: ${WORK_DIR} holds ${count} archives (${archives}), expected one")
endif()
string(REPLACE "." "\\." pattern "^praenora-runtime-${VERSION}-(linux|macos|windows)-")
string(REPLACE "." "\\." escaped_extension "${extension}")
string(APPEND pattern
    "(x86_64|arm64)-(appleclang|clang|gcc|msvc)[0-9]+-${label}${escaped_extension}$")
if(NOT archives MATCHES "${pattern}")
    message(FATAL_ERROR "step 3: the archive is named '${archives}', which does not match "
        "'${pattern}'")
endif()
string(LENGTH "${extension}" extension_length)
string(LENGTH "${archives}" archive_length)
math(EXPR name_length "${archive_length} - ${extension_length}")
string(SUBSTRING "${archives}" 0 ${name_length} name)
set(archive "${WORK_DIR}/${archives}")
if(NOT EXISTS "${archive}.sha256")
    message(FATAL_ERROR "step 3: ${archive}.sha256 is missing")
endif()
file(SHA256 "${archive}" hash)
file(READ "${archive}.sha256" checksum)
if(NOT checksum STREQUAL "${hash}  ${archives}\n")
    message(FATAL_ERROR "step 3: ${archive}.sha256 holds '${checksum}', expected "
        "'${hash}  ${archives}' and a line feed")
endif()
message(STATUS "step 3: ${archives}, and its .sha256 file: ${hash}")

# Step 4.
set(prefix "${WORK_DIR}/i")
set(command "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${prefix}")
praenora_test_run(install "step 4: cmake --install" "${command}")
if(NOT install_EXIT STREQUAL "0")
    praenora_test_fail("step 4: cmake --install" "exit code ${install_EXIT}, expected 0"
        "${command}" "${install_EXIT}" "${install_OUT}" "${install_ERR}")
endif()
file(GLOB_RECURSE expected LIST_DIRECTORIES false RELATIVE "${prefix}" "${prefix}/*")
list(FILTER expected EXCLUDE REGEX "^${LIBDIR}/praenora/modules/")
list(SORT expected)

set(unpacked "${WORK_DIR}/u")
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${unpacked}")
file(GLOB top LIST_DIRECTORIES true RELATIVE "${unpacked}" "${unpacked}/*")
if(NOT top STREQUAL name OR NOT IS_DIRECTORY "${unpacked}/${name}")
    message(FATAL_ERROR "step 4: the archive holds '${top}', expected the directory '${name}'")
endif()
file(GLOB_RECURSE found LIST_DIRECTORIES false RELATIVE "${unpacked}/${name}"
    "${unpacked}/${name}/*")
list(SORT found)
if(NOT found STREQUAL expected)
    string(REPLACE ";" "\n  " found "${found}")
    string(REPLACE ";" "\n  " expected "${expected}")
    message(FATAL_ERROR "step 4: the archive's files differ from the installation's.\n"
        "installed (without the example modules):\n  ${expected}\narchived:\n  ${found}\n")
endif()
set(links 0)
foreach(file IN LISTS expected)
    if(IS_SYMLINK "${prefix}/${file}")
        file(READ_SYMLINK "${prefix}/${file}" installed_target)
        set(archived_target "")
        if(IS_SYMLINK "${unpacked}/${name}/${file}")
            file(READ_SYMLINK "${unpacked}/${name}/${file}" archived_target)
        endif()
        if(NOT archived_target STREQUAL installed_target)
            message(FATAL_ERROR "step 4: the archive's ${file} is not a symbolic link to "
                "${installed_target}, as the installation's is")
        endif()
        math(EXPR links "${links} + 1")
        continue()
    endif()
    file(SHA256 "${prefix}/${file}" installed_hash)
    file(SHA256 "${unpacked}/${name}/${file}" archived_hash)
    if(NOT archived_hash STREQUAL installed_hash)
        message(FATAL_ERROR "step 4: the archive's ${file} differs from the installation's")
    endif()
endforeach()
set(license "${DATADIR}/praenora/licenses/praenora-runtime/LICENSE")
file(SHA256 "${SOURCE_DIR}/LICENSE" source_hash)
if(NOT EXISTS "${unpacked}/${name}/${license}")
    message(FATAL_ERROR "step 4: the archive holds no ${license}")
endif()
file(SHA256 "${unpacked}/${name}/${license}" archived_hash)
if(NOT archived_hash STREQUAL source_hash)
    message(FATAL_ERROR "step 4: the archive's ${license} differs from ${SOURCE_DIR}/LICENSE")
endif()
list(LENGTH found file_count)
message(STATUS "step 4: the archive holds ${name}/, with the ${file_count} files of the "
    "installation, without the example modules, byte for byte (${links} of them symbolic links), "
    "${license} among them")
foreach(scratch IN ITEMS i u _CPack_Packages s)
    file(REMOVE_RECURSE "${WORK_DIR}/${scratch}")
endforeach()
message(STATUS "the test passed: the installation, the unpacked archive and cpack's staging are "
    "removed from ${WORK_DIR}; ${archives} and its .sha256 file are kept (a failing run keeps "
    "everything)")
