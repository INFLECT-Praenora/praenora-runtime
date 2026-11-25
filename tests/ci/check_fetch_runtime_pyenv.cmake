# Checks a fetch script built on cmake/praenora_fetch_bundle.cmake, which downloads, checks and
# unpacks the bundle that a pin file names for a platform: the driver of ci.fetch_runtime_pyenv
# (tests/ci/CMakeLists.txt), which runs it for cmake/praenora_fetch_runtime_pyenv.cmake. Script
# mode, cmake -P; it needs only CMake and uses no network: the script downloads from file:// URLs.
#
# Every name that belongs to one script comes from a -D variable, so that the same driver can
# check either fetch script, cmake/praenora_fetch_runtime_deps.cmake too, with a change of
# tests/ci/CMakeLists.txt only:
#   NAME            the test's name, which begins each report, such as ci.fetch_runtime_pyenv;
#   SCRIPT          the fetch script;
#   PIN_NAME        the file name of the script's own pin file, beside SCRIPT, such as
#                   PraenoraRuntimePyenvPin.cmake;
#   PREFIX          the prefix of the pin file's variables and the name of the variable that the
#                   printed line sets, such as PRAENORA_RUNTIME_PYENV;
#   MANIFEST        the file name of the bundle's manifest, such as praenora-runtime-pyenv.json;
#   STEM            the start of the bundles' names, such as praenora-runtime-pyenv;
#   OTHER_PREFIX, OTHER_MANIFEST
#                   the same of the other fetch script, whose pin file and manifest this script must
#                   refuse, such as PRAENORA_RUNTIME_DEPS and praenora-runtime-deps.json;
#   WORK            an absolute directory of the build tree, which it deletes and fills.
#
# It makes two fake bundles in WORK: <STEM>-9.9.9-test-release/, which holds a manifest
# <MANIFEST>, {"version":"9.9.9"}, and <STEM>-9.9.9-other-release/, which holds the other
# script's manifest, <OTHER_MANIFEST>, of the same version, and no other. It archives the first as
# .tar.xz and as .zip and the second as .tar.xz into "repository é/releases/download/v9.9.9/", laid
# out like the release v9.9.9 of a repository file://<WORK>/repository é (so every URL holds a
# space and a non-ASCII character, as they are), and writes three pin files: pin.cmake, of version
# 9.9.9 and that repository, with the platforms
#   fake-tar    the .tar.xz archive of the first bundle, with its SHA-256,
#   fake-zip    the .zip archive of the first bundle, with its SHA-256,
#   fake-bad    the .tar.xz archive of the first bundle, with a wrong SHA-256,
#   fake-other  the archive of the second bundle, with its SHA-256;
# pin-9.9.8.cmake, of version 9.9.8, whose only platform, fake-tar, is the same archive; and
# pin-other.cmake, the first pin file with <OTHER_PREFIX>_ for <PREFIX>_, as the other script's pin
# file would set them.
#
# Then it runs the script through tests/support/run_command.cmake and checks its exit code, its
# standard output, its standard error where it fails, and the files it leaves. GITHUB_ENV is unset
# in every case but the first two, which set it to a file of WORK: a run of the tests in a GitHub
# Actions job must not append to the job's own file.
#   tar          fake-tar into "dest dir é", BASE_URL the release's directory, GITHUB_ENV a file
#                that holds a line already: exit code 0; the standard output is exactly the line
#                <PREFIX>=<WORK>/dest dir é/<STEM>-9.9.9-test-release, the file holds its old line
#                and then this one, the bundle holds its manifest and the archive is kept beside it.
#   zip          fake-zip into "zip dest é", without BASE_URL (so from the pin's repository),
#                GITHUB_ENV a file that does not exist: the same, and the file holds only the line.
#   rerun        fake-tar into "dest dir é" again, BASE_URL a directory without archives, after a
#                file was added to the bundle: the same line, and the file is still there (the
#                kept archive and the bundle were used; nothing was downloaded or unpacked).
#   reunpack     the same, after the bundle's manifest was made to name version 0.0.0: the bundle
#                was unpacked again from the kept archive (its manifest names 9.9.9 again, and the
#                added file is gone).
#   stale        fake-tar into "dest dir é" once more, after the kept archive was overwritten with
#                garbage and the bundle removed: exit code 0, the archive was downloaded again (it
#                has the pinned SHA-256) and unpacked.
#   leftovers-kept
#                fake-tar into "dest dir é" once more, BASE_URL a directory without archives, after
#                the leftovers of a stopped run were planted there: a partial
#                <archive>.download, and the staging directory .u-<id>/ (<id> the first 8
#                hexadecimal digits of the SHA-256 of the bundle's name) holding a bundle with an
#                added file: the same line, and the destination holds only the bundle and the
#                archive (the leftovers are removed; the kept archive and the bundle are used).
#   leftovers    the same leftovers planted into a new destination, "leftovers dest", BASE_URL the
#                release's directory: the archive is downloaded and unpacked, the bundle holds no
#                added file, and the destination holds only the bundle and the archive.
#   relative     fake-zip into "relative dest", a path relative to the current directory, WORK:
#                the line names <WORK>/relative dest/<STEM>-9.9.9-test-release, an absolute path.
#   bad          fake-bad: a nonzero exit code after one attempt; the standard error says SHA256
#                and gives both values, and the destination holds nothing.
#   retry        fake-tar, BASE_URL a directory without archives: a nonzero exit code after 3
#                attempts, 3 seconds apart; the standard error says so, with the last status, and
#                the destination holds nothing.
#   unknown      PLATFORM nope: a nonzero exit code; the standard error lists the four platforms.
#   manifest     fake-tar of pin-9.9.8.cmake: a nonzero exit code; the standard error names both
#                versions, and the destination holds only the archive.
#   other-manifest
#                fake-other: a nonzero exit code; the standard error names the missing manifest,
#                <STEM>-9.9.9-other-release/<MANIFEST> (the other script's manifest is not this
#                script's), and the destination holds only the archive.
#   other-pin    fake-tar of pin-other.cmake: a nonzero exit code; the standard error says that the
#                pin file does not set <PREFIX>_VERSION, and nothing is downloaded.
#   no-pin-file  PIN_FILE a file that does not exist: a nonzero exit code; the standard error names
#                it.
#   no-platform, no-destination
#                PLATFORM, then DESTINATION, not set: a nonzero exit code; the standard error says
#                which, and its usage names SCRIPT (not the shared implementation).
#   pin          each platform of the script's own pin file, <PIN_NAME> beside SCRIPT, without
#                PIN_FILE (so that one), BASE_URL a directory that holds a file of garbage under
#                each pinned archive name: the script accepts the pinned archive name and SHA-256,
#                downloads the file and fails at its SHA-256, giving the pinned one, and the
#                destination holds nothing.
# The standard error is searched with each run of spaces and line feeds in it read as one space:
# message() wraps its lines.
cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS NAME SCRIPT PIN_NAME PREFIX MANIFEST STEM OTHER_PREFIX OTHER_MANIFEST
                          WORK)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "check_fetch_runtime_pyenv.cmake: ${variable} is not set")
    endif()
