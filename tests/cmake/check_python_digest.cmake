# Test python.digest_cmake: praenora_python_directory_digest() (cmake/PraenoraPythonDigest.cmake)
# gives the value that the shell command below gives for tests/fixtures/python-digest, changes
# with one byte of a file, a file added or a name changed, and ignores everything below a
# __pycache__ directory. Run as
#   cmake -DPRAENORA_SOURCE_DIR=<repo> -DPRAENORA_WORK_DIR=<dir> -P check_python_digest.cmake
#
# The pinned value comes from (in the fixture's directory; sorted by bytes):
#   find . -type f -not -path '*/__pycache__/*' | LC_ALL=C sort | while IFS= read -r f; do
#     printf '%s  %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "${f#./}"; done | shasum -a 256
# The listing has three lines: a.py, pkg/__init__.py and pkg/data.txt (its content has an e acute).

if(NOT PRAENORA_SOURCE_DIR OR NOT PRAENORA_WORK_DIR)
    message(FATAL_ERROR "give -DPRAENORA_SOURCE_DIR and -DPRAENORA_WORK_DIR")
endif()
include("${PRAENORA_SOURCE_DIR}/cmake/PraenoraPythonDigest.cmake")

set(expected "6abd23f2aa8ccc218a422d41e7e218a08fa00ba40e42455d3895446393056a85")
set(fixture "${PRAENORA_SOURCE_DIR}/tests/fixtures/python-digest")

function(check_equal what actual wanted)
    if(NOT actual STREQUAL wanted)
        message(FATAL_ERROR "${what}: got ${actual}, wanted ${wanted}")
    endif()
endfunction()

function(check_differs what actual other)
    if(actual STREQUAL other)
        message(FATAL_ERROR "${what}: the digest did not change (${actual})")
    endif()
endfunction()

# The fixture itself, taking every file below the directory, and with the files named.
praenora_python_directory_digest(by_glob ROOT "${fixture}" GLOB_ROOT "${fixture}")
check_equal("digest of the fixture (GLOB_ROOT)" "${by_glob}" "${expected}")
string(LENGTH "${by_glob}" digest_length)
if(NOT by_glob MATCHES "^[0-9a-f]+$" OR NOT digest_length EQUAL 64)
    message(FATAL_ERROR "the digest '${by_glob}' is not 64 lowercase hexadecimal digits")
endif()

praenora_python_directory_digest(by_names ROOT "${fixture}"
    FILES pkg/data.txt a.py "${fixture}/pkg/__init__.py")
check_equal("digest of the fixture (FILES, relative and absolute, any order)"
    "${by_names}" "${expected}")

# A file below __pycache__ named in FILES is left out too.
praenora_python_directory_digest(with_cache ROOT "${fixture}"
    FILES a.py pkg/__init__.py pkg/data.txt pkg/__pycache__/x.pyc)
check_equal("digest with a __pycache__ file named" "${with_cache}" "${expected}")

# Work on a copy under the build tree.
set(copy "${PRAENORA_WORK_DIR}/python-digest")
file(REMOVE_RECURSE "${copy}")
file(MAKE_DIRECTORY "${copy}/pkg")
foreach(name IN ITEMS a.py pkg/__init__.py pkg/data.txt)
    file(COPY_FILE "${fixture}/${name}" "${copy}/${name}")
endforeach()
praenora_python_directory_digest(copied ROOT "${copy}" GLOB_ROOT "${copy}")
check_equal("digest of the copy" "${copied}" "${expected}")

# A file below __pycache__, at any depth, does not count.
file(MAKE_DIRECTORY "${copy}/pkg/__pycache__" "${copy}/pkg/sub/__pycache__")
file(COPY_FILE "${fixture}/a.py" "${copy}/pkg/__pycache__/y.pyc")
file(COPY_FILE "${fixture}/a.py" "${copy}/pkg/sub/__pycache__/z.pyc")
praenora_python_directory_digest(cached ROOT "${copy}" GLOB_ROOT "${copy}")
check_equal("digest with files below __pycache__" "${cached}" "${expected}")

# Another content for pkg/data.txt (copying a file over it keeps the bytes exact on every
# platform, which file(WRITE) does not).
file(COPY_FILE "${fixture}/pkg/__init__.py" "${copy}/pkg/data.txt")
praenora_python_directory_digest(edited ROOT "${copy}" GLOB_ROOT "${copy}")
check_differs("digest after pkg/data.txt changed" "${edited}" "${expected}")
file(COPY_FILE "${fixture}/pkg/data.txt" "${copy}/pkg/data.txt")
praenora_python_directory_digest(restored ROOT "${copy}" GLOB_ROOT "${copy}")
check_equal("digest after pkg/data.txt was restored" "${restored}" "${expected}")

