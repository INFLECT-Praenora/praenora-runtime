# Test python.provider_cmake: what praenora_add_python_provider()
# (cmake/PraenoraPythonProvider.cmake) decides before it builds anything, in script mode, without
# a project, Python or the option:
#   - the engine object it composes from ENGINE_JSON and the environment's object
#     (_praenora_python_provider_engine()): ENGINE_JSON's text as written, a line break and the
#     white space around it made one space (as praenora_add_provider() writes it), with
#     "environment": <the environment's object> after its last entry, byte for byte; a value that
#     holds " : ", text that is not ASCII and a number's spelling are kept;
#   - the JSON string literals of the environment files (_praenora_python_json_string()): quotes,
#     backslashes and every control character escaped, text that is not ASCII kept;
#   - its refusals: an ENGINE_JSON that is not a JSON object or has an "environment" of its own,
#     arguments it does not know, a keyword without a value, a required keyword missing,
#     PYTHON_ROOT without PYTHON_SOURCES, an absolute INSTALL_DESTINATION, and a call before the
#     Python core's targets exist. Each refusal stops a child process
#     (python_provider_refusal.cmake), whose message must say why;
#   - where praenora and praenora-module-host find the provider python's installed environment
#     file (praenora_python_environment_file_from(), cmake/PraenoraProvider.cmake): relative to
#     their installed directories, below the library directory lib, lib64, a multiarch one or an
#     absolute one below the prefix.
# Run as
#   cmake -DPRAENORA_SOURCE_DIR=<repo> -P check_python_provider.cmake

if(NOT PRAENORA_SOURCE_DIR)
    message(FATAL_ERROR "give -DPRAENORA_SOURCE_DIR")
endif()
include("${PRAENORA_SOURCE_DIR}/cmake/PraenoraPythonProvider.cmake")

function(check_equal what actual wanted)
    if(NOT actual STREQUAL wanted)
        message(FATAL_ERROR "${what}:\n  got    '${actual}'\n  wanted '${wanted}'")
    endif()
endfunction()

# The engine object.
string(CONCAT env "{\"lock_sha256\":\"0123\",\"name\":\"praenora-runtime-pyenv\","
    "\"package\":\"p\",\"version\":\"0.1.0\"}")
# Each engine object given, then the one composed from it with the environment's object `env`.
set(cases
    "{\"name\": \"test-python-echo-engine\", \"version\": \"1.0.0\"}"
    "{\"name\": \"test-python-echo-engine\", \"version\": \"1.0.0\", \"environment\": ${env}}"
    "{}"
    "{\"environment\": ${env}}"
    "{ }"
    "{\"environment\": ${env}}"
    "{\"a\" : \"x : y\", \"b\": \"déjà\", \"n\": 1.50, \"e\": 1e3}  "
    "{\"a\" : \"x : y\", \"b\": \"déjà\", \"n\": 1.50, \"e\": 1e3, \"environment\": ${env}}"
    "{\n  \"name\": \"x\",\n\t\"inner\": {\"environment\": 1}\n}\n"
    "{ \"name\": \"x\", \"inner\": {\"environment\": 1}, \"environment\": ${env}}")
list(LENGTH cases count)
math(EXPR last "${count} - 1")
foreach(index RANGE 0 ${last} 2)
    math(EXPR wanted_index "${index} + 1")
    list(GET cases ${index} engine)
    list(GET cases ${wanted_index} wanted)
    _praenora_python_provider_engine(composed "check" "${engine}" "${env}")
    check_equal("the engine object of '${engine}'" "${composed}" "${wanted}")
endforeach()

# JSON string literals.
string(ASCII 1 control_1)
string(ASCII 8 backspace)
string(ASCII 12 form_feed)
string(ASCII 31 control_31)
_praenora_python_json_string(literal
    "a\"b\\c/d\ne\rf\tg${backspace}h${form_feed}i${control_1}j${control_31}ké")
check_equal("a JSON string literal" "${literal}"
    "\"a\\\"b\\\\c/d\\ne\\rf\\tg\\bh\\fi\\u0001j\\u001fké\"")
_praenora_python_json_string(empty "")
check_equal("the empty JSON string literal" "${empty}" "\"\"")
_praenora_python_json_string(path "C:/Program Files/Prænora/lib")
check_equal("a path's JSON string literal" "${path}" "\"C:/Program Files/Prænora/lib\"")

