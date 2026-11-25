# The pin of the Python environment bundles that CI jobs build the runtime against, with
# PRAENORA_WITH_PYTHON_ENVIRONMENT: a release of praenora-runtime-pyenv and, for each platform key,
# the name and SHA-256 of its archive there. cmake/praenora_fetch_runtime_pyenv.cmake reads it; the
# project's build does not include it.
#
# Defines PRAENORA_RUNTIME_PYENV_VERSION (the release, v<version>),
# PRAENORA_RUNTIME_PYENV_REPOSITORY, PRAENORA_RUNTIME_PYENV_PLATFORMS (the platform keys, those of
# PraenoraRuntimeDepsPin.cmake) and, for each key, with <KEY> the key in upper case and each '-' as
# '_', PRAENORA_RUNTIME_PYENV_<KEY>_ARCHIVE (the archive's file name in the release) and
# PRAENORA_RUNTIME_PYENV_<KEY>_SHA256 (its SHA-256, as the release's .sha256 file beside it and
# GitHub's digest of the asset give it). The environment is built in Release only, so
# windows-x64-debug names the archive of windows-x64-release.
#
# The pin is changed only after the maintainer confirms the change: raised to a published release,
# with every archive name and SHA-256 of that release, or corrected within one
# (docs/releasing.md).

set(PRAENORA_RUNTIME_PYENV_VERSION "0.1.0")
set(PRAENORA_RUNTIME_PYENV_REPOSITORY "https://github.com/INFLECT-Praenora/praenora-runtime-pyenv")
set(PRAENORA_RUNTIME_PYENV_PLATFORMS
    "linux-x64-release;macos-arm64-release;windows-x64-release;windows-x64-debug")

set(PRAENORA_RUNTIME_PYENV_LINUX_X64_RELEASE_ARCHIVE
    "praenora-runtime-pyenv-0.1.0-linux-x86_64-cp312-gcc13-release.tar.xz")
set(PRAENORA_RUNTIME_PYENV_LINUX_X64_RELEASE_SHA256
    "4cb18f720c22aaee4f50266ba0369b75410206fa98f8e4e78a3d304222db7ca8")

set(PRAENORA_RUNTIME_PYENV_MACOS_ARM64_RELEASE_ARCHIVE
    "praenora-runtime-pyenv-0.1.0-macos-arm64-cp312-appleclang21-release.tar.xz")
set(PRAENORA_RUNTIME_PYENV_MACOS_ARM64_RELEASE_SHA256
    "d923e10215bd13a7e8ccc525824941bbf5c48faf45c3f6ea30a00be64b216160")

set(PRAENORA_RUNTIME_PYENV_WINDOWS_X64_RELEASE_ARCHIVE
    "praenora-runtime-pyenv-0.1.0-windows-x86_64-cp312-msvc143-release.zip")
set(PRAENORA_RUNTIME_PYENV_WINDOWS_X64_RELEASE_SHA256
    "8dd8abc51b9ebeb2a15358911bc4c4506548bda61bde6b4085da71c2e625be0d")

set(PRAENORA_RUNTIME_PYENV_WINDOWS_X64_DEBUG_ARCHIVE
    "praenora-runtime-pyenv-0.1.0-windows-x86_64-cp312-msvc143-release.zip")
set(PRAENORA_RUNTIME_PYENV_WINDOWS_X64_DEBUG_SHA256
    "8dd8abc51b9ebeb2a15358911bc4c4506548bda61bde6b4085da71c2e625be0d")
