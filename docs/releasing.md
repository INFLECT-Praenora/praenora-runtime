# Releasing

This document describes how GitHub Actions builds and tests the runtime against the dependency
bundles of [praenora-runtime-deps](https://github.com/INFLECT-Praenora/praenora-runtime-deps), and
how an administrator publishes a release with the archives it makes. Running the same presets on
your own machine is in [developer-guide.md](developer-guide.md#ci-presets), what an archive holds in
[developer-guide.md](developer-guide.md#archiving-an-installation), and checking and running one in
[user-guide.md](user-guide.md#getting-the-tool). The decisions behind the workflow are in
[ADR-0041](adr/ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md).

Not yet verified: the workflow has not run on GitHub yet. Its Linux and Windows jobs are the first
builds of those platforms, and its jobs are the first to download a bundle from its release and to
build yaml-cpp, spdlog and GoogleTest from downloaded sources. No release has been made.

## The workflow

[`.github/workflows/build.yml`](../.github/workflows/build.yml) (named "Build") runs:

- on every pull request to `main`;
- when run manually, on any branch: in the repository's Actions tab, *Build*, *Run workflow*; or
  `gh workflow run build.yml --ref <branch>`. GitHub offers the manual run once the workflow is on
  `main`. With the input `publish`, which only `main` accepts, the run also creates a draft
  release.

Its jobs:

1. **Prepare** reads the runtime's version from `project(VERSION)` in `CMakeLists.txt`, and the
   version and repository of the dependency bundles from
   [`cmake/PraenoraRuntimeDepsPin.cmake`](../cmake/PraenoraRuntimeDepsPin.cmake). With `publish`,
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

   | Job | Runner | Compiler (expected) | Archive |
   |---|---|---|---|
   | `linux-x64-release` | `ubuntu-24.04` | GCC 13 | `praenora-runtime-<version>-linux-x86_64-gcc13-release.tar.xz` |
   | `macos-arm64-release` | `macos-26` | AppleClang 21 | `praenora-runtime-<version>-macos-arm64-appleclang21-release.tar.xz` |
   | `windows-x64-release` | `windows-2022` | MSVC of Visual Studio 2022 (toolset 143) | `praenora-runtime-<version>-windows-x86_64-msvc143-release.zip` |
   | `windows-x64-debug` | `windows-2022` | MSVC of Visual Studio 2022 (toolset 143) | `praenora-runtime-<version>-windows-x86_64-msvc143-debug.zip` |

   The compilers are those that the archive names of the pinned bundles give
   ([The dependency bundles](#the-dependency-bundles)), which these images are expected to have
   still; the archive names of the first run will show them. None of these jobs has run yet. Each
   job turns off Git's line-ending conversion (Windows), checks out the repository, sets up
   Python 3.12 with `actions/setup-python`, installs Ninja and SWIG with apt (Linux), those of
   them that are missing with Homebrew (macOS), or SWIG with Chocolatey (Windows, whose presets
   use the Visual Studio 2022 generator, which finds MSVC without a developer environment),
   fetches the bundle of its key, and runs, with `<key>` its key:

   ```sh
   cmake -DPLATFORM=<key> -DDESTINATION=<runner temp>/deps \
       -P cmake/praenora_fetch_runtime_deps.cmake
   cmake --preset ci-<key> -DPython3_EXECUTABLE=<the Python set up>
   cmake --build --preset ci-<key> --parallel
   ctest --preset ci-<key>
   cmake --build --preset ci-<key> --target package    # PACKAGE on the windows-* keys
   ```

   The presets are those of [developer-guide.md](developer-guide.md#ci-presets): both providers,
   the Python bindings and the tests, with warnings as errors, and yaml-cpp, spdlog and GoogleTest
   built from their pinned commits. `ctest` runs every test of the tree.

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
   | Windows | The API sets (`api-ms-win-*`, `ext-ms-*`), the Visual C++ runtime and its debug libraries (`msvcp140*`, `vcruntime140*`, `concrt140*`, `ucrtbase*`), `praenora_runtime.dll` from the archive's `bin\`, and the DLLs in `%SystemRoot%\System32`. |

   Any other library fails the step, which names it with the file that needs it, as does a file
   whose list cannot be read or is empty. On macOS arm64 the step's script was run on the
   archive of a local `ci-macos-arm64-release` tree configured with Homebrew's yaml-cpp and
   spdlog ([developer-guide.md](developer-guide.md#ci-presets)), where it fails, naming
   Homebrew's yaml-cpp, spdlog and fmt for the runtime library and the module host, and, with
   those entries left out of `otool`'s output, passes; its Linux and Windows parts were run only
   on hand-made archives, with scripts that print the output of `readelf` and `dumpbin` in their
   formats. Not yet verified on GitHub.

   Last, the job uploads the archive and its `.sha256` file from `build/ci-<key>/package/` as a
   workflow artifact named after the key, kept for 7 days.
3. **Draft release**, with `publish` only: downloads the archives, checks that their names carry
   the version and that they match their `.sha256` files, unpacks the Linux archive and runs its
   `bin/praenora version --json` and `providers list --json` for the release notes (the versions
   of the runtime, the module SDK, PMP and the module ABI, and each provider with its services and
   engine), names the pinned release of the dependency bundles, and creates the draft release
   `v<version>` of the built commit, with every archive and `.sha256` file. A draft is visible only
   to users with write access, and creates no tag; publishing it creates the tag `v<version>` on
   that commit.

**Path lengths on Windows.** praenora-runtime-deps' workflow maps a drive letter to the parent of
its checkout with `subst`, because `cl` refuses an object file whose path is longer than 259
characters and its object files' paths were longer. This workflow does not: in the checkout
`D:\a\praenora-runtime\praenora-runtime\` (39 characters), the longest path of a
`windows-x64-release` job is estimated at 235 characters, a license file in CPack's staging
directory (`build\ci-windows-x64-release\package\_CPack_Packages\win64\ZIP\<archive name>\` and
then `share\praenora\licenses\libroadrunner-deps\third_party\NuML\libnuml\LICENSE.txt`); 231 in
the staging directory of `install.package` (`build\ci-<key>\pkg\`); about 197 for the files of
FetchContent's sub-build of GoogleTest; and 172 for the longest object file, which `cl` writes. The
`windows-x64-debug` job's paths are 4 characters shorter. These are estimates from the paths of a
macOS build tree (`build/ci-macos-arm64-release`), with the Windows prefix, archive name and
directories of the Visual Studio generator; not yet verified. Should a Windows job fail on a path
that is too long, add a step like the one of praenora-runtime-deps' workflow
(`Shorten the path of the checkout`), and the `working-directory` of the steps that run CMake.

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
job, on the same label, is expected to get the same one (not yet verified). The test
`ci.workflow_consistency` checks that the workflow's matrix keys are the platforms of the pin,
each with its `ci-<key>` presets, and that each key runs on the label of its operating system
([testing.md](testing.md#labels)).

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
   release, v0.2.0, is made at the current version and needs no raise.
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

   - Unpack it, then run its `bin/praenora version` and `bin/praenora providers list` (both
     providers usable), and the example workflow with that tool, given the example modules of a
     local build of the same commit with `--module-path` (the archive holds none)
     ([user-guide.md](user-guide.md#getting-the-tool),
     [user-guide.md](user-guide.md#running-the-example-workflow)). On a Mac with Homebrew's
     libraries this does not show whether the archive needs them; each build job's step
     *Check the archive's libraries* does ([The workflow](#the-workflow)), and its log lists the
     libraries of each file.
   - Check that the archives may be published under the licenses of what they hold
     ([developer-guide.md](developer-guide.md#license-files)): for the NLEQ1 and NLEQ2 solvers
     that libRoadRunner links, this is open (OQ-27), and libSBML (LGPL-2.1) is linked statically
     by both providers (OQ-27, OQ-29) ([open-questions.md](open-questions.md)).
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
