# Checks that the workflow .github/workflows/build.yml agrees with the pins of the dependency
# bundles and of the Python environment bundles and with the CI presets, and that it pins its
# actions and runner images: the driver of ci.workflow_consistency (tests/ci/CMakeLists.txt).
# Script mode, cmake -P, with WORKFLOW, PIN_FILE, PYENV_PIN_FILE and PRESETS set to the workflow,
# cmake/PraenoraRuntimeDepsPin.cmake, cmake/PraenoraRuntimePyenvPin.cmake and CMakePresets.json;
# it reads only these files and uses no network.
#
# It reads the workflow as text, line by line, in the format that the workflow's header comment
# states (CMake reads no YAML): each value it checks is on the same line as its key, unquoted and
# without a comment (but the version comment of a uses: line). It checks that:
#   1. each matrix entry is a line "- key: <key>" followed by a line "runner: <label>", and the
#      keys are those of PRAENORA_RUNTIME_DEPS_PLATFORMS in PIN_FILE, each once, in any order, and
#      those of PRAENORA_RUNTIME_PYENV_PLATFORMS in PYENV_PIN_FILE too: each job fetches both
#      bundles of its key;
#   2. each key has a configure, a build and a test preset ci-<key> in CMakePresets.json;
#   3. each entry's runner label is the one for its key's operating system, the key's first word:
#      ubuntu-24.04 for linux, macos-26 for macos and windows-2022 for windows, the runner images
#      of praenora-runtime-deps' workflow (docs/releasing.md); and each runs-on value is one of
#      these labels or ${{ matrix.runner }}, so never a -latest label;
#   4. each line "uses: <action>" (or "- uses: <action>") names the action at a commit, 40
#      hexadecimal digits in lower case, followed by a comment with the version of that commit:
#      "uses: <owner>/<name>@<commit> # v<version>";
#   5. no line "runs-on:", "uses:", "runner:" or "- key:" leaves its value to the lines after it,
#      where the checks above would not see it.
# Each check must find at least one line to check. Every check runs; each problem is printed on
# its own line, naming the file and its line, and the script then fails, giving the number of
# problems.
cmake_minimum_required(VERSION 3.24)

set(_script "check_workflow_consistency.cmake")
foreach(_variable IN ITEMS WORKFLOW PIN_FILE PYENV_PIN_FILE PRESETS)
    if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_script}: ${_variable} is not set")
    endif()
    if(NOT EXISTS "${${_variable}}" OR IS_DIRECTORY "${${_variable}}")
        message(FATAL_ERROR "${_script}: ${_variable} names no file: ${${_variable}}")
    endif()
endforeach()

# The runner label for each operating system of the keys.
set(_runner_linux "ubuntu-24.04")
set(_runner_macos "macos-26")
set(_runner_windows "windows-2022")
set(_runners "${_runner_linux}" "${_runner_macos}" "${_runner_windows}")

set(_problems 0)
# _problem(<text>...): reports one problem, the arguments joined.
function(_problem)
    string(CONCAT text ${ARGV})
    message("ci.workflow_consistency: ${text}")
    math(EXPR count "${_problems} + 1")
    set(_problems "${count}" PARENT_SCOPE)
endfunction()

# The pins: for each variable that names a pin file, the prefix of that file's variables.
set(_pins PIN_FILE PYENV_PIN_FILE)
set(_prefix_PIN_FILE PRAENORA_RUNTIME_DEPS)
set(_prefix_PYENV_PIN_FILE PRAENORA_RUNTIME_PYENV)
foreach(_file_variable IN LISTS _pins)
    set(_prefix "${_prefix_${_file_variable}}")
    include("${${_file_variable}}")
    if(NOT DEFINED ${_prefix}_PLATFORMS OR "${${_prefix}_PLATFORMS}" STREQUAL "")
        _problem("${${_file_variable}}: it sets no ${_prefix}_PLATFORMS")
    endif()
endforeach()

# The names of the presets of each kind.
file(READ "${PRESETS}" _presets_json)
foreach(_kind IN ITEMS configurePresets buildPresets testPresets)
    set(_presets_${_kind} "")
    string(JSON _count ERROR_VARIABLE _error LENGTH "${_presets_json}" ${_kind})
    if(NOT _error STREQUAL "NOTFOUND")
        _problem("${PRESETS}: no ${_kind}: ${_error}")
        continue()
    endif()
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_index RANGE ${_last})
            string(JSON _name GET "${_presets_json}" ${_kind} ${_index} name)
            list(APPEND _presets_${_kind} "${_name}")
        endforeach()
    endif()
endforeach()

# The workflow's lines, one list element each. A '\' would escape the ';' that ends its line,
# which would join the next line to it, and ';', '[' and ']' would split or join elements, so
# these four characters are replaced first (the '\' before the others, which the replacements do
# not hold); no value that is checked holds one.
file(READ "${WORKFLOW}" _text)
string(REPLACE "\\" "<backslash>" _text "${_text}")
string(REPLACE ";" "<semicolon>" _text "${_text}")
string(REPLACE "[" "<bracket>" _text "${_text}")
string(REPLACE "]" "<bracket>" _text "${_text}")
string(REPLACE "\n" ";" _lines "${_text}")

