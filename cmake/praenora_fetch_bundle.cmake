# The implementation that the bundle fetch scripts share: praenora_fetch_runtime_deps.cmake (the
# dependency bundles of praenora-runtime-deps) and praenora_fetch_runtime_pyenv.cmake (the Python
# environment bundles of praenora-runtime-pyenv) each include it and call praenora_fetch_bundle()
# once, in script mode; the project's build does not include it. It needs only CMake 3.24 or newer.
#
#   praenora_fetch_bundle(SCRIPT <name> SCRIPT_FILE <file> PREFIX <prefix> MANIFEST <file name>
#                         DEFAULT_PIN_FILE <file>)
#
# - SCRIPT: the calling script's file name, which begins each message.
# - SCRIPT_FILE: the calling script's path, which the usage in an error names.
# - PREFIX: the prefix of the pin file's variables, such as PRAENORA_RUNTIME_DEPS, and the name of
#   the variable that the printed line sets.
# - MANIFEST: the file name of the bundle's manifest, at the top of the bundle, such as
#   praenora-runtime-deps.json.
# - DEFAULT_PIN_FILE: the pin file read when PIN_FILE is not set.
#
# It reads the variables that the calling script was given with -D:
#
# - PLATFORM: a key of <PREFIX>_PLATFORMS in the pin file, such as macos-arm64-release.
# - DESTINATION: the directory that receives the archive and the bundle, made when it does not
#   exist. A relative path is relative to the current directory.
# - PIN_FILE: the pin file, a relative path being relative to the current directory; by default
#   DEFAULT_PIN_FILE (the tests ci.fetch_runtime_deps and ci.fetch_runtime_pyenv give their own).
# - BASE_URL: where the archives are, taken as given, with /<archive> appended; by default the
#   pinned release, <repository>/releases/download/v<version>. A file:// URL takes the absolute
#   path as it is: a space or a non-ASCII character in it needs no percent-encoding (checked on
#   macOS with CMake 4.4).
#
# The pin file sets <PREFIX>_VERSION (the release, v<version>), <PREFIX>_REPOSITORY,
# <PREFIX>_PLATFORMS (the platform keys) and, for each key, with <KEY> the key in upper case and
# each '-' as '_', <PREFIX>_<KEY>_ARCHIVE (the archive's file name in the release) and
# <PREFIX>_<KEY>_SHA256 (its SHA-256).
#
# It downloads <BASE_URL>/<archive> into DESTINATION, with TLS certificates verified (a transfer
# that fails, or stalls for 120 seconds, is tried again, up to 3 attempts in all, 3 seconds apart;
# a file with another SHA-256 is never tried again), checks the file's SHA-256 against the pin,
# unpacks it into <DESTINATION>/<name>/, <name> being the archive's name without .tar.xz or .zip,
# and checks that the bundle's manifest, <name>/<MANIFEST>, names the pinned version (its member
# version). It then prints one line to standard output, <PREFIX>=<bundle directory>, an absolute
# path, and, when the environment variable GITHUB_ENV names a file (as in a step of a GitHub
# Actions job, whose later steps then have the variable), appends the same line to that file.
# Every other message goes to standard error.
#
# A later run with the same DESTINATION reuses what an earlier one left. The archive kept in
# DESTINATION is hashed again and used when it has the pinned SHA-256; otherwise it is removed and
# downloaded again. The bundle is used as it is when the kept archive had the pinned SHA-256 and
# the bundle's manifest names the pinned version; otherwise it is removed and unpacked again. An
# archive is downloaded to <archive>.download and renamed once its SHA-256 is checked, and unpacked
# into a staging directory .u-<id>/ of DESTINATION, <id> being the first 8 hexadecimal digits of
# the SHA-256 of <name>, and moved to <name>/ once its manifest is checked. So a run that fails or
# is stopped leaves no archive or bundle that a later run would take for a checked one, and the
# next run removes what it left. The staging directory's name is short, so the paths while
# unpacking are only 12 characters longer than the bundle's (Windows limits paths to 260
# characters unless long paths are enabled).
#
# Each error stops the script with message(FATAL_ERROR), so cmake exits with 1: PLATFORM or
# DESTINATION not set; a pin file that does not exist or lacks a value; a PLATFORM that the pin
# file does not name (the message lists those it does); a download that failed 3 times (the
# message gives the last status); an archive whose SHA-256 is not the pinned one (the message gives
# both); an archive without <name>/<MANIFEST>, or whose manifest names another version than the
# pin (the message gives both).

include_guard(GLOBAL)

# _praenora_fetch_manifest_version(<variable> <directory> <manifest>): the version that the
# manifest <manifest> in <directory> names, or empty when there is no manifest or it names no
# version.
function(_praenora_fetch_manifest_version variable directory manifest)
    set(version "")
    if(EXISTS "${directory}/${manifest}" AND NOT IS_DIRECTORY "${directory}/${manifest}")
        file(READ "${directory}/${manifest}" json)
        string(JSON version ERROR_VARIABLE error GET "${json}" version)
        if(NOT error STREQUAL "NOTFOUND")
            set(version "")
        endif()
    endif()
    set(${variable} "${version}" PARENT_SCOPE)
