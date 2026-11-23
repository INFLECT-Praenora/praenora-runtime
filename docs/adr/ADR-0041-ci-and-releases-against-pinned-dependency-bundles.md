# ADR-0041: CI and releases against pinned dependency bundles

- Status: Accepted
- Date: 2026-11-21
- Supersedes: none
- Superseded by: none

## Context

Until now every build and test of this repository ran on a developer's machine, on macOS arm64
only: Linux x86-64 and Windows x86-64 have presets and portable code but have never been built, and
there were no release archives. The engines of the service providers (libRoadRunner with its LLVM,
libAntimony) and json-schema-validator come from no package manager ready to use, so a build on
another machine first needs them built.

The repository praenora-runtime-deps builds them, with nlohmann/json, into one dependency bundle
per platform key (`linux-x64-release`, `macos-arm64-release`, `windows-x64-release`,
`windows-x64-debug`). Its workflow "Build" builds and checks every bundle on each pull request to
`main` and, run by hand on `main` with the input `publish`, drafts a release `v<version>` with the
archives, which an administrator publishes. Its `docs/outputs.md` ("From a release") says how the
runtime consumes a bundle: pin the release and, per platform, the archive's name and SHA-256, and
check a download against it.

The maintainer asked for the same kind of automation here: a build on every pull request to
`main`, and a drafted release with a version tag and archives from `main`, built against the
bundles of a pinned release. The change rules of AGENTS.md froze every version, the runtime
package's included, so a release could never carry a new version.

## Decision

