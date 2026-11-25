# Releasing

This document describes how GitHub Actions builds and tests the runtime against the dependency
bundles of [praenora-runtime-deps](https://github.com/INFLECT-Praenora/praenora-runtime-deps) and
the Python environment bundles of
[praenora-runtime-pyenv](https://github.com/INFLECT-Praenora/praenora-runtime-pyenv), and how an
administrator publishes a release with the archives it makes. Running the same presets on your own
machine is in [developer-guide.md](developer-guide.md#ci-presets), what an archive holds in
[developer-guide.md](developer-guide.md#archiving-an-installation), and checking and running one in
[user-guide.md](user-guide.md#getting-the-tool). The decisions behind the workflow are in
[ADR-0041](adr/ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md), and those behind the
Python environment's pin in
[ADR-0045](adr/ADR-0045-python-environment-pinned-in-ci-and-installation.md).

## What the runs verified

This section says what the workflow's runs have verified, and is updated in place when a run
verifies more. Runs that failed, and the fixes they needed, are for commit messages, not for this
section or [ADR-0041](adr/ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md).

The workflow first passed on 2026-11-22, in two runs: on the pull request that brought it to
`main` (run 45041491678, head commit `de4b0e8`), and on `main` with `publish` (run 45044102336,
commit `bcc3775`), which drafted the first release, v0.2.0; an administrator published it the same
day.
In both runs every build job passed every step: the bundle downloaded from praenora-runtime-deps'
release v0.1.1 and checked against the pin, yaml-cpp, spdlog and GoogleTest downloaded at their
pinned commits, the configuration, a build with warnings as errors, the tests, the archive and the
check of its libraries. With `publish`, the release job checked the archives and ran the Linux
archive's tool for the release notes.

| Job | Runner | Compiler | Tests (none failed) |
|---|---|---|---|
| `linux-x64-release` | `ubuntu-24.04` | GCC 13.3.0 | 1146, of which 15 skipped: the 12 of the restricted sandbox profile, which is available only on macOS, and 3 that need what the runner lacks (the locale `de_DE.UTF-8`, a locale that folds bytes beyond ASCII, a file system that ignores case). |
| `macos-arm64-release` | `macos-26` | AppleClang 21.0.0 | 1150, none skipped: the runner applies no sandbox, so the tests of the restricted profile ran. |
| `windows-x64-release` | `windows-2022` | MSVC 19.44 (Visual Studio 2022) | 986, of which 5 skipped: 3 that need a file the process cannot read (Windows does not enforce the permissions they set), the check that a provider library exports only its entry point (Windows exports only what is marked `__declspec(dllexport)`), and `Frontend.NamesFoldOnlyAsciiLetters` (no locale that folds bytes beyond ASCII). |
| `windows-x64-debug` | `windows-2022` | MSVC 19.44 (Visual Studio 2022) | 986, of which 5 skipped, as in the Release job. |

Windows runs fewer tests mostly because it has no process isolation in this version. The runs
verified, on Linux x86-64 and Windows x86-64 for the first time: the CI presets with both
providers, the Python bindings (CPython 3.12), the install and archive tests, and, on Linux,
process isolation in module hosts; on every key, fetch mode for yaml-cpp, spdlog and GoogleTest
and a bundle downloaded from its release. They do not verify: the other Linux presets (Debug and
the sanitizer builds); other compilers; the distributions' or vcpkg's packages; a wheel build;
the tools of the macOS and Windows archives, which no job runs; whether a provider's results are
the same on other machines of one architecture, or the same across platforms beyond the tests'
committed expected outputs; and the license review of distributing the providers (OQ-27, OQ-29)
([platforms.md](platforms.md)).

These runs came before the control API and the OTLP export
([ADR-0042](adr/ADR-0042-control-and-status-api-over-http.md),
[ADR-0043](adr/ADR-0043-otlp-export-of-metrics-and-logs.md)), so the test counts above are those
of the tree before them. Run 45165537866, by hand on `feature/server`, built and tested every
platform key with them: cpp-httplib downloaded at its pinned commit, its license installed, and the
listener tests run with `PRAENORA_TESTS_REQUIRE_LISTENERS=1`; its test counts are not recorded
here.

The Python environment
([ADR-0045](adr/ADR-0045-python-environment-pinned-in-ci-and-installation.md)) came after all of
these runs: no run has yet fetched the pinned Python environment bundles, built the CI presets with
them, tested the provider `python` and the Python modules on Linux or Windows, or archived an
installation with the environment (not yet verified).

## The workflow

[`.github/workflows/build.yml`](../.github/workflows/build.yml) (named "Build") runs:

- on every pull request to `main`;
- when run manually, on any branch: in the repository's Actions tab, *Build*, *Run workflow*; or
  `gh workflow run build.yml --ref <branch>`. GitHub offers the manual run once the workflow is on
  `main`. With the input `publish`, which only `main` accepts, the run also creates a draft
  release.

Its jobs:

1. **Prepare** reads the runtime's version from `project(VERSION)` in `CMakeLists.txt`, the
   version and repository of the dependency bundles from
   [`cmake/PraenoraRuntimeDepsPin.cmake`](../cmake/PraenoraRuntimeDepsPin.cmake), and those of the
   Python environment bundles from
   [`cmake/PraenoraRuntimePyenvPin.cmake`](../cmake/PraenoraRuntimePyenvPin.cmake). With `publish`,
   it stops the run unless the run is on `main` and the tag `v<version>` does not exist; when it
   exists, the message says to raise `project(VERSION)` in `CMakeLists.txt`
   ([Release procedure](#release-procedure)). It looks for the tag with
   `git ls-remote --exit-code`, which exits with 2 when there is no such tag; any other failure
   stops the run too, saying that `git ls-remote` could not list the tags. The checkout keeps no
   credentials, so this works because the repository is public. In a private repository every
   run with `publish` would stop there, and the check would need the job's token instead, such as
   `gh api repos/<owner>/<repository>/git/ref/tags/v<version>` with `GH_TOKEN`. That endpoint
   names exactly one tag, unlike `git/matching-refs/`, which lists every tag that begins with the
   name (`v0.2.0` would find `v0.2.0-rc1`): it answers with the tag's reference when the tag
   exists, and with HTTP 404 (Not Found) when it does not. `gh api` then exits with 1 and says
   `HTTP 404` on standard error, so the check would read a failure that says `HTTP 404` as "no
   such tag", and stop the run on any other failure, as it does for `git ls-remote` (not yet
   verified).
2. **One build job per platform key**, named after the key:

   | Job | Runner | Compiler | Archive |
   |---|---|---|---|
   | `linux-x64-release` | `ubuntu-24.04` | GCC 13 | `praenora-runtime-<version>-linux-x86_64-gcc13-release.tar.xz` |
   | `macos-arm64-release` | `macos-26` | AppleClang 21 | `praenora-runtime-<version>-macos-arm64-appleclang21-release.tar.xz` |
   | `windows-x64-release` | `windows-2022` | MSVC of Visual Studio 2022 (toolset 143) | `praenora-runtime-<version>-windows-x86_64-msvc143-release.zip` |
   | `windows-x64-debug` | `windows-2022` | MSVC of Visual Studio 2022 (toolset 143) | `praenora-runtime-<version>-windows-x86_64-msvc143-debug.zip` |

   The compilers are those that the archive names of the pinned bundles give
   ([The dependency bundles](#the-dependency-bundles)), which these images are expected to have
   still; the runs of 2026-11-22 got them (GCC 13.3.0, AppleClang 21.0.0, MSVC 19.44)
   ([What the runs verified](#what-the-runs-verified)). Each
   job turns off Git's line-ending conversion (Windows), checks out the repository, sets up
   Python 3.12 with `actions/setup-python`, installs Ninja and SWIG with apt (Linux), those of
   them that are missing with Homebrew (macOS), or SWIG with Chocolatey (Windows, whose presets
   use the Visual Studio 2022 generator, which finds MSVC without a developer environment),
   fetches the dependency bundle and the Python environment bundle of its key, and runs, with
   `<key>` its key:

   ```sh
   cmake -DPLATFORM=<key> -DDESTINATION=<runner temp>/deps \
       -P cmake/praenora_fetch_runtime_deps.cmake
   cmake -DPLATFORM=<key> -DDESTINATION=<runner temp>/pyenv \
       -P cmake/praenora_fetch_runtime_pyenv.cmake
   cmake --preset ci-<key> -DPython3_EXECUTABLE=<the Python set up>
   cmake --build --preset ci-<key> --parallel
   ctest --preset ci-<key>
   cmake --build --preset ci-<key> --target package    # PACKAGE on the windows-* keys
   ```

   The presets are those of [developer-guide.md](developer-guide.md#ci-presets): both providers,
   the Python environment with the provider `python`, the Python bindings and the tests, with
   warnings as errors, and yaml-cpp, spdlog, GoogleTest and cpp-httplib (header-only) built from
   their pinned commits. `ctest` runs every test of the tree.
   Its test preset sets `PRAENORA_TESTS_REQUIRE_LISTENERS=1`, so the tests that open local
   listeners (label `server`, and the Python test of the OTLP export) fail, instead of skipping,
   should the runner refuse a listener on `127.0.0.1`, `::1` or a Unix socket
   ([testing.md](testing.md#listeners)); a job's run never passes without them.

   **The archive's libraries.** The job then checks that the archive needs only the system's
   libraries and its own, as the presets' static third-party libraries should make it. No test of
   the tree checks this, and a run on a machine where the libraries are installed would not show
   it. It unpacks the archive into the runner's temporary directory and lists the libraries that
   each program and library of `bin/`, `lib/` (the providers included) and `libexec/` names: with
   `readelf -d` on Linux (its `NEEDED` entries), `otool -L` on macOS, and `dumpbin -dependents` on
   Windows (that of Visual Studio 2022, found with `vswhere`). It accepts:

   | Platform | Libraries accepted |
   |---|---|
   | Linux | glibc's (`libc.so.6`, `libm.so.6`, `libdl.so.2`, `libpthread.so.0`, `librt.so.1`, `libutil.so.1`, `libresolv.so.2`, `ld-linux-x86-64.so.2`), GCC's `libstdc++.so.6` and `libgcc_s.so.1`, and a name that a file or link of the archive has. |
   | macOS | `/usr/lib/*` and `/System/*`, and an `@rpath/`, `@loader_path/` or `@executable_path/` name whose last part a file or link of the archive has. |
   | Windows | The API sets (`api-ms-win-*`, `ext-ms-*`), the Visual C++ runtime and its debug libraries (`msvcp140*`, `vcruntime140*`, `concrt140*`, `ucrtbase*`), `praenora_runtime.dll` from the archive's `bin\`, `python3*.dll` for the files of `lib\praenora\providers\python\` (the Python core) when the archive holds it in `lib\praenora\python-environment\python\`, and the DLLs in `%SystemRoot%\System32`. |

   Any other library fails the step, which names it with the file that needs it, as does a file
   whose list cannot be read or is empty. The step skips the Python environment,
   `lib/praenora/python-environment/`: the archive holds the bundle unchanged, and
   praenora-runtime-pyenv's workflow checks it. On Linux and macOS the Python core's libpython
   (`libpython3.12.so.1.0`, `@rpath/libpython3.12.dylib`) is accepted as any library whose name a
   file of the archive has.

   On macOS arm64 the step's script was first run on the archive of a local
   `ci-macos-arm64-release` tree configured with Homebrew's yaml-cpp and spdlog
   ([developer-guide.md](developer-guide.md#ci-presets)), where it fails, naming Homebrew's
   yaml-cpp, spdlog and fmt for the runtime library and the module host, and, with those entries
   left out of `otool`'s output, passes; its Linux and Windows parts were first run on hand-made
   archives, with scripts that print the output of `readelf` and `dumpbin` in their formats. On
   GitHub the step passed on every key, before the Python environment
   ([What the runs verified](#what-the-runs-verified)). With the environment (skipping it,
   accepting the core's libpython and, on Windows, the rule for `python3*.dll`), the step is not
   yet verified, on GitHub or on a local archive.

   Last, the job uploads the archive and its `.sha256` file from `build/ci-<key>/package/` as a
   workflow artifact named after the key, kept for 7 days.
3. **Draft release**, with `publish` only: downloads the archives, checks that their names carry
   the version and that they match their `.sha256` files, unpacks the Linux archive and runs its
   `bin/praenora version --json` and `providers list --json` for the release notes (the versions
   of the runtime, the module SDK, PMP and the module ABI, and each provider with its services and
   engine), names the pinned releases of the dependency bundles and of the Python environment
   bundles, each linked, says that the Python environment's licenses are in each archive (its
   license files in `share/praenora/licenses/python-environment/`), and creates the draft release
   `v<version>` of the built commit, with every archive and `.sha256` file. A draft is visible only
   to users with write access, and creates no tag; publishing it creates the tag `v<version>` on
   that commit.

**Path lengths on Windows.** praenora-runtime-deps' workflow maps a drive letter to the parent of
its checkout with `subst`, because `cl` refuses an object file whose path is longer than 259
characters and its object files' paths were longer. This workflow does not: in the checkout
`D:\a\praenora-runtime\praenora-runtime\` (39 characters), the longest object file path, which
`cl` writes, is estimated at 172 characters, and the files of FetchContent's sub-build of
GoogleTest at about 197. Without the Python environment, the longest path of a
`windows-x64-release` job was estimated at 235 characters, a license file in CPack's staging
directory, and both Windows jobs passed without shortening the path
([What the runs verified](#what-the-runs-verified)).

The Python environment takes other paths past 259. Its deepest file is about 140 characters below
the bundle's root (`python\Lib\site-packages\jedi\third_party\typeshed\stubs\...`), and an
installation holds the bundle in `lib\praenora\python-environment\` (32 characters). With the
archive name `praenora-runtime-<version>-windows-x86_64-msvc143-release` (53 characters for
0.2.0), the estimates for a `windows-x64-release` job are:

| Where | Longest path |
|---|---|
| CPack's staging directory of the target `PACKAGE`, `build\ci-windows-x64-release\package\_CPack_Packages\win64\ZIP\<archive name>\` | 329 |
| The staging directory of `install.package`, `build\ci-<key>\pkg\_CPack_Packages\win64\ZIP\<archive name>\` | 325 |
| The archive unpacked by `install.package`, `build\ci-<key>\pkg\u\<archive name>\` | 301 |
| The moved installation of `install.consumer`, `build\ci-<key>\tests\install\work\moved prëfix\` | 273 |
| The installation of `install.consumer`, `build\ci-<key>\tests\install\work\prefix\` | 267 |
| The archive unpacked by the step *Check the archive's libraries*, `D:\a\_temp\archive-check\<archive name>\` | 252 |
| The installation of `install.package`, `build\ci-<key>\pkg\i\` | 247 |
| The Python environment bundle, `D:\a\_temp\pyenv\<bundle name>\` (12 more while the fetch script unpacks it) | 224 |

The `windows-x64-debug` job's are up to 4 characters shorter (`debug` for `release` in the tree's
and the archive's names). Only CMake and CPack open the paths beyond 259 (they install, archive,
unpack, hash, compare and remove the environment's files), so the jobs rely on CMake and CPack
handling long paths: praenora-runtime-pyenv's workflow shows that CPack archives paths of about 300
characters on `windows-2022` (not yet verified for this workflow). The Python files that an
installation's programs open stay below 259: the deepest file of numpy, which the tests' Python
modules import, is at about 230 in the moved installation. Shortening the work directories of the
tests would not bring the longest paths below 259, nor would a step like praenora-runtime-deps'
`Shorten the path of the checkout`, which saves 36 characters. Should a Windows job fail on a path
that is too long, its log names the path and the program that failed; the remedies, long paths
enabled in Windows (`LongPathsEnabled`) for that program or shorter directories in CPack's staging,
need the maintainer's decision.

## The dependency bundles

[`cmake/PraenoraRuntimeDepsPin.cmake`](../cmake/PraenoraRuntimeDepsPin.cmake) pins a release of
praenora-runtime-deps (`PRAENORA_RUNTIME_DEPS_VERSION`, now 0.1.1) and, for each platform key of
`PRAENORA_RUNTIME_DEPS_PLATFORMS`, the name and SHA-256 of its archive there. Each build job runs
[`cmake/praenora_fetch_runtime_deps.cmake`](../cmake/praenora_fetch_runtime_deps.cmake) for its own
key, so it builds against the bundle of the same name: `linux-x64-release` against the bundle
`linux-x64-release`, and so on. The script downloads the archive from the release, checks its
SHA-256 against the pin, unpacks it into the runner's temporary directory and appends
`PRAENORA_RUNTIME_DEPS=<bundle>` to the file that `GITHUB_ENV` names, so the later steps have the
variable that the CI presets read
([developer-guide.md](developer-guide.md#using-a-dependency-bundle)). The deps repository is
public, so no token is needed.

The runner labels are those of praenora-runtime-deps' workflow, fixed image versions, never
`ubuntu-latest`, `macos-latest` or `windows-latest`. A bundle holds static libraries, built with
the compiler of its runner image, which its name gives, and the runtime must be built with that
compiler or a newer one of the same family: GCC 13 on `ubuntu-24.04`, the AppleClang of
`macos-26`, and Visual Studio 2022 on `windows-2022` (Visual Studio 2022 cannot link static
libraries that Visual Studio 2026 built, as on `windows-2025`). The same labels give the same
compilers, as long as GitHub keeps the images' compilers. The macOS bundle of 0.1.1 is named
`appleclang21`, while praenora-runtime-deps' own documentation expects AppleClang 17 on
`macos-26`: the image's compiler is newer than that documentation says, and the runtime's macOS
job, on the same label, got AppleClang 21.0.0, the major version that the bundle's name gives. The
test
`ci.workflow_consistency` checks that the workflow's matrix keys are the platforms of the pin,
each with its `ci-<key>` presets, and that each key runs on the label of its operating system
([testing.md](testing.md#labels)).

## The Python environment bundles

[`cmake/PraenoraRuntimePyenvPin.cmake`](../cmake/PraenoraRuntimePyenvPin.cmake) pins a release of
[praenora-runtime-pyenv](https://github.com/INFLECT-Praenora/praenora-runtime-pyenv)
(`PRAENORA_RUNTIME_PYENV_VERSION`, now 0.1.0, the release v0.1.0, published on 2026-11-25 and built
from its commit `1120cc4`) and, for each platform key of `PRAENORA_RUNTIME_PYENV_PLATFORMS` (the
same keys as the dependency bundles'), the name and SHA-256 of its archive there. The environment
is built in Release only, so `windows-x64-debug` names the archive of `windows-x64-release`:

| Key | Archive |
|---|---|
| `linux-x64-release` | `praenora-runtime-pyenv-0.1.0-linux-x86_64-cp312-gcc13-release.tar.xz` |
| `macos-arm64-release` | `praenora-runtime-pyenv-0.1.0-macos-arm64-cp312-appleclang21-release.tar.xz` |
| `windows-x64-release`, `windows-x64-debug` | `praenora-runtime-pyenv-0.1.0-windows-x86_64-cp312-msvc143-release.zip` |

Each build job runs
[`cmake/praenora_fetch_runtime_pyenv.cmake`](../cmake/praenora_fetch_runtime_pyenv.cmake) for its
own key, after the dependency bundle's script, with which it shares its implementation
([`cmake/praenora_fetch_bundle.cmake`](../cmake/praenora_fetch_bundle.cmake)): it downloads the
archive from the release, checks its SHA-256 against the pin, unpacks it into the runner's
temporary directory, checks that the bundle's manifest, `praenora-runtime-pyenv.json`, names the
pinned version, and appends `PRAENORA_RUNTIME_PYENV=<bundle>` to the file that `GITHUB_ENV` names,
so that the CI presets find the environment's CMake package
([developer-guide.md](developer-guide.md#ci-presets)). The pyenv repository is public, so no token
is needed. Its workflow runs on the same runner labels as praenora-runtime-deps', and its bundles
name the same compilers (GCC 13, AppleClang 21, MSVC toolset 143); the environment's CMake package
refuses a build for another platform, an older macOS deployment target than the bundle's (14.0, as
`ci-macos-arm64-release` sets) or an older MSVC toolset. CI's use of the pin is not yet verified:
no run of the workflow has fetched it ([What the runs verified](#what-the-runs-verified)). The test
`ci.workflow_consistency` checks that the platforms of both pins are the workflow's matrix keys.

## Raising the deps pin

The pin is changed only after the maintainer confirms the change ([AGENTS.md](../AGENTS.md)):
raised to a newer published release of praenora-runtime-deps, or corrected within one. Then, in one
pull request:

1. **Read the release's archives.** Their names, and the SHA-256 that GitHub records for each
   asset (its `digest`, `sha256:<hash>`):

   ```sh
   gh release view v<deps version> --repo INFLECT-Praenora/praenora-runtime-deps --json assets
   ```

   or the release's `.sha256` files, one beside each archive.
2. **Update the pin file**: `PRAENORA_RUNTIME_DEPS_VERSION`, and for every key the archive's name
   and SHA-256, all together. The names carry the bundles' compilers.
3. **Move the runner labels** if that release moved one of its jobs to another runner image (its
   `docs/releasing.md`, "Changing a runner image"): the job's `runner` in the workflow's matrix
   and the label of its operating system in `tests/ci/check_workflow_consistency.cmake`. If the
   release adds or drops a platform key, add or remove its matrix entry and its `ci-<key>`
   presets with it.
4. **Update this document**: the jobs table in [The workflow](#the-workflow) and the bundle's
   compiler in [The dependency bundles](#the-dependency-bundles), and the documents that name the
   pinned version or an archive name, such as
   [developer-guide.md](developer-guide.md#using-a-dependency-bundle).
5. **Run the pull request's checks**: every job builds and tests the runtime against the new
   bundles; `ci.workflow_consistency` checks that the pin, the presets and the workflow agree.

## Raising the Python environment pin

The pin of the Python environment bundles is changed only after the maintainer confirms the change
([AGENTS.md](../AGENTS.md)), as the deps pin is: raised to a newer published release of
praenora-runtime-pyenv, or corrected within one. Then, in one pull request:

1. **Read the release's archives.** Their names, and the SHA-256 that GitHub records for each
   asset (its `digest`, `sha256:<hash>`):

   ```sh
   gh release view v<pyenv version> --repo INFLECT-Praenora/praenora-runtime-pyenv --json assets
   ```

   or the release's `.sha256` files, one beside each archive. The Windows `.sha256` file of
   v0.1.0 ends in a carriage return and a line feed, which `shasum -a 256 -c` on macOS refuses;
   the pin holds the value itself, so read it from the file rather than checking with it.
2. **Update the pin file**: `PRAENORA_RUNTIME_PYENV_VERSION`, and for every key the archive's name
   and SHA-256, all together; both Windows keys name the Windows archive while the environment is
   built in Release only. The names carry the bundles' platforms, CPython versions and compilers.
3. **Check the runner labels and the environment's requirements**: if that release moved one of
   its jobs to another runner image or compiler, or raised its macOS deployment target, the
   runtime's jobs and presets must still meet them (the environment's CMake package says what it
   refuses). A new CPython minor version changes the interpreter that the provider `python` and
   every Python module run, so the Python modules' tests are the ones to read in the pull
   request's runs.
4. **Update this document**: [The Python environment bundles](#the-python-environment-bundles),
   and the documents that name the pinned version or an archive name.
5. **Run the pull request's checks**: every job builds and tests the runtime against the new
   environment; `ci.workflow_consistency` checks that the pins, the presets and the workflow agree.

## Artifacts and releases

The artifacts of a run are for checking that run's archives: they can be downloaded from the run's
page or with `gh run download <run id>`, and they expire after 7 days. The archives of a pull
request are built from the merge commit that GitHub makes for it, which is on no branch. Users take
archives from releases only.

## Required checks

The jobs report the checks `Prepare`, `linux-x64-release`, `macos-arm64-release`,
`windows-x64-release` and `windows-x64-debug`; `Draft release` runs only with `publish`. The
workflow has no path filters, so every pull request reports all of them, and the ruleset on `main`
can require them.

## Repository settings

The release procedure relies on these settings:

- **Rulesets.** `main-pr`: `main` takes changes only through pull requests, with one approval; it
  requires no checks yet, and an administrator may make it require those above. `v-tag`: tags
  `v*` are created, moved or deleted only by administrators; publishing a draft release creates
  its tag, so the administrators who publish releases must be allowed to create tags.
- **Releases.** Immutable releases on: once published, a release's assets and tag cannot change.
- **Actions.** The workflow asks for `contents: read`, and the release job for `contents: write`
  itself. If the allowed actions are restricted, `actions/checkout`, `actions/setup-python`,
  `actions/upload-artifact` and `actions/download-artifact` are allowed; the workflow pins each to
  a commit, with its version in a comment, which `ci.workflow_consistency` checks.

## Release procedure

1. **Prepare the change.** The runtime package's version, `project(VERSION)` in `CMakeLists.txt`,
   is raised for a release, when the maintainers decide it, and is the only version that is
   ([AGENTS.md](../AGENTS.md),
   [specs/compatibility-policy.md](specs/compatibility-policy.md#changing-a-version)).
   In a pull request to `main`, raise it, together with the documentation, fixtures and golden
   files that name it; the pull request's checks build and test every platform. The first
   release, v0.2.0, was made at the current version without a raise; every later release raises
   it, since a run with `publish` stops when the tag of its version exists.
2. **Merge** the pull request into `main`.
3. **Run the workflow with `publish`** on `main`: in the Actions tab, *Build*, *Run workflow*,
   branch `main`, with *Create a draft release* checked; or

   ```sh
   gh workflow run build.yml --ref main -f publish=true
   ```

   The run stops at once if the tag `v<version>` exists; then the version was not raised.
4. **Check the draft release** `v<version>` when the run has finished. It holds four archives and
   four `.sha256` files.
   - The archive names carry the version and the expected compiler labels
     ([The workflow](#the-workflow)). Another label means that a runner image changed its
     compiler, so that the runtime and the bundles may differ in compilers: compare the labels with
     those of the bundles before publishing ([Raising the deps pin](#raising-the-deps-pin)).
   - Download the archive of your platform and check it:

     ```sh
     gh release download v<version> --repo INFLECT-Praenora/praenora-runtime \
         --pattern '*-macos-arm64-*'
     shasum -a 256 -c praenora-runtime-<version>-macos-arm64-appleclang21-release.tar.xz.sha256
     ```

   - Unpack it, then run its `bin/praenora version` and `bin/praenora providers list` (the three
     providers, `roadrunner`, `antimony` and `python`, usable), and the example workflow with that
     tool, given the example modules of a
     local build of the same commit with `--module-path` (the archive holds none)
     ([user-guide.md](user-guide.md#getting-the-tool),
     [user-guide.md](user-guide.md#running-the-example-workflow)). On a Mac with Homebrew's
     libraries this does not show whether the archive needs them; each build job's step
     *Check the archive's libraries* does ([The workflow](#the-workflow)), and its log lists the
     libraries of each file.
   - Check that the archives may be published under the licenses of what they hold
     ([developer-guide.md](developer-guide.md#license-files)): for the NLEQ1 and NLEQ2 solvers
     that libRoadRunner links, this is open (OQ-27), and libSBML (LGPL-2.1) is linked statically
     by both providers (OQ-27, OQ-29) ([open-questions.md](open-questions.md)). cpp-httplib (MIT)
     is compiled in from its header (into the runtime library, the tool and the module host) and
     taken from its pinned commit, so its license is in `share/praenora/licenses/httplib/`, as
     yaml-cpp's and spdlog's are (not yet verified in an archive of the workflow). The Python
     environment holds CPython and the packages it pins, each under its own license, with their
     license files in `share/praenora/licenses/python-environment/` (not yet verified in an
     archive of the workflow).
5. **Publish** the release, after editing its notes if needed. Publishing creates the tag
   `v<version>` on the built commit; the release then cannot change.

## If a job fails

If a build job fails, the run creates no release; the job's log shows the failing step, and its
artifact is not uploaded. If Prepare stops a run with `publish` because `git ls-remote` could not
list the tags, see [The workflow](#the-workflow): the check needs a public repository. A draft
that should not be published is deleted with
`gh release delete v<version> --repo INFLECT-Praenora/praenora-runtime`; it has no tag, so the
workflow can then run again for the same version. A published release with a defect is replaced by
a new version. Releases, `v*` tags and release assets are made only by this workflow and by
publishing its drafts, never by hand ([AGENTS.md](../AGENTS.md)).