endfunction()

function(praenora_fetch_bundle)
    cmake_parse_arguments(PARSE_ARGV 0 _arg ""
        "SCRIPT;SCRIPT_FILE;PREFIX;MANIFEST;DEFAULT_PIN_FILE" "")
    foreach(_argument IN ITEMS SCRIPT SCRIPT_FILE PREFIX MANIFEST DEFAULT_PIN_FILE)
        if(NOT DEFINED _arg_${_argument} OR _arg_${_argument} STREQUAL "")
            message(FATAL_ERROR "praenora_fetch_bundle(): ${_argument} is not given")
        endif()
    endforeach()
    if(DEFINED _arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "praenora_fetch_bundle(): unknown arguments "
            "'${_arg_UNPARSED_ARGUMENTS}'")
    endif()
    set(_script "${_arg_SCRIPT}")
    set(_prefix "${_arg_PREFIX}")
    set(_manifest "${_arg_MANIFEST}")

    # The pin file.
    if(NOT DEFINED PIN_FILE OR PIN_FILE STREQUAL "")
        set(PIN_FILE "${_arg_DEFAULT_PIN_FILE}")
    endif()
    # In script mode, CMAKE_CURRENT_SOURCE_DIR is the current directory.
    cmake_path(SET _pin_file NORMALIZE "${PIN_FILE}")
    cmake_path(ABSOLUTE_PATH _pin_file BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    if(NOT EXISTS "${_pin_file}" OR IS_DIRECTORY "${_pin_file}")
        message(FATAL_ERROR "${_script}: the pin file ${_pin_file} does not exist")
    endif()
    include("${_pin_file}")
    foreach(_variable IN ITEMS ${_prefix}_VERSION ${_prefix}_REPOSITORY ${_prefix}_PLATFORMS)
        if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
            message(FATAL_ERROR "${_script}: the pin file ${_pin_file} does not set ${_variable}")
        endif()
    endforeach()
    set(_version_pinned "${${_prefix}_VERSION}")
    set(_platforms_pinned "${${_prefix}_PLATFORMS}")
    list(JOIN _platforms_pinned ", " _platforms)

    # The arguments.
    foreach(_variable IN ITEMS PLATFORM DESTINATION)
        if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
            message(FATAL_ERROR "${_script}: ${_variable} is not set. Run it as cmake "
                "-DPLATFORM=<key> -DDESTINATION=<dir> -P ${_arg_SCRIPT_FILE}, with one of the "
                "keys ${_platforms}.")
        endif()
    endforeach()
    if(NOT PLATFORM IN_LIST _platforms_pinned)
        message(FATAL_ERROR "${_script}: the pin file names no platform '${PLATFORM}'; its "
            "platforms are ${_platforms}.\n"
            " pin file: ${_pin_file}")
    endif()
    string(TOUPPER "${PLATFORM}" _key)
    string(REPLACE "-" "_" _key "${_key}")
    set(_archive "${${_prefix}_${_key}_ARCHIVE}")
    string(TOLOWER "${${_prefix}_${_key}_SHA256}" _expected_sha256)
    if(NOT _archive MATCHES "^([A-Za-z0-9._-]+)\\.(tar\\.xz|zip)$")
        message(FATAL_ERROR "${_script}: ${_prefix}_${_key}_ARCHIVE in ${_pin_file} is "
            "'${_archive}', not a file name that ends in .tar.xz or .zip")
    endif()
    set(_name "${CMAKE_MATCH_1}")
    string(LENGTH "${_expected_sha256}" _length)
    if(NOT _expected_sha256 MATCHES "^[0-9a-f]+$" OR NOT _length EQUAL 64)
        message(FATAL_ERROR "${_script}: ${_prefix}_${_key}_SHA256 in ${_pin_file} is "
            "'${_expected_sha256}', not a SHA-256 of 64 hexadecimal digits")
    endif()
    if(NOT DEFINED BASE_URL OR BASE_URL STREQUAL "")
        string(CONCAT BASE_URL "${${_prefix}_REPOSITORY}/releases/download/"
            "v${_version_pinned}")
    endif()
    set(_url "${BASE_URL}/${_archive}")

    # The paths, absolute, with '/' as the separator.
    cmake_path(SET _destination NORMALIZE "${DESTINATION}")
    cmake_path(ABSOLUTE_PATH _destination BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    cmake_path(APPEND _destination "${_archive}" OUTPUT_VARIABLE _archive_file)
    cmake_path(APPEND _destination "${_name}" OUTPUT_VARIABLE _bundle)
    set(_download_file "${_archive_file}.download")
    string(SHA256 _id "${_name}")
    string(SUBSTRING "${_id}" 0 8 _id)
    cmake_path(APPEND _destination ".u-${_id}" OUTPUT_VARIABLE _unpack_dir)
    # The download's attempts, the seconds between two of them, and the seconds of no data after
    # which an attempt fails.
    set(_download_attempts 3)
    set(_download_pause 3)
    set(_download_inactivity_timeout 120)

    file(MAKE_DIRECTORY "${_destination}")
    # What a run that failed or was stopped may have left.
    file(REMOVE "${_download_file}")
    file(REMOVE_RECURSE "${_unpack_dir}")

    # The archive: kept from an earlier run when it has the pinned SHA-256, otherwise downloaded.
    set(_archive_kept FALSE)
    if(EXISTS "${_archive_file}" AND NOT IS_DIRECTORY "${_archive_file}")
        file(SHA256 "${_archive_file}" _kept_sha256)
        if(_kept_sha256 STREQUAL _expected_sha256)
            set(_archive_kept TRUE)
            message("${_script}: using ${_archive_file}, kept from an earlier run: it has the "
                "pinned SHA256")
        else()
            message("${_script}: removing ${_archive_file}, kept from an earlier run: its SHA256 "
                "is ${_kept_sha256}, not the pinned ${_expected_sha256}")
            file(REMOVE "${_archive_file}")
        endif()
    endif()
    if(NOT _archive_kept)
        # Only a transfer that fails is tried again; a file with another SHA-256 fails below.
        foreach(_attempt RANGE 1 ${_download_attempts})
            message("${_script}: downloading ${_url} (attempt ${_attempt} of "
                "${_download_attempts})")
            file(DOWNLOAD "${_url}" "${_download_file}" STATUS _status TLS_VERIFY ON
                INACTIVITY_TIMEOUT ${_download_inactivity_timeout})
            list(GET _status 0 _code)
            if(_code EQUAL 0)
                break()
            endif()
            string(REGEX REPLACE "^[0-9]+;" "" _reason "${_status}")
            file(REMOVE "${_download_file}")
            if(_attempt LESS _download_attempts)
                message("${_script}: the download failed (${_code}: ${_reason}); trying again in "
                    "${_download_pause} seconds")
                execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${_download_pause}
                    ENCODING UTF-8)
            endif()
        endforeach()
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "${_script}: the download failed ${_download_attempts} times, the "
                "last time with status ${_code}: ${_reason}.\n"
                " URL: ${_url}")
        endif()
        # Checked here rather than with file(DOWNLOAD EXPECTED_HASH), whose error names no
        # algorithm and leaves the file.
        file(SHA256 "${_download_file}" _actual_sha256)
        if(NOT _actual_sha256 STREQUAL _expected_sha256)
            file(REMOVE "${_download_file}")
            message(FATAL_ERROR "${_script}: the archive does not have the SHA256 that the pin "
                "file gives it.\n"
                " URL:             ${_url}\n"
                " pin file:        ${_pin_file}\n"
                " expected SHA256: ${_expected_sha256}\n"
                " actual SHA256:   ${_actual_sha256}")
        endif()
        file(RENAME "${_download_file}" "${_archive_file}")
    endif()

    # The bundle: used as it is when the archive was kept and its manifest names the pinned
    # version, otherwise unpacked again.
    set(_bundle_kept FALSE)
    if(_archive_kept AND IS_DIRECTORY "${_bundle}")
        _praenora_fetch_manifest_version(_version "${_bundle}" "${_manifest}")
        if(_version STREQUAL _version_pinned)
            set(_bundle_kept TRUE)
            message("${_script}: using ${_bundle}, unpacked by an earlier run: its manifest names "
                "version ${_version}")
        endif()
    endif()
    if(NOT _bundle_kept)
        file(REMOVE_RECURSE "${_bundle}")
        message("${_script}: unpacking ${_archive_file}")
        file(ARCHIVE_EXTRACT INPUT "${_archive_file}" DESTINATION "${_unpack_dir}")
        if(NOT EXISTS "${_unpack_dir}/${_name}/${_manifest}")
            file(REMOVE_RECURSE "${_unpack_dir}")
            message(FATAL_ERROR "${_script}: the archive holds no ${_name}/${_manifest}, so it is "
                "not the bundle ${_name}.\n"
                " archive: ${_archive_file}")
        endif()
        _praenora_fetch_manifest_version(_version "${_unpack_dir}/${_name}" "${_manifest}")
        if(NOT _version STREQUAL _version_pinned)
            file(REMOVE_RECURSE "${_unpack_dir}")
            if(_version STREQUAL "")
                set(_version "none")
            endif()
            message(FATAL_ERROR "${_script}: the archive's manifest, ${_name}/${_manifest}, names "
                "version ${_version}, but the pin file names version "
                "${_version_pinned}.\n"
                " archive:  ${_archive_file}\n"
                " pin file: ${_pin_file}")
        endif()
        file(RENAME "${_unpack_dir}/${_name}" "${_bundle}")
        file(REMOVE_RECURSE "${_unpack_dir}")
    endif()

    set(_line "${_prefix}=${_bundle}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_line}" ENCODING UTF-8)
    if(DEFINED ENV{GITHUB_ENV} AND NOT "$ENV{GITHUB_ENV}" STREQUAL "")
        file(APPEND "$ENV{GITHUB_ENV}" "${_line}\n")
        message("${_script}: appended the line ${_line} to $ENV{GITHUB_ENV}")
    endif()
endfunction()
