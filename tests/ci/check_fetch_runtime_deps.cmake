# Checks cmake/praenora_fetch_runtime_deps.cmake, which downloads, checks and unpacks the dependency
# bundle that a pin file names for a platform: the driver of ci.fetch_runtime_deps
# (tests/ci/CMakeLists.txt). Script mode, cmake -P, with SCRIPT set to the script and WORK to an
# absolute directory of the build tree, which it deletes and fills; it needs only CMake and uses no
# network: the script downloads from file:// URLs.
#
# It makes two fake bundles in WORK: praenora-runtime-deps-9.9.9-test-release/, which holds a
# manifest praenora-runtime-deps.json, {"version":"9.9.9"}, and
# praenora-runtime-deps-9.9.9-empty-release/, which holds none. It archives the first as .tar.xz
# and as .zip and the second as .tar.xz into "repository é/releases/download/v9.9.9/", laid out
# like the release v9.9.9 of a repository file://<WORK>/repository é (so every URL holds a space
# and a non-ASCII character, as they are), and writes two pin files: pin.cmake, of version 9.9.9
# and that repository, with the platforms
#   fake-tar    the .tar.xz archive of the first bundle, with its SHA-256,
#   fake-zip    the .zip archive of the first bundle, with its SHA-256,
#   fake-bad    the .tar.xz archive of the first bundle, with a wrong SHA-256,
#   fake-empty  the archive of the second bundle, with its SHA-256,
# and pin-9.9.8.cmake, of version 9.9.8, whose only platform, fake-tar, is the same archive.
#
# Then it runs the script through tests/support/run_command.cmake and checks its exit code, its
# standard output, its standard error where it fails, and the files it leaves. GITHUB_ENV is unset
# in every case but the first two, which set it to a file of WORK: a run of the tests in a GitHub
# Actions job must not append to the job's own file.
#   tar          fake-tar into "dest dir é", BASE_URL the release's directory, GITHUB_ENV a file
#                that holds a line already: exit code 0; the standard output is exactly the line
#                PRAENORA_RUNTIME_DEPS=<WORK>/dest dir é/praenora-runtime-deps-9.9.9-test-release,
#                the file holds its old line and then this one, the bundle holds its manifest and
#                the archive is kept beside it.
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
#                the line names <WORK>/relative dest/praenora-runtime-deps-9.9.9-test-release, an
#                absolute path.
#   bad          fake-bad: a nonzero exit code after one attempt; the standard error says SHA256
#                and gives both values, and the destination holds nothing.
#   retry        fake-tar, BASE_URL a directory without archives: a nonzero exit code after 3
#                attempts, 3 seconds apart; the standard error says so, with the last status, and
#                the destination holds nothing.
#   unknown      PLATFORM nope: a nonzero exit code; the standard error lists the four platforms.
#   manifest     fake-tar of pin-9.9.8.cmake: a nonzero exit code; the standard error names both
#                versions, and the destination holds only the archive.
#   no-manifest  fake-empty: a nonzero exit code; the standard error names the missing manifest,
#                and the destination holds only the archive.
#   no-platform, no-destination
#                PLATFORM, then DESTINATION, not set: a nonzero exit code; the standard error says
#                which.
#   pin          each platform of the repository's own pin file, PraenoraRuntimeDepsPin.cmake
#                beside SCRIPT, without PIN_FILE (so that one), BASE_URL a directory that holds a
#                file of garbage under each pinned archive name: the script accepts the pinned
#                archive name and SHA-256, downloads the file and fails at its SHA-256, giving the
#                pinned one, and the destination holds nothing.
# The standard error is searched with each run of spaces and line feeds in it read as one space:
# message() wraps its lines.
cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/../support/run_command.cmake")

foreach(variable IN ITEMS SCRIPT WORK)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "check_fetch_runtime_deps.cmake: ${variable} is not set")
    endif()
endforeach()
if(NOT IS_ABSOLUTE "${WORK}")
    message(FATAL_ERROR "check_fetch_runtime_deps.cmake: WORK must be an absolute path")
endif()
# The script's runs inherit this environment.
unset(ENV{GITHUB_ENV})

set(what "ci.fetch_runtime_deps")
set(bundle_name "praenora-runtime-deps-9.9.9-test-release")
set(empty_name "praenora-runtime-deps-9.9.9-empty-release")
set(manifest_name "praenora-runtime-deps.json")
set(manifest_text "{\"version\":\"9.9.9\"}\n")

