# ADR-0045: The Python environment bundle pinned in CI and shipped in the installation

- Status: Proposed
- Date: 2026-11-24
- Supersedes: none
- Superseded by: none

## Context

[ADR-0044](ADR-0044-python-modules-and-providers-through-an-embedded-environment.md) lets modules
and providers use the Python packages of the environment that praenora-runtime-pyenv builds. That
repository releases the environment as one archive per platform key (`linux-x64-release`,
`macos-arm64-release`, `windows-x64-release`, `windows-x64-debug`), each with a manifest and a CMake
package that refuses a build for another platform, an older macOS deployment target or an older
MSVC toolset. The runtime's CI already pins the bundles of praenora-runtime-deps and checks every
download against a SHA-256
([ADR-0041](ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md)), and the runtime's
release archives carry what an installation holds.

## Decision

Proposed; it follows ADR-0041's process for the pin.

- **The build asks for the bundle as a CMake package**: `find_package(PraenoraRuntimePyenv 0.1
  CONFIG REQUIRED)`, found through `CMAKE_PREFIX_PATH` or `PraenoraRuntimePyenv_DIR`, only with
  `PRAENORA_WITH_PYTHON_ENVIRONMENT=ON`. The build never downloads it and never uses
  `find_package(Python3)` for it; an imported target `praenora_libpython` is made from the package's
  variables. A build without the bundle, or with one that the package refuses, stops at
  configuration, and the package says why.
- **The environment's identity comes from its manifest** at configuration: package name, version,
  lock SHA-256 and interpreter version. The engine object of the provider `python`, and so the run
  fingerprint, carries them.