endforeach()
if(NOT IS_ABSOLUTE "${WORK}")
    message(FATAL_ERROR "check_fetch_runtime_pyenv.cmake: WORK must be an absolute path")
endif()
if(PREFIX STREQUAL OTHER_PREFIX OR MANIFEST STREQUAL OTHER_MANIFEST)
    message(FATAL_ERROR "check_fetch_runtime_pyenv.cmake: OTHER_PREFIX and OTHER_MANIFEST must "
        "differ from PREFIX and MANIFEST")
endif()
# The script's runs inherit this environment.
unset(ENV{GITHUB_ENV})

set(what "${NAME}")
set(prefix "${PREFIX}")
set(bundle_name "${STEM}-9.9.9-test-release")
set(other_name "${STEM}-9.9.9-other-release")
set(manifest_name "${MANIFEST}")
set(manifest_text "{\"version\":\"9.9.9\"}\n")

file(REMOVE_RECURSE "${WORK}")
set(bundles "${WORK}/bundles")
# The é is a fixture choice (real use downloads from https:// URLs); drop it if Windows fails.
set(repository "${WORK}/repository é")
set(release "${repository}/releases/download/v9.9.9")
file(MAKE_DIRECTORY "${bundles}/${bundle_name}" "${bundles}/${other_name}" "${release}")
file(WRITE "${bundles}/${bundle_name}/${manifest_name}" "${manifest_text}")
file(WRITE "${bundles}/${bundle_name}/README.txt" "A fake bundle of the test ${what}.\n")
file(WRITE "${bundles}/${other_name}/${OTHER_MANIFEST}" "${manifest_text}")

