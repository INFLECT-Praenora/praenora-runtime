# The pin of the dependency bundles that CI jobs and developers build the runtime against: a
# release of praenora-runtime-deps and, for each platform, the name and SHA-256 of its archive
# there. cmake/praenora_fetch_runtime_deps.cmake reads it; the project's build does not include it.
#
# Defines PRAENORA_RUNTIME_DEPS_VERSION (the release, v<version>), PRAENORA_RUNTIME_DEPS_REPOSITORY,
# PRAENORA_RUNTIME_DEPS_PLATFORMS (the platform keys) and, for each key, with <KEY> the key in
# upper case and each '-' as '_', PRAENORA_RUNTIME_DEPS_<KEY>_ARCHIVE (the archive's file name in
# the release) and PRAENORA_RUNTIME_DEPS_<KEY>_SHA256 (its SHA-256, as the release's .sha256 file
# beside it gives it).
#
# The pin is changed only after the maintainer confirms the change: raised to a published release,
# with every archive name and SHA-256 of that release, or corrected within one
# (docs/releasing.md).

set(PRAENORA_RUNTIME_DEPS_VERSION "0.1.1")
set(PRAENORA_RUNTIME_DEPS_REPOSITORY "https://github.com/INFLECT-Praenora/praenora-runtime-deps")
set(PRAENORA_RUNTIME_DEPS_PLATFORMS
    "linux-x64-release;macos-arm64-release;windows-x64-release;windows-x64-debug")

set(PRAENORA_RUNTIME_DEPS_LINUX_X64_RELEASE_ARCHIVE
    "praenora-runtime-deps-0.1.1-linux-x86_64-gcc13-release.tar.xz")
set(PRAENORA_RUNTIME_DEPS_LINUX_X64_RELEASE_SHA256
    "a426eddff1551911e62d0eddde63675e4bb070d03df2498e0193929f77640901")

set(PRAENORA_RUNTIME_DEPS_MACOS_ARM64_RELEASE_ARCHIVE
    "praenora-runtime-deps-0.1.1-macos-arm64-appleclang21-release.tar.xz")
set(PRAENORA_RUNTIME_DEPS_MACOS_ARM64_RELEASE_SHA256
    "fa328c4130869b57323e90034bd7b78102026ef661f6fd2f05440beaeb67a98f")

set(PRAENORA_RUNTIME_DEPS_WINDOWS_X64_RELEASE_ARCHIVE
    "praenora-runtime-deps-0.1.1-windows-x86_64-msvc143-release.zip")
set(PRAENORA_RUNTIME_DEPS_WINDOWS_X64_RELEASE_SHA256
    "9029c3d07c1ee075791fa793d2b34acb9c28a3f1deb0883db32de9a39a242594")

set(PRAENORA_RUNTIME_DEPS_WINDOWS_X64_DEBUG_ARCHIVE
    "praenora-runtime-deps-0.1.1-windows-x86_64-msvc143-debug.zip")
set(PRAENORA_RUNTIME_DEPS_WINDOWS_X64_DEBUG_SHA256
    "cac90ebb82a6aee6f080b95b2585533842d1f5004e77bf8be6117f8d29f2b5fa")