file(REMOVE_RECURSE "${WORK}")
set(bundles "${WORK}/bundles")
# The é is a fixture choice (real use downloads from https:// URLs); drop it if Windows fails.
set(repository "${WORK}/repository é")
set(release "${repository}/releases/download/v9.9.9")
file(MAKE_DIRECTORY "${bundles}/${bundle_name}" "${bundles}/${empty_name}" "${release}")
file(WRITE "${bundles}/${bundle_name}/${manifest_name}" "${manifest_text}")
file(WRITE "${bundles}/${bundle_name}/README.txt" "A fake bundle of the test ${what}.\n")
file(WRITE "${bundles}/${empty_name}/README.txt" "A fake bundle without a manifest.\n")

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
_archive("${empty_name}.tar.xz" "${empty_name}" cJf)
file(SHA256 "${release}/${bundle_name}.tar.xz" tar_sha256)
file(SHA256 "${release}/${bundle_name}.zip" zip_sha256)
file(SHA256 "${release}/${empty_name}.tar.xz" empty_sha256)
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
    "set(PRAENORA_RUNTIME_DEPS_VERSION \"9.9.9\")\n"
    "set(PRAENORA_RUNTIME_DEPS_REPOSITORY [==[${repository_url}]==])\n"
    "set(PRAENORA_RUNTIME_DEPS_PLATFORMS \"fake-tar;fake-zip;fake-bad;fake-empty\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_TAR_ARCHIVE \"${bundle_name}.tar.xz\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_TAR_SHA256 \"${tar_sha256}\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_ZIP_ARCHIVE \"${bundle_name}.zip\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_ZIP_SHA256 \"${zip_sha256}\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_BAD_ARCHIVE \"${bundle_name}.tar.xz\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_BAD_SHA256 \"${wrong_sha256}\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_EMPTY_ARCHIVE \"${empty_name}.tar.xz\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_EMPTY_SHA256 \"${empty_sha256}\")\n")
file(WRITE "${pin}" "${pin_text}")
set(pin_9_9_8 "${WORK}/pin-9.9.8.cmake")
string(CONCAT pin_text
    "set(PRAENORA_RUNTIME_DEPS_VERSION \"9.9.8\")\n"
    "set(PRAENORA_RUNTIME_DEPS_REPOSITORY [==[${repository_url}]==])\n"
    "set(PRAENORA_RUNTIME_DEPS_PLATFORMS \"fake-tar\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_TAR_ARCHIVE \"${bundle_name}.tar.xz\")\n"
    "set(PRAENORA_RUNTIME_DEPS_FAKE_TAR_SHA256 \"${tar_sha256}\")\n")
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
    if(NOT out STREQUAL "PRAENORA_RUNTIME_DEPS=${directory}\n")
        _fail("its standard output is not exactly the line\nPRAENORA_RUNTIME_DEPS=${directory}")
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
_expect_file("${tar_env}" "EXISTING=1\nPRAENORA_RUNTIME_DEPS=${tar_bundle}\n")
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
_expect_file("${zip_env}" "PRAENORA_RUNTIME_DEPS=${zip_bundle}\n")
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
    " PRAENORA_RUNTIME_DEPS=${relative_bundle}")

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
_expect_error("nope" "fake-tar" "fake-zip" "fake-bad" "fake-empty")
message(STATUS "unknown: a platform that the pin does not name fails, listing those it does")

# manifest
set(manifest_dest "${WORK}/manifest dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DDESTINATION=${manifest_dest}"
    "-DPIN_FILE=${pin_9_9_8}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(manifest)
_expect_error("version 9.9.9" "version 9.9.8")
_expect_entries("${manifest_dest}" "${bundle_name}.tar.xz")
message(STATUS "manifest: a bundle of another version than the pin's fails, naming both, and "
    "leaves only the archive")

# no-manifest
set(no_manifest_dest "${WORK}/no-manifest dest")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-empty "-DDESTINATION=${no_manifest_dest}"
    "-DPIN_FILE=${pin}" "-DBASE_URL=${release_url}" -P "${SCRIPT}")
_fetch(no-manifest)
_expect_error("${empty_name}/${manifest_name}")
_expect_entries("${no_manifest_dest}" "${empty_name}.tar.xz")
message(STATUS "no-manifest: an archive without the manifest fails, naming it, and leaves only "
    "the archive")

# no-platform, no-destination
set(command "${CMAKE_COMMAND}" "-DDESTINATION=${WORK}/unset dest" "-DPIN_FILE=${pin}"
    -P "${SCRIPT}")
_fetch(no-platform)
_expect_error("PLATFORM is not set")
set(command "${CMAKE_COMMAND}" -DPLATFORM=fake-tar "-DPIN_FILE=${pin}" -P "${SCRIPT}")
_fetch(no-destination)
_expect_error("DESTINATION is not set")
message(STATUS "no-platform, no-destination: each fails, saying which is not set")

# pin
get_filename_component(script_dir "${SCRIPT}" DIRECTORY)
include("${script_dir}/PraenoraRuntimeDepsPin.cmake")
foreach(platform IN LISTS PRAENORA_RUNTIME_DEPS_PLATFORMS)
    string(TOUPPER "${platform}" key)
    string(REPLACE "-" "_" key "${key}")
    set(pin_dest "${WORK}/pin dest/${platform}")
    file(WRITE "${WORK}/pin-server/${PRAENORA_RUNTIME_DEPS_${key}_ARCHIVE}" "Not the archive.\n")
    set(command "${CMAKE_COMMAND}" "-DPLATFORM=${platform}" "-DDESTINATION=${pin_dest}"
        "-DBASE_URL=${file_url}${WORK}/pin-server" -P "${SCRIPT}")
    _fetch("pin, ${platform}")
    _expect_error("SHA256" "${PRAENORA_RUNTIME_DEPS_${key}_SHA256}"
        "pin-server/${PRAENORA_RUNTIME_DEPS_${key}_ARCHIVE}")
    _expect_entries("${pin_dest}" "")
endforeach()
list(JOIN PRAENORA_RUNTIME_DEPS_PLATFORMS ", " platforms)
message(STATUS "pin: the script accepts each platform of the repository's pin file, ${platforms}, "
    "and checks the file it downloads against the pinned SHA-256")