- **The pin and the fetch** follow the dependency bundles: a pin file,
  [`cmake/PraenoraRuntimePyenvPin.cmake`](../../cmake/PraenoraRuntimePyenvPin.cmake), with the
  release, and per platform key the archive's name and SHA-256 (the environment is built in Release
  only, so `windows-x64-debug` names the Windows Release archive); a fetch script,
  [`cmake/praenora_fetch_runtime_pyenv.cmake`](../../cmake/praenora_fetch_runtime_pyenv.cmake), that
  checks the download against the pin and prints `PRAENORA_RUNTIME_PYENV=<bundle>`; CI presets that
  name the bundle through that environment variable, with `PRAENORA_WITH_PYTHON_ENVIRONMENT` on and
  the bundle's CMake package in `PraenoraRuntimePyenv_DIR`; and a workflow step that fetches it in
  each build job. The pin names praenora-runtime-pyenv's release v0.1.0, published on 2026-11-25
  and built from its commit `1120cc4`; CI's use of it is not yet verified. The pin changes only
  after the maintainer confirms the change, with every archive name and SHA-256 of the release, as
  the dependency bundles' pin does
  ([releasing](../releasing.md#raising-the-python-environment-pin)).
- **The workflow's archive check skips the environment**, which the pyenv repository checks, and
  on Windows accepts the environment's `python3*.dll` for the Python core when the archive holds it
  in `lib/praenora/python-environment/python/`; the release notes name and link the pinned
  environment, and say where its license files are.
- **The installation holds the environment unchanged** in `lib/praenora/python-environment/`, with
  its license files copied to `share/praenora/licenses/python-environment/`, and the release
  archives carry it.
- **Fixtures use numpy** (the environment's general-purpose numeric package) and not the
  environment's native engines; the one exception is the fixture of the test of a Python step
  beside the roadrunner provider, a test that runs only with that provider, which simulates with
  the environment's `antimony` and `roadrunner` packages
  ([ADR-0044](ADR-0044-python-modules-and-providers-through-an-embedded-environment.md)). An
  example Python module, with a configuration variant of the example workflow that binds its
  predict step to it, is built only with the option and installed with the Examples component.
- **The directory digest function** (`PraenoraPythonDigest.cmake`) is installed with the CMake
  package, so a module built outside this repository computes the same digest the runtime checks.

### Builder notes (Proposed)

- **Variables of the environment module.** Besides the directory, package, version, lock and
  interpreter version, the module also sets the environment's name, its manifest and its interpreter
  executable, so tests and later rules need not read the package's variables again.
- **The imported target is `SHARED IMPORTED GLOBAL`** and is defined once, so a second inclusion of
  the module (a subproject) finds it.
- **The manifest is checked, not trusted**: each value the engine object needs must be present, the
  lock must be 64 lowercase hexadecimal digits, the manifest's package name must equal the CMake
  package's, and the interpreter, its library and its include directory must exist.
- **The environment's directory is `lib/praenora/python-environment/`**, not
  `lib/praenora/python/`: the name says what it holds, and the bundle's own `python/` below it does
  not become `python/python/`.
- **The installation copies the bundle's directory with `install(DIRECTORY ...
  USE_SOURCE_PERMISSIONS)`**, everything it holds and nothing left out (its `__pycache__`
  directories included), symbolic links as links: what the installation runs is what the bundle's
  lock and manifest describe, and what the pyenv repository checked. Its `licenses/` is copied a
  second time into `share/praenora/licenses/python-environment/`, where an installation's license
  files are.
- **The provider `python`'s directory is installed as the build tree has it**: the provider's
  library and manifest, the relative environment file, the core through `install(TARGETS)` (which
  gives it its relative `INSTALL_RPATH`) and the build's copy of `praenora.sdk`, whose digest the
  core carries, `__pycache__` left out. On Windows the core's import library goes to a component of
  its own that no installation asks for: nothing links the core.
- **The application manifest of `praenora.exe` declares `longPathAware` only.** The tool takes its
  arguments as UTF-16 and calls the system's wide functions, so it needs no UTF-8 code page of the
  process; the test programs that run the environment in their process get the same manifest.
- **The install test compares the installed environment with the bundle file by file**, by
  SHA-256, links by their targets and, with CMake 3.29 or later, executable bits, and checks the
  RPATHs of the core and the provider with `otool` or `readelf` where found: a moved installation
  that still found the bundle through an absolute path would otherwise pass. Its tests get more
  time in a tree with the environment, which is about a gigabyte. A passing install test removes
  what is large (its installation, the unpacked archive, cpack's staging) and a failing one keeps
  everything: with the environment the scratch would otherwise hold several gigabytes per tree, on
  CI runners' disks too.
- **The two fetch scripts share one implementation**, `cmake/praenora_fetch_bundle.cmake`, a
  function given the calling script's name and file, the prefix of the pin file's variables (which
  is also the printed variable's name), the manifest's file name and the default pin file. A second
  copy of the script would drift from the first; the dependency bundles' script keeps its
  interface, its output and its messages. Only CMake's own framing of an error changes: the line
  "CMake Error at" names the shared file, and a two-line call stack naming the calling script
  follows the message.
- **`ci.fetch_runtime_pyenv` is a test of its own** with the cases of `ci.fetch_runtime_deps`,
  while the dependency bundles' test stays as it is, so that it shows the shared implementation
  still behaving as the script did. Its driver takes every name that belongs to one script (the
  script, its pin file, the pin variables' prefix, the manifest, the bundles' names, and the other
  script's prefix and manifest) from its arguments, so that one driver can later check both
  scripts with a change of `tests/ci/CMakeLists.txt` only. It adds the cases that show what a
  script gives the implementation: the other script's manifest in an archive, and the other
  script's pin file, are refused, and the usage names the script.
- **`PRAENORA_RUNTIME_PYENV` is checked beside `PRAENORA_RUNTIME_DEPS`**, in
  `praenora_find_dependencies()`, before any dependency is looked up, so that a CI configuration
  without the bundle stops before anything is downloaded; and only with
  `PRAENORA_WITH_PYTHON_ENVIRONMENT`, so that one that turns the option off needs no bundle. The
  presets set the variables in the hidden `ci` preset, which `ci-windows` inherits.
- **The release job names the environment's version and repository `PYTHON_ENVIRONMENT_VERSION`
  and `PYTHON_ENVIRONMENT_REPOSITORY`**, not `PYENV_*`: the version manager pyenv reads
  `PYENV_VERSION`.
- **Windows path lengths: the jobs rely on CMake and CPack handling long paths**, rather than on
  shorter directories. With the environment, whose deepest file is about 140 characters below its
  root, the longest paths of a Windows job are in CPack's staging directories (about 329 characters
  for the target `PACKAGE`, 325 for `install.package`), which neither shorter work directories of
  the install tests nor a shorter checkout path would bring below 259; only CMake and CPack open
  them, and praenora-runtime-pyenv's workflow shows CPack archiving paths of about 300 characters on
  `windows-2022`. The Python files that an installation's programs open stay below 259. Not yet
  verified for this workflow ([releasing](../releasing.md#the-workflow)).

## Alternatives considered

- **`find_package(Python3 COMPONENTS Development.Embed)` against the bundle.** Not chosen: it can
  find another interpreter on the machine, and the bundle's library and headers are named exactly by
  its CMake package.
- **Building the environment in this repository.** Not chosen: the environment has its own
  repository, lock file and release, and its build needs tools (uv, Rust, native engines) that the
  runtime does not.
- **An environment fetched by the runtime at first run.** Not chosen: a default build and every test
  use no network.

## Consequences

Builds with the option need the bundle of their platform key; a bundle built with a newer macOS
deployment target than the build's is refused. The installation grows by the environment's size,
about a gigabyte, and its archive to about 190 MB on macOS arm64. Each CI job downloads a second
bundle, checked against its pin, and a release names the environment it holds; raising the
environment is a change of the pin, which the maintainer confirms. A build without the option, such
as the wheel's, needs no bundle. Windows jobs open paths longer than 259 characters (not yet
verified).

Open: CI's first runs with the pin, on every key, and the archives they make
([releasing.md](../releasing.md#what-the-runs-verified)); whether Windows jobs handle the long paths
of CPack's staging; and what distributing the environment's packages asks beyond their license
files, which every archive holds, a part of OQ-13's question of how binaries are distributed
([open-questions.md](../open-questions.md)).

## References

- [Python service](../specs/python-service.md), [releasing](../releasing.md),
  [developer guide](../developer-guide.md)
- [ADR-0017](ADR-0017-dependency-management.md),
  [ADR-0041](ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md),
  [ADR-0044](ADR-0044-python-modules-and-providers-through-an-embedded-environment.md)