# Refusals, each in a child process: message(FATAL_ERROR) ends the script that calls it.
# `definition` is the child's one -D argument; it may hold a list.
function(check_refused what expected definition)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPRAENORA_SOURCE_DIR=${PRAENORA_SOURCE_DIR}" "${definition}"
            -P "${PRAENORA_SOURCE_DIR}/tests/cmake/python_provider_refusal.cmake"
        RESULT_VARIABLE refused
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        ENCODING UTF-8)
    # CMake wraps the text of message(FATAL_ERROR); compare it with white space collapsed.
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${error}")
    if(refused EQUAL 0)
        message(FATAL_ERROR "${what} was not refused:\n${output}${error}")
    endif()
    string(FIND "${flat}" "${expected}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${what} was refused without saying '${expected}':\n${error}")
    endif()
endfunction()

check_refused("an engine that is a JSON array" "ENGINE_JSON is not a JSON object"
    "-DENGINE=[1]")
check_refused("an engine that is a JSON string" "ENGINE_JSON is not a JSON object"
    "-DENGINE=\"text\"")
check_refused("an engine that is not JSON" "ENGINE_JSON is not a JSON object"
    "-DENGINE={\"name\": ")
check_refused("an empty engine" "ENGINE_JSON is not a JSON object" "-DENGINE=")
check_refused("an engine with an environment of its own"
    "ENGINE_JSON has an \"environment\" of its own" "-DENGINE={\"environment\": {}}")

set(valid NAME p VERSION 1.0.0 SERVICES a.b@0.0 SOURCES p.cpp)
check_refused("an argument it does not know" "unexpected arguments: BOGUS"
    "-DARGUMENTS=BOGUS;${valid}")
check_refused("a keyword without a value" "no value given for NAME"
    "-DARGUMENTS=NAME;VERSION;1.0.0;SERVICES;a.b@0.0;SOURCES;p.cpp")
foreach(required IN ITEMS NAME VERSION SERVICES SOURCES)
    set(without ${valid})
    list(FIND without ${required} at)
    math(EXPR value_at "${at} + 1")
    list(REMOVE_AT without ${at} ${value_at})
    check_refused("a call without ${required}" "${required} is required"
        "-DARGUMENTS=${without}")
endforeach()
check_refused("PYTHON_ROOT without PYTHON_SOURCES" "PYTHON_ROOT needs PYTHON_SOURCES"
    "-DARGUMENTS=${valid};PYTHON_ROOT;python")
check_refused("an absolute INSTALL_DESTINATION" "INSTALL_DESTINATION must be relative"
    "-DARGUMENTS=${valid};INSTALL_DESTINATION;/opt/providers")
check_refused("a call before the Python core is made"
    "the target praenora_python_core does not exist" "-DARGUMENTS=${valid}")
# The installed environment file, from the tool's directory and the module host's
# (<libdir>/praenora/../../libexec/praenora, as src/module_host/CMakeLists.txt installs it).
set(CMAKE_INSTALL_PREFIX "/opt/praenora")
set(CMAKE_INSTALL_BINDIR bin)
set(environment_file "praenora/providers/python/python-environment.json")
foreach(case IN ITEMS "lib|../lib|../../lib" "lib64|../lib64|../../lib64"
                      "lib/x86_64-linux-gnu|../lib/x86_64-linux-gnu|../../x86_64-linux-gnu"
                      "/opt/praenora/lib64|../lib64|../../lib64")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 CMAKE_INSTALL_LIBDIR)
    list(GET case 1 from_tool)
    list(GET case 2 from_module_host)
    cmake_path(SET module_host_dir NORMALIZE
        "${CMAKE_INSTALL_LIBDIR}/praenora/../../libexec/praenora")
    praenora_python_environment_file_from(found "${CMAKE_INSTALL_BINDIR}")
    check_equal("the environment file from bin, libdir ${CMAKE_INSTALL_LIBDIR}" "${found}"
        "${from_tool}/${environment_file}")
    praenora_python_environment_file_from(found "${module_host_dir}")
    check_equal("the environment file from ${module_host_dir}, libdir ${CMAKE_INSTALL_LIBDIR}"
        "${found}" "${from_module_host}/${environment_file}")
endforeach()

message(STATUS "python provider checks passed")