# _archive(<file> <directory> <mode> [<option>...]): archives <directory> of bundles/ into <file> of
# the release's directory, with cmake -E tar <mode> and the options, run in bundles/ so that the
# archive holds <directory>/ at its top.
function(_archive file directory mode)
    set(command "${CMAKE_COMMAND}" -E chdir "${bundles}"
        "${CMAKE_COMMAND}" -E tar "${mode}" "${release}/${file}" ${ARGN} -- "${directory}")
    praenora_test_run(ARCHIVE "${what}: cmake -E tar" "${command}")
    if(NOT ARCHIVE_EXIT STREQUAL "0")
        praenora_test_fail("${what}" "cmake -E tar did not make ${file}" "${command}"
            "${ARCHIVE_EXIT}" "${ARCHIVE_OUT}" "${ARCHIVE_ERR}")
    endif()
endfunction()

_archive("${bundle_name}.tar.xz" "${bundle_name}" cJf)
_archive("${bundle_name}.zip" "${bundle_name}" cf --format=zip)
_archive("${other_name}.tar.xz" "${other_name}" cJf)
file(SHA256 "${release}/${bundle_name}.tar.xz" tar_sha256)
file(SHA256 "${release}/${bundle_name}.zip" zip_sha256)
file(SHA256 "${release}/${other_name}.tar.xz" other_sha256)
set(wrong_sha256 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")

# file:// and the absolute path; before a Windows drive letter, file:///.
if(WORK MATCHES "^/")
    set(file_url "file://")
else()
    set(file_url "file:///")
endif()
set(repository_url "${file_url}${repository}")
set(release_url "${file_url}${release}")

# The pin files. The repository's URL is a bracket argument, which takes any path as it is.
set(pin "${WORK}/pin.cmake")
string(CONCAT pin_text
    "set(${prefix}_VERSION \"9.9.9\")\n"
    "set(${prefix}_REPOSITORY [==[${repository_url}]==])\n"
    "set(${prefix}_PLATFORMS \"fake-tar;fake-zip;fake-bad;fake-other\")\n"
    "set(${prefix}_FAKE_TAR_ARCHIVE \"${bundle_name}.tar.xz\")\n"
    "set(${prefix}_FAKE_TAR_SHA256 \"${tar_sha256}\")\n"
    "set(${prefix}_FAKE_ZIP_ARCHIVE \"${bundle_name}.zip\")\n"
    "set(${prefix}_FAKE_ZIP_SHA256 \"${zip_sha256}\")\n"
    "set(${prefix}_FAKE_BAD_ARCHIVE \"${bundle_name}.tar.xz\")\n"
    "set(${prefix}_FAKE_BAD_SHA256 \"${wrong_sha256}\")\n"
    "set(${prefix}_FAKE_OTHER_ARCHIVE \"${other_name}.tar.xz\")\n"
    "set(${prefix}_FAKE_OTHER_SHA256 \"${other_sha256}\")\n")
file(WRITE "${pin}" "${pin_text}")
set(pin_other "${WORK}/pin-other.cmake")
string(REPLACE "${prefix}_" "${OTHER_PREFIX}_" pin_other_text "${pin_text}")
file(WRITE "${pin_other}" "${pin_other_text}")
set(pin_9_9_8 "${WORK}/pin-9.9.8.cmake")
string(CONCAT pin_text
    "set(${prefix}_VERSION \"9.9.8\")\n"
    "set(${prefix}_REPOSITORY [==[${repository_url}]==])\n"
    "set(${prefix}_PLATFORMS \"fake-tar\")\n"
    "set(${prefix}_FAKE_TAR_ARCHIVE \"${bundle_name}.tar.xz\")\n"
    "set(${prefix}_FAKE_TAR_SHA256 \"${tar_sha256}\")\n")
file(WRITE "${pin_9_9_8}" "${pin_text}")

# _fetch(<case>): runs `command`, a ;-list, through praenora_test_run() and sets case, exit_code,
# out and err in the caller's scope, for _fail() and the checks below.
function(_fetch case_name)
    praenora_test_run(FETCH "${what}, case ${case_name}" "${command}")
    set(case "${case_name}" PARENT_SCOPE)
    set(exit_code "${FETCH_EXIT}" PARENT_SCOPE)
    set(out "${FETCH_OUT}" PARENT_SCOPE)
    set(err "${FETCH_ERR}" PARENT_SCOPE)
endfunction()

# _fail(<problem>): fails the test with <problem> and the report of the last run.
function(_fail problem)
    praenora_test_fail("${what}, case ${case}" "${problem}" "${command}" "${exit_code}" "${out}"
        "${err}")
endfunction()

# _expect_bundle(<directory>): the run succeeded, its standard output is exactly the line that
# names <directory>, and <directory> holds the manifest.
function(_expect_bundle directory)
    if(NOT exit_code STREQUAL "0")
        _fail("the script failed")
    endif()
    if(NOT out STREQUAL "${prefix}=${directory}\n")
        _fail("its standard output is not exactly the line\n${prefix}=${directory}")
    endif()
    if(NOT EXISTS "${directory}/${manifest_name}")
        _fail("${directory} holds no ${manifest_name}")
    endif()
endfunction()

# _expect_file(<file> <text>): <file> holds exactly <text>.
function(_expect_file file text)
    if(NOT EXISTS "${file}")
        _fail("${file} does not exist")
    endif()
    file(READ "${file}" content)
    if(NOT content STREQUAL text)
        _fail("${file} does not hold exactly\n${text}--- but ---\n${content}")
    endif()
endfunction()

# _expect_error(<text>...): the run failed, and its standard error says each <text>, read with
# each run of spaces and line feeds as one space.
function(_expect_error)
    if(exit_code STREQUAL "0")
        _fail("the script did not fail")
    endif()
    string(REGEX REPLACE "[ \n]+" " " joined "${err}")
    foreach(text IN LISTS ARGN)
        string(FIND "${joined}" "${text}" position)
        if(position EQUAL -1)
            _fail("its standard error does not say\n${text}")
        endif()
    endforeach()
endfunction()

# _expect_entries(<directory> <names>): <directory> holds exactly the entries <names> (a ;-list,
# sorted; empty for none).
function(_expect_entries directory names)
    file(GLOB found RELATIVE "${directory}" LIST_DIRECTORIES true "${directory}/*")
    list(SORT found)
    if(NOT found STREQUAL names)
        _fail("${directory} holds '${found}', not '${names}'")
    endif()
endfunction()

# tar
set(tar_dest "${WORK}/dest dir é")
set(tar_bundle "${tar_dest}/${bundle_name}")
set(tar_env "${WORK}/github env.txt")
file(WRITE "${tar_env}" "EXISTING=1\n")
set(command "${CMAKE_COMMAND}" -E env "GITHUB_ENV=${tar_env}" "${CMAKE_COMMAND}"
    -DPLATFORM=fake-tar "-DDESTINATION=${tar_dest}" "-DPIN_FILE=${pin}"
    "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(tar)
_expect_bundle("${tar_bundle}")
_expect_file("${tar_env}" "EXISTING=1\n${prefix}=${tar_bundle}\n")
_expect_entries("${tar_dest}" "${bundle_name};${bundle_name}.tar.xz")
message(STATUS "tar: the .tar.xz archive, downloaded from ${release_url}, is unpacked into "
    "${tar_bundle} and kept beside it; the line is printed and appended to GITHUB_ENV's file")

# zip
set(zip_dest "${WORK}/zip dest é")
set(zip_bundle "${zip_dest}/${bundle_name}")
set(zip_env "${WORK}/github-env-zip.txt")
set(command "${CMAKE_COMMAND}" -E env "GITHUB_ENV=${zip_env}" "${CMAKE_COMMAND}"
    -DPLATFORM=fake-zip "-DDESTINATION=${zip_dest}" "-DPIN_FILE=${pin}" -P "${SCRIPT}")
_fetch(zip)
_expect_bundle("${zip_bundle}")
_expect_file("${zip_env}" "${prefix}=${zip_bundle}\n")
_expect_entries("${zip_dest}" "${bundle_name};${bundle_name}.zip")
message(STATUS "zip: the .zip archive, downloaded from the pin's release, is unpacked into "
    "${zip_bundle}; GITHUB_ENV's file, new, holds only the line")

# rerun
file(WRITE "${tar_bundle}/added.txt" "Added by the test.\n")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${tar_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${file_url}${WORK}/no-server" -P "${SCRIPT}")
_fetch(rerun)
_expect_bundle("${tar_bundle}")
if(NOT EXISTS "${tar_bundle}/added.txt")
    _fail("the bundle was unpacked again: the file added to it is gone")
endif()
message(STATUS "rerun: the kept archive and the bundle are used as they are, with no download")

# reunpack: the same command.
file(WRITE "${tar_bundle}/${manifest_name}" "{\"version\":\"0.0.0\"}\n")
_fetch(reunpack)
_expect_bundle("${tar_bundle}")
_expect_file("${tar_bundle}/${manifest_name}" "${manifest_text}")
if(EXISTS "${tar_bundle}/added.txt")
    _fail("the bundle was not unpacked again: the file added to it is still there")
endif()
_expect_entries("${tar_dest}" "${bundle_name};${bundle_name}.tar.xz")
message(STATUS "reunpack: a bundle whose manifest names another version is unpacked again from "
    "the kept archive")

# stale
file(WRITE "${tar_dest}/${bundle_name}.tar.xz" "garbage\n")
file(REMOVE_RECURSE "${tar_bundle}")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${tar_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(stale)
_expect_bundle("${tar_bundle}")
file(SHA256 "${tar_dest}/${bundle_name}.tar.xz" kept_sha256)
if(NOT kept_sha256 STREQUAL tar_sha256)
    _fail("the kept archive's SHA-256 is ${kept_sha256}, not the pinned ${tar_sha256}")
endif()
_expect_entries("${tar_dest}" "${bundle_name};${bundle_name}.tar.xz")
message(STATUS "stale: a kept archive with another SHA-256 is downloaded again and unpacked")

# leftovers-kept, leftovers
string(SHA256 staging_id "${bundle_name}")
string(SUBSTRING "${staging_id}" 0 8 staging_id)
# _plant_leftovers(<destination>): what a run stopped while downloading and unpacking leaves.
function(_plant_leftovers destination)
    file(WRITE "${destination}/${bundle_name}.tar.xz.download" "partial")
    file(WRITE "${destination}/.u-${staging_id}/${bundle_name}/${manifest_name}" "${manifest_text}")
    file(WRITE "${destination}/.u-${staging_id}/${bundle_name}/planted.txt" "Planted.\n")
endfunction()
_plant_leftovers("${tar_dest}")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${tar_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${file_url}${WORK}/no-server" -P "${SCRIPT}")
_fetch(leftovers-kept)
_expect_bundle("${tar_bundle}")
_expect_entries("${tar_dest}" "${bundle_name};${bundle_name}.tar.xz")
set(leftovers_dest "${WORK}/leftovers dest")
set(leftovers_bundle "${leftovers_dest}/${bundle_name}")
_plant_leftovers("${leftovers_dest}")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${leftovers_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(leftovers)
_expect_bundle("${leftovers_bundle}")
if(EXISTS "${leftovers_bundle}/planted.txt")
    _fail("the bundle holds the file planted in the staging directory .u-${staging_id}/")
endif()
_expect_entries("${leftovers_dest}" "${bundle_name};${bundle_name}.tar.xz")
message(STATUS "leftovers-kept, leftovers: a partial download and a staging directory left by a "
    "stopped run are removed, beside a kept bundle and before a new one is unpacked")

# relative: the current directory, as the script sees it, has no symbolic links.
file(REAL_PATH "${WORK}" real_work)
set(relative_bundle "${real_work}/relative dest/${bundle_name}")
set(command "${CMAKE_COMMAND}" -E chdir "${WORK}" "${CMAKE_COMMAND}" -DPLATFORM=fake-zip
    "-DDESTINATION=relative dest" "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(relative)
_expect_bundle("${relative_bundle}")
message(STATUS "relative: a relative DESTINATION gives the absolute line\n"
    " ${prefix}=${relative_bundle}")

# bad
set(bad_dest "${WORK}/bad dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-bad "-DDESTINATION=${bad_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(bad)
_expect_error("SHA256" "${wrong_sha256}" "${tar_sha256}")
string(FIND "${err}" "attempt 2 of" position)
if(NOT position EQUAL -1)
    _fail("the download was tried again after the SHA-256 did not match")
endif()
_expect_entries("${bad_dest}" "")
message(STATUS "bad: an archive with another SHA-256 than the pinned one fails at once, giving "
    "both, and leaves nothing")

# retry
set(retry_dest "${WORK}/retry dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${retry_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${file_url}${WORK}/no-server" -P "${SCRIPT}")
_fetch(retry)
_expect_error("attempt 1 of 3" "attempt 2 of 3" "attempt 3 of 3" "the download failed 3 times"
    "the last time with status" "no-server/${bundle_name}.tar.xz")
_expect_entries("${retry_dest}" "")
message(STATUS "retry: a download that fails is tried 3 times, and the error gives the last status")

# unknown
set(command "${CMAKE_COMMAND}" -DPLATFORM=nope "-DDESTINATION=${WORK}/unknown dest"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(unknown)
_expect_error("nope" "fake-tar" "fake-zip" "fake-bad" "fake-other")
message(STATUS "unknown: a platform that the pin does not name fails, listing those it does")

# manifest
set(manifest_dest "${WORK}/manifest dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${manifest_dest}"
    "-DPIN_FILE=${pin_9_9_8}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(manifest)
_expect_error("version 9.9.9" "version 9.9.8" "${bundle_name}/${manifest_name}")
_expect_entries("${manifest_dest}" "${bundle_name}.tar.xz")
message(STATUS "manifest: a bundle of another version than the pin's fails, naming both, and "
    "leaves only the archive")

# other-manifest
set(other_dest "${WORK}/other-manifest dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-other "-DDESTINATION=${other_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(other-manifest)
_expect_error("${other_name}/${manifest_name}")
_expect_entries("${other_dest}" "${other_name}.tar.xz")
message(STATUS "other-manifest: an archive that holds the other script's manifest, "
    "${OTHER_MANIFEST}, instead of ${manifest_name} fails, naming it, and leaves only the archive")

# other-pin
set(other_pin_dest "${WORK}/other-pin dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${other_pin_dest}"
    "-DPIN_FILE=${pin_other}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(other-pin)
_expect_error("${pin_other} does not set ${prefix}_VERSION")
if(EXISTS "${other_pin_dest}")
    _fail("${other_pin_dest} exists: the script went on after the pin file was refused")
endif()
message(STATUS "other-pin: a pin file of the other script, which sets ${OTHER_PREFIX}_* and no "
    "${prefix}_* variable, fails, naming the first it lacks")

# no-pin-file
set(no_pin_file "${WORK}/no such pin.cmake")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${WORK}/no-pin dest"
    "-DPIN_FILE=${no_pin_file}" -P "${SCRIPT}")
_fetch(no-pin-file)
_expect_error("the pin file ${no_pin_file} does not exist")
message(STATUS "no-pin-file: a pin file that does not exist fails, naming it")

# no-platform, no-destination
set(command "${CMAKE_COMMAND}" "-DDESTINATION=${WORK}/unset dest" "-DPIN_FILE=${pin}"
    -P "${SCRIPT}")
_fetch(no-platform)
_expect_error("PLATFORM is not set" "-P ${SCRIPT},")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DPIN_FILE=${pin}" -P "${SCRIPT}")
_fetch(no-destination)
_expect_error("DESTINATION is not set" "-P ${SCRIPT},")
message(STATUS "no-platform, no-destination: each fails, saying which is not set, with the "
    "usage of ${SCRIPT}")

# pin
get_filename_component(script_dir "${SCRIPT}" DIRECTORY)
include("${script_dir}/${PIN_NAME}")
foreach(platform IN LISTS ${prefix}_PLATFORMS)
    string(TOUPPER "${platform}" key)
    string(REPLACE "-" "_" key "${key}")
    set(pin_dest "${WORK}/pin dest/${platform}")
    file(WRITE "${WORK}/pin-server/${${prefix}_${key}_ARCHIVE}" "Not the archive.\n")
    set(command "${CMAKE_COMMAND}" "-DPLATFORM=${platform}" "-DDESTINATION=${pin_dest}"
        "-DBASE_URL=${file_url}${WORK}/pin-server" -P "${SCRIPT}")
    _fetch("pin, ${platform}")
    _expect_error("SHA256" "${${prefix}_${key}_SHA256}"
        "pin-server/${${prefix}_${key}_ARCHIVE}")
    _expect_entries("${pin_dest}" "")
endforeach()
list(JOIN ${prefix}_PLATFORMS ", " platforms)
message(STATUS "pin: the script accepts each platform of the repository's pin file, ${platforms}, "
    "and checks the file it downloads against the pinned SHA-256")