string(REPEAT "[0-9a-f]" 40 _commit)
set(_action "[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@${_commit} # v[0-9]+(\\.[0-9]+)*")
set(_matrix_runner "\${{ matrix.runner }}")
set(_keys "")
set(_uses 0)
set(_runs_on 0)
set(_number 0)
set(_expect_runner_for "")
foreach(_line IN LISTS _lines)
    math(EXPR _number "${_number} + 1")
    set(_where "${WORKFLOW}:${_number}")
    # A matrix entry's runner, on the line after its key.
    if(NOT _expect_runner_for STREQUAL "")
        set(_key "${_expect_runner_for}")
        set(_expect_runner_for "")
        if(NOT _line MATCHES "^ *runner: (.*)$")
            _problem("${_where}: the matrix entry of ${_key} has no line 'runner: <label>' after "
                "its key")
        else()
            set(_runner "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "-.*" "" _os "${_key}")
            if(NOT DEFINED _runner_${_os})
                _problem("${_where}: the key ${_key} names no operating system of linux, macos "
                    "and windows")
            elseif(NOT _runner STREQUAL _runner_${_os})
                _problem("${_where}: the runner of ${_key} is '${_runner}', not "
                    "'${_runner_${_os}}', the label for ${_os}")
            endif()
            continue()
        endif()
    endif()
    if(_line MATCHES "^ *(- )?(runs-on|uses|runner|- key): *$")
        _problem("${_where}: '${CMAKE_MATCH_2}:' has no value on its line, and this test reads "
            "only a value on the line of its key")
    elseif(_line MATCHES "^ *- key: (.*)$")
        set(_key "${CMAKE_MATCH_1}")
        if(_key IN_LIST _keys)
            _problem("${_where}: the key ${_key} is in the matrix twice")
        endif()
        list(APPEND _keys "${_key}")
        set(_expect_runner_for "${_key}")
        foreach(_kind IN ITEMS configurePresets buildPresets testPresets)
            if(NOT "ci-${_key}" IN_LIST _presets_${_kind})
                _problem("${_where}: ${PRESETS} has no ${_kind} entry ci-${_key}")
            endif()
        endforeach()
    elseif(_line MATCHES "^ *(- )?runner: (.*)$")
        _problem("${_where}: a runner label, ${CMAKE_MATCH_2}, on a line that does not follow a "
            "line '- key: <key>'")
    elseif(_line MATCHES "^ *runs-on: (.*)$")
        set(_value "${CMAKE_MATCH_1}")
        math(EXPR _runs_on "${_runs_on} + 1")
        if(NOT _value IN_LIST _runners AND NOT _value STREQUAL _matrix_runner)
            list(JOIN _runners ", " _labels)
            _problem("${_where}: runs-on is '${_value}', neither ${_matrix_runner} nor one of "
                "the labels ${_labels}")
        endif()
    elseif(_line MATCHES "^ *(- )?uses: (.*)$")
        set(_value "${CMAKE_MATCH_2}")
        math(EXPR _uses "${_uses} + 1")
        if(NOT _value MATCHES "^${_action}$")
            _problem("${_where}: '${_value}' is not pinned as <owner>/<name>@<commit> # "
                "v<version>, with a commit of 40 hexadecimal digits in lower case")
        endif()
    endif()
endforeach()
if(NOT _expect_runner_for STREQUAL "")
    _problem("${WORKFLOW}: the matrix entry of ${_expect_runner_for} has no line "
        "'runner: <label>' after its key")
endif()

# The keys against each pin.
if(_keys STREQUAL "")
    _problem("${WORKFLOW}: no matrix entry, a line '- key: <key>'")
else()
    set(_sorted_keys ${_keys})
    list(REMOVE_DUPLICATES _sorted_keys)
    list(SORT _sorted_keys)
    foreach(_file_variable IN LISTS _pins)
        set(_prefix "${_prefix_${_file_variable}}")
        set(_sorted_platforms ${${_prefix}_PLATFORMS})
        list(SORT _sorted_platforms)
        if(NOT _sorted_keys STREQUAL _sorted_platforms)
            list(JOIN _keys ", " _keys_text)
            list(JOIN ${_prefix}_PLATFORMS ", " _platforms_text)
            _problem("${WORKFLOW}: the matrix keys (${_keys_text}) are not the platforms of "
                "${_prefix}_PLATFORMS in ${${_file_variable}} (${_platforms_text})")
        endif()
    endforeach()
endif()
if(_runs_on EQUAL 0)
    _problem("${WORKFLOW}: no line 'runs-on: <label>'")
endif()
if(_uses EQUAL 0)
    _problem("${WORKFLOW}: no line 'uses: <action>'")
endif()

if(_problems GREATER 0)
    message(FATAL_ERROR "ci.workflow_consistency: ${_problems} problem(s)")
endif()
list(LENGTH _keys _key_count)
list(LENGTH _pins _pin_count)
message("ci.workflow_consistency: ${_key_count} matrix keys against ${_pin_count} pins, "
    "${_runs_on} runs-on values and ${_uses} actions checked")
