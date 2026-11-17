# Lists the symbols a provider library exports, with nm, and checks that the provider entry point
# is the only one: the driver of the providers.<name>.exports tests
# (tests/providers/CMakeLists.txt). Script mode, cmake -P, with these variables:
#
#   NM=<file>         nm (CMAKE_NM)
#   LIBRARY=<file>    the provider library
#   BINARY=<format>   macho (nm -gU lists the exports, with a leading '_') or elf
#                     (nm -D --defined-only lists them)
#
# nm's standard error is ignored; only the symbols it lists on standard output count.
cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS NM LIBRARY BINARY)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "check_exports.cmake: ${variable} is required")
    endif()
endforeach()

if(BINARY STREQUAL "macho")
    set(command "${NM}" -gU "${LIBRARY}")
    set(entry_point "_praenora_provider_get_api")
else()
    set(command "${NM}" -D --defined-only "${LIBRARY}")
    set(entry_point "praenora_provider_get_api")
endif()
praenora_test_run(NM "nm" ${command})
if(NOT NM_EXIT STREQUAL "0")
    praenora_test_fail("nm" "exit code ${NM_EXIT}, expected 0" "${command}" "${NM_EXIT}"
        "${NM_OUT}" "${NM_ERR}")
endif()

# Each line is "<address> <type> <name>"; the name is the last field.
string(REPLACE "\n" ";" lines "${NM_OUT}")
set(exported "")
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "^.* " "" name "${line}")
    list(APPEND exported "${name}")
endforeach()
if(NOT exported STREQUAL entry_point)
    list(LENGTH exported count)
    praenora_test_fail("${LIBRARY}"
        "exports ${count} symbols, expected only ${entry_point}" "${command}" "${NM_EXIT}"
        "${NM_OUT}" "${NM_ERR}")
endif()
message(STATUS "${LIBRARY} exports only ${entry_point}.")