Accepted by the maintainer on 2026-11-22, after the workflow passed on every platform key and the
first release, v0.2.0, was published ([releasing.md](../releasing.md#what-the-runs-verified)). It
answers how CI builds and how releases are made, the release-archive part of OQ-13, OQ-27 and
OQ-29, and resolves none of them: wheels, manylinux, signing and the license review of
distributing the providers stay open. The builder notes are Proposed.

- **The workflow.** [`.github/workflows/build.yml`](../../.github/workflows/build.yml), named
  "Build", mirrors praenora-runtime-deps' workflow: it runs on each pull request to `main` and by
  hand on any branch, with `permissions: contents: read` and one run per pull request or ref
  (`concurrency`). Its job `Prepare` reads `project(VERSION)` and the pin; one build job per
  platform key, named after the key, on the runner of the same key in praenora-runtime-deps'
  workflow (`ubuntu-24.04`, `macos-26`, `windows-2022`), fetches the bundle of its key, configures,
  builds and tests with the presets `ci-<key>` (both providers, the Python bindings, every test,
  warnings as errors), archives the installation with the target `package` (`PACKAGE` with Visual
  Studio), checks that the archive needs only the system's libraries and its own, and keeps the
  archive and its `.sha256` file as a workflow artifact for 7 days. With
  `publish`, accepted on `main` only and only while the tag `v<version>` does not exist, the job
  `Draft release` checks the archives' names and SHA-256 and creates the draft release
  `v<version>` of the built commit with every archive. An administrator publishes it, which
  creates the tag. Each action is pinned to a commit, with its version in a comment.
- **The pin.** [`cmake/PraenoraRuntimeDepsPin.cmake`](../../cmake/PraenoraRuntimeDepsPin.cmake)
  pins praenora-runtime-deps 0.1.1 and the names and SHA-256 of its four archives, and
  [`cmake/praenora_fetch_runtime_deps.cmake`](../../cmake/praenora_fetch_runtime_deps.cmake)
  downloads, checks and unpacks one of them, for the jobs and for developers. The pin is changed
  only after the maintainer confirms the change, together with the runner labels when that release
  of praenora-runtime-deps moved its jobs to other runner images
  ([releasing.md](../releasing.md#raising-the-deps-pin)).
- **The version exemption.** The runtime package's version, `project(VERSION)` in
  `CMakeLists.txt`, is raised when a release is made, by the maintainers' decision, following
  [releasing.md](../releasing.md#release-procedure); every other version (the SDK, PMP, the module
  ABI, the provider interface, the services, the document formats, the catalog entries and this
  repository's modules) stays frozen. The first release is v0.2.0, the current version, made
  without a raise. Decided by the maintainer on 2026-11-21.
- **Releases only from the workflow.** No release, `v*` tag or release asset is made by hand: a
  release is a draft of the workflow, published by an administrator.
- **A consistency test.** `ci.workflow_consistency` (label `ci`) checks, offline, that the
  workflow's matrix keys are the platforms of the pin, each with its `ci-<key>` presets, that each
  key runs on the runner label of its operating system and every job on a fixed label, and that
  every action is pinned to a commit with its version
  ([testing.md](../testing.md#labels)).

### Builder notes (Proposed)

- **A CMake fetch script** rather than `gh release download` and `shasum` in the workflow:
  `file(DOWNLOAD)` and `file(ARCHIVE_EXTRACT)` behave the same on the three platforms, where the
  shells differ (bash, and PowerShell on Windows); praenora-runtime-deps is public, so no token is
  needed; the repository's other drivers are CMake scripts already; a developer fetches the same
  bundle with the same command; and the test `ci.fetch_runtime_deps` runs the script offline on
  `file://` URLs. The pin is a CMake file that the script includes, rather than values in the
  workflow's YAML or a JSON file, which the workflow and Windows would each need a reader for. The
  script checks the SHA-256 itself rather than with `file(DOWNLOAD EXPECTED_HASH)`, so that its
  error gives both values and leaves no file, and tries a failed or stalled transfer again, up to
  3 attempts.
- **yaml-cpp, spdlog and GoogleTest are built from their pinned commits** in every job, with
  `PRAENORA_FETCH_DEPENDENCIES` and `CMAKE_DISABLE_FIND_PACKAGE_<name>` for each (`googletest`,
  FetchContent's content name, for GoogleTest), rather than taken from apt, Homebrew or vcpkg: the
  archives then link yaml-cpp and spdlog statically and need only the system's libraries, what a
  job builds does not depend on the packages of its runner image, and these jobs are the first to
  build the fetched dependencies. nlohmann/json and json-schema-validator come from the bundle,
  which is first on `CMAKE_PREFIX_PATH`.
- **A step checks the archive's libraries** after the archive is made, since no test of the tree
  shows what an archive needs, and the smoke test of the release procedure, on a Mac with
  Homebrew, would pass with an archive that loads Homebrew's libraries. A bash step unpacks the
  archive and lists what each program and library of `bin/`, `lib/` and `libexec/` names, with
  `readelf -d`, `otool -L` or `dumpbin -dependents` (found with `vswhere`), and accepts only the
  system's libraries (glibc's, `libstdc++` and `libgcc_s` on Linux; `/usr/lib/*` and
  `/System/*` on macOS; the API sets, the Visual C++ runtime and the DLLs of
  `%SystemRoot%\System32` on Windows) and those the archive holds (on Windows only
  `praenora_runtime.dll`) ([releasing.md](../releasing.md#the-workflow)). Windows
  is checked too rather than left out: CMake searches the prefixes of the directories on `PATH`
  on every platform, and a runner image can have other software's libraries in some of them.
- **The runners follow praenora-runtime-deps.** A bundle holds static libraries built with the
  compiler of its runner image, and the runtime must be built with that compiler or a newer one of
  the same family; the same fixed labels give the same images, so the runtime's jobs use exactly
  the labels of praenora-runtime-deps' jobs, never `-latest`. The macOS bundle of 0.1.1 is named
  `appleclang21`, although praenora-runtime-deps' documentation expects AppleClang 17 on
  `macos-26`: the name, not that documentation, gives the compiler of the image
  ([releasing.md](../releasing.md#the-dependency-bundles)).
- **Windows** uses the Visual Studio 2022 generator of the project's Windows presets, which finds
  MSVC itself, so the jobs need neither Ninja nor the developer environment that
  praenora-runtime-deps' jobs enter; the archive's target is `PACKAGE` there. No step shortens the
  checkout's path with `subst`, as praenora-runtime-deps' does for its object files: the longest
  path of a Windows job is estimated at 235 characters (a license file in CPack's staging
  directory) and the longest object file path at 172, below the limit of 259
  ([releasing.md](../releasing.md#the-workflow)).
- **Python** comes from `actions/setup-python` (3.12), whose `python-path` is passed as
  `Python3_EXECUTABLE`; the CI presets do not set it. The build step passes `--parallel`, so that
  MSBuild builds projects side by side (`/m`); Ninja builds in parallel without it.
- **`Prepare` reads the pin** (its version and repository, with `sed`) for the release notes,
  since the release job has no checkout. It tells `git ls-remote --exit-code` exiting with 2 (no
  such tag) from any other failure, which stops the run, rather than taking every failure for a
  missing tag. The checkout keeps no credentials, so `git ls-remote` needs a public repository,
  as this one is; a private one would need the job's token for the check, through `gh api`
  ([releasing.md](../releasing.md#the-workflow)).
- **The release notes** give the versions of the runtime, the module SDK, PMP and the module ABI,
  and each provider with its services and engine, from `praenora version --json` and
  `praenora providers list --json` of the Linux archive, run on the release job's runner, the image
  the archive was built on; the pinned release of praenora-runtime-deps; and the license
  paragraph: this repository's files are MIT, the runtime (yaml-cpp, spdlog with the fmt library
  it bundles, json-schema-validator, nlohmann/json) and the providers (libRoadRunner with LLVM,
  libSBML and libroadrunner-deps' other libraries; libAntimony) hold third-party code, linked
  statically or compiled in from its headers, that keeps its own licenses, whose files each
  archive holds in `share/praenora/licenses/`, and the terms of NLEQ1 and NLEQ2 are open
  (OQ-27). `praenora version` reports no version of the provider interface, so the notes give
  none.
- **`ci.workflow_consistency` reads the workflow as text**, line by line, since CMake reads no
  YAML; the workflow's header comment states the format it relies on (a line `- key: <key>`
  followed by `runner: <label>` for each matrix entry, `runs-on: <label>`, and
  `uses: <owner>/<name>@<commit> # v<version>`, each value on the line of its key), and the test
  refuses a key whose value is on a later line. It replaces `\`, `;`, `[` and `]` before it
  splits the text into lines, so that no line joins the next or splits into two.
- **Warnings on Linux and Windows.** The first jobs compiled the code with GCC and MSVC, with
  warnings as errors, for the first time. A warning is fixed in the code; it is turned off only
  where its every report here is a false alarm or comes from code that is not this project's, and
  then for no more than that code ([platforms.md](../platforms.md#common-to-every-platform)):
  GCC 13's `-Wdangling-reference` for C++ (`cmake/PraenoraWarnings.cmake`), whose reports here are
  references to objects that outlive the call, such as a provider that `load_provider()` keeps
  loaded; MSVC's C4702 (unreachable code) for the targets that compile libRoadRunner's headers,
  through the interface target `praenora_roadrunner_engine` (`cmake/PraenoraRoadrunner.cmake`),
  since its `rr/Setting.h` gives it where its templates are instantiated, out of reach of the
  warning level of external headers; and the wrapper that SWIG generates, compiled with the
  compiler's default warnings and never as errors, while the facade it wraps is an object library
  of its own with the project's warnings (`src/python/CMakeLists.txt`). MSVC's deprecation of
  `getenv()` (C4996) is turned off around each call alone, with `#pragma warning(push)`,
  `disable` and `pop`, rather than with `suppress`, whose effect depends on what MSVC takes for
  the line after it.
- **MSVC's traditional preprocessor is kept**, rather than the conforming one
  (`/Zc:preprocessor`), which every target and the headers it includes would go through for the
  first time. The traditional one stringizes a macro argument that holds a raw string with quotes
  inside it as if those quotes began or ended string literals, so an assertion of GoogleTest, which
  stringizes its condition, breaks where a backslash that it then takes for code comes before a
  quote. Such an assertion builds its expected text in a variable first.
- **What the runs verified** is kept in [releasing.md](../releasing.md#what-the-runs-verified), and
  each platform's status in [platforms.md](../platforms.md#verification-status), not in these
  notes. The runs that failed found warnings, a few defects of the runtime and its build, and
  tests that assumed POSIX paths and behavior; the fixes are in the repository's history, and the
  calls they needed are the two notes above.

## Alternatives considered

- **The pin in the workflow, with `gh release download` and `shasum`**, as praenora-runtime-deps'
  documentation shows: simple, but the shell commands differ on Windows, the pin lives in YAML
  only, and developers cannot reuse it.
- **The pin in a JSON file read with `jq`**: the workflow could read it, but Windows and
  developers would need a second reader.
- **yaml-cpp, spdlog and GoogleTest from the runners' package managers**: the archives would load
  shared libraries that they do not hold, from places that differ between images.
- **`-latest` runner labels**: GitHub moves them to newer images, whose compilers can be newer or
  of another toolset than the bundles', without a change in this repository.
- **A release on each push to `main`, or a workflow started by pushing a tag**: releases would not
  wait for the maintainers' decision, and the tag would exist before the archives were checked;
  publishing a draft creates the tag, which the ruleset `v-tag` and immutable releases protect.
- **Ninja on Windows, in the Visual Studio developer environment**, as praenora-runtime-deps does:
  the project's Windows presets use the Visual Studio generator, and the CI presets inherit them.
- **A second workflow for releases**: the archives of a release would not be those that the
  pull request's jobs built and tested with the same steps.

## Consequences

- Every pull request to `main` builds and tests the runtime on four platform keys, with both
  providers, and reports the checks `Prepare`, `linux-x64-release`, `macos-arm64-release`,
  `windows-x64-release` and `windows-x64-debug`, which the ruleset on `main` can require.
- Linux and Windows get their first builds, by CI, which verify them
  ([platforms.md](../platforms.md#verification-status)).
- Releases carry one archive per platform key with its `.sha256` file; wheels, manylinux and
  code signing stay open (OQ-13).
- A job needs the network: it downloads the bundle and the sources of yaml-cpp, spdlog and
  GoogleTest. The CI presets opt in to this; a default build and the tests still use no network.
- The version freeze has one exemption, the runtime package's version for a release
  ([specs/compatibility-policy.md](../specs/compatibility-policy.md#changing-a-version)).
- Raising the pin is a deliberate change, confirmed by the maintainer, in which the pin, the
  presets and the workflow's matrix and runners move together, and `ci.workflow_consistency` checks
  that they agree.
- The license questions of distributing the providers stay open, and the release procedure checks
  them before each release is published (OQ-27, OQ-29).

## References

- [releasing.md](../releasing.md): the workflow, the dependency bundles, raising the pin, the
  release procedure.
- [developer-guide.md](../developer-guide.md#using-a-dependency-bundle): using a dependency
  bundle; [CI presets](../developer-guide.md#ci-presets);
  [archiving an installation](../developer-guide.md#archiving-an-installation).
- [ADR-0017](ADR-0017-dependency-management.md): dependency management;
  [ADR-0040](ADR-0040-mit-license.md): the MIT license and the license files of installations.
- [open-questions.md](../open-questions.md): OQ-13, OQ-27, OQ-29.
- praenora-runtime-deps: its `.github/workflows/build.yml`, its `docs/releasing.md` ("Changing a
  runner image") and its `docs/outputs.md` ("From a release").
