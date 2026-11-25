# Downloads, checks and unpacks the Python environment bundle of praenora-runtime-pyenv that the
# pin file (PraenoraRuntimePyenvPin.cmake) names for one platform, for CI jobs and for developers;
# the project's build does not include it. Script mode; it needs only CMake 3.24 or newer:
#
#   cmake -DPLATFORM=<key> -DDESTINATION=<dir> [-DPIN_FILE=<file>] [-DBASE_URL=<url>]
#         -P cmake/praenora_fetch_runtime_pyenv.cmake
#
# - PLATFORM: a key of PRAENORA_RUNTIME_PYENV_PLATFORMS in the pin file, such as
#   macos-arm64-release.
# - DESTINATION: the directory that receives the archive and the bundle, made when it does not
#   exist. A relative path is relative to the current directory.
# - PIN_FILE: the pin file, a relative path being relative to the current directory; by default
#   PraenoraRuntimePyenvPin.cmake beside this script (the test ci.fetch_runtime_pyenv gives one of
#   its own).
# - BASE_URL: where the archives are, taken as given, with /<archive> appended; by default the
#   pinned release, <repository>/releases/download/v<version>.
#
# It behaves as praenora_fetch_runtime_deps.cmake does, with the same implementation,
# praenora_fetch_bundle.cmake beside this script, whose header comment gives the details: it
# downloads the archive into DESTINATION (up to 3 attempts), checks its SHA-256 against the pin,
# unpacks it into <DESTINATION>/<name>/, <name> being the archive's name without .tar.xz or .zip,
# and checks that the bundle's manifest, praenora-runtime-pyenv.json, names the pinned version. It
# then prints one line to standard output, PRAENORA_RUNTIME_PYENV=<bundle directory>, an absolute
# path, and, when the environment variable GITHUB_ENV names a file (as in a step of a GitHub
# Actions job, whose later steps then have the variable), appends the same line to that file.
# Every other message goes to standard error. A later run with the same DESTINATION reuses the
# archive and the bundle that an earlier one checked, and removes what a stopped run left. Each
# error stops the script with message(FATAL_ERROR), so cmake exits with 1.

cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/praenora_fetch_bundle.cmake")
praenora_fetch_bundle(
    SCRIPT "praenora_fetch_runtime_pyenv.cmake"
    SCRIPT_FILE "${CMAKE_CURRENT_LIST_FILE}"
    PREFIX PRAENORA_RUNTIME_PYENV
    MANIFEST "praenora-runtime-pyenv.json"
    DEFAULT_PIN_FILE "${CMAKE_CURRENT_LIST_DIR}/PraenoraRuntimePyenvPin.cmake")