# A file added, and a file renamed.
file(COPY_FILE "${fixture}/a.py" "${copy}/b.py")
praenora_python_directory_digest(added ROOT "${copy}" GLOB_ROOT "${copy}")
check_differs("digest after a file was added" "${added}" "${expected}")
file(RENAME "${copy}/b.py" "${copy}/c.py")
praenora_python_directory_digest(renamed ROOT "${copy}" GLOB_ROOT "${copy}")
check_differs("digest after the file was renamed" "${renamed}" "${added}")
file(REMOVE "${copy}/c.py")
praenora_python_directory_digest(removed ROOT "${copy}" GLOB_ROOT "${copy}")
check_equal("digest after the file was removed" "${removed}" "${expected}")

# Symbolic links are refused, to a file and to a directory (where a link can be made).
file(CREATE_LINK "${copy}/a.py" "${copy}/link.py" SYMBOLIC RESULT link_made)
if(link_made STREQUAL "0")
    file(CREATE_LINK "${copy}/pkg" "${copy}/linkdir" SYMBOLIC)
    foreach(arguments IN ITEMS "GLOB_ROOT;${copy}" "FILES;a.py;link.py" "FILES;linkdir/data.txt")
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                "-DPRAENORA_SOURCE_DIR=${PRAENORA_SOURCE_DIR}"
                "-DARGUMENTS=ROOT;${copy};${arguments}"
                -P "${PRAENORA_SOURCE_DIR}/tests/cmake/python_digest_refusal.cmake"
            RESULT_VARIABLE refused
            ERROR_VARIABLE refusal_message
            OUTPUT_QUIET
            ENCODING UTF-8)
        if(refused EQUAL 0)
            message(FATAL_ERROR "a symbolic link was not refused (${arguments})")
        endif()
        # CMake wraps the text of message(FATAL_ERROR); compare it with whitespace collapsed.
        string(REGEX REPLACE "[ \t\r\n]+" " " flat "${refusal_message}")
        if(NOT flat MATCHES "is a symbolic link; the digest covers regular files only")
            message(FATAL_ERROR "the refusal for ${arguments} does not name the link: "
                "${refusal_message}")
        endif()
    endforeach()
    # A link below a __pycache__ directory is left out like any file there.
    file(REMOVE "${copy}/link.py" "${copy}/linkdir")
    file(CREATE_LINK "${copy}/a.py" "${copy}/pkg/__pycache__/link.pyc" SYMBOLIC)
    praenora_python_directory_digest(linked ROOT "${copy}" GLOB_ROOT "${copy}")
    check_equal("digest with a link below __pycache__" "${linked}" "${expected}")
else()
    message(STATUS "symbolic links cannot be made here (${link_made}); links not checked")
endif()

# A directory named in FILES is refused (only the directories that GLOB_ROOT lists are skipped).
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPRAENORA_SOURCE_DIR=${PRAENORA_SOURCE_DIR}" "-DARGUMENTS=ROOT;${copy};FILES;a.py;pkg"
        -P "${PRAENORA_SOURCE_DIR}/tests/cmake/python_digest_refusal.cmake"
    RESULT_VARIABLE refused
    ERROR_VARIABLE refusal_message
    OUTPUT_QUIET
    ENCODING UTF-8)
string(REGEX REPLACE "[ \t\r\n]+" " " flat "${refusal_message}")
if(refused EQUAL 0 OR NOT flat MATCHES "pkg is not a file")
    message(FATAL_ERROR "a directory in FILES was not refused as not a file (${refused}): "
        "${refusal_message}")
endif()

# Refusals.
foreach(arguments IN ITEMS "ROOT;${copy}" "GLOB_ROOT;${copy}")
    # a call that lacks ROOT or both FILES and GLOB_ROOT stops the configuration; checked through
    # a child process, since message(FATAL_ERROR) ends this script
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPRAENORA_SOURCE_DIR=${PRAENORA_SOURCE_DIR}" "-DARGUMENTS=${arguments}"
            -P "${PRAENORA_SOURCE_DIR}/tests/cmake/python_digest_refusal.cmake"
        RESULT_VARIABLE refused
        OUTPUT_QUIET ERROR_QUIET
        ENCODING UTF-8)
    if(refused EQUAL 0)
        message(FATAL_ERROR "praenora_python_directory_digest(${arguments}) was not refused")
    endif()
endforeach()
message(STATUS "python digest checks passed")
