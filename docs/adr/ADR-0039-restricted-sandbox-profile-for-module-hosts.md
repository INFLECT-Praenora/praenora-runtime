# ADR-0039: A restricted sandbox profile for module hosts

- Status: Proposed
- Date: 2026-11-20
- Supersedes: none
- Superseded by: none

## Context

A step can run its module instance in a module host, a process of its own
([ADR-0037](ADR-0037-process-isolation-of-module-instances.md)): a crash then ends only that
process. The module's code still has every right of the runtime's user in that process: it can
open network connections, start programs and write any file the user may. A configuration that
runs a module it trusts less wants to take those rights away, without changing the module, and
without taking away what scientific engines need: reading files, loading libraries, and generating
code at run time (libRoadRunner compiles models with LLVM's MCJit, which writes code into memory and
then makes it executable).

macOS can confine a process with a sandbox profile (`sandbox_init()`, the system's sandbox profile
language). A process confines itself for good, and one that runs in a sandbox already cannot apply
another (`EPERM`). Linux and Windows have other mechanisms (seccomp, Landlock; job objects,
AppContainer), which this version does not use.

## Decision

- **A per-step setting.** The configuration's `isolation` object gains the field `sandbox`, `"none"`
  (the default) or `"restricted"` (`workflow-config` 0.1, changed in place), resolved field by
  field like `mode` and `share_compiled_models`. It applies only to a step in mode `process`:
  validation refuses `restricted` on a step in mode `in-process` (`CONFIG_INVALID`, at the field
  that set it), and where the platform has no restricted profile (anything but macOS in this
  version) on a step in a module host (`UNSUPPORTED`, at that field). The caller's override
  `in-process` sets every step's profile to `none`. The run record's `execution.isolation.<step>`
  always holds `sandbox` (`run-record` 0.2, changed in place); it is not part of the fingerprint.
- **The profile.** Allowed by default; denied: `network*`, `process-fork`, `process-exec`, the
  system's other ways to start a process for it (`job-creation`, `lsopen`, `appleevent-send`), and
  `file-write*` except below the module host's temporary directory and to `/dev/null`. Reads,
  `dlopen()`, `MAP_JIT` memory and memory made executable with `mprotect()`, and the descriptors
  the module host already has (its channel, standard output and error) keep working.
- **What it is not.** The profile limits what the module host's own process does: it keeps a
  module that keeps to its documents from the network, from starting programs and from changing
  files. It is not a boundary against a module meant to do harm: `(allow default)` leaves it the
  system services it can reach over Mach IPC, which act for it with their own rights.
- **Applied by the module host, first.** The module host applies the profile to itself before it
  loads any provider or module library, so that their static initializers already run under it.
- **The module host's own temporary directory.** For a module host under the profile, the runtime
  makes a temporary directory of its own, below the real path of the system's temporary directory;
  the module host's `TMPDIR` names it, and it is removed once the module host has ended, with a
  warning for what cannot be removed. A runtime that is killed leaves it behind.
- **Failure.** A profile that cannot be applied, or a directory that cannot be made, fails the
  step's load with `MODULE_HOST_UNAVAILABLE`, `details.reason` `sandbox`, with the system's text.
  A runtime that runs in a sandbox already therefore cannot run a step under the profile.
- **Not verified for signed programs.** A module host signed with the hardened runtime would need
  the entitlements `com.apple.security.cs.allow-jit` (or
  `com.apple.security.cs.allow-unsigned-executable-memory`) and
  `com.apple.security.cs.disable-library-validation` to generate code and to load modules and
  providers that are signed otherwise; this is documented, not verified.

The specification is [process-isolation.md](../specs/process-isolation.md#the-restricted-profile).

### Builder notes (Proposed)

- The module host's temporary directory is private: `praenora-module-host-XXXXXX`, made by the
  runtime with `mkdtemp()` (for its owner only) below `std::filesystem::canonical()` of
  `std::filesystem::temp_directory_path()`, so its path is real (`/var` is `/private/var`) and the
  profile, which the system checks against real paths, names it as it is. The module host sets
  `TMPDIR` to it before it applies the profile, so `std::filesystem::temp_directory_path()` names
  it too. It is the module host's alone: a module under the profile cannot write into another
  program's temporary files. The runtime removes it with everything in it
  (`PrivateTemporaryDirectory::remove()`, `src/runtime/isolation/sandbox.hpp`) once the module
  host has been waited for (`~ChildModule`). A module host without a profile keeps the `TMPDIR` it
  inherits, as before. The other choice, the system's temporary directory itself, would let a
  module write into every temporary file of the user.
- A module may change the modes of what it makes there (`file-write-mode` is a write below the
  directory), and one directory of mode 000 with a file in it would keep the directory from being
  removed. So the removal first gives the owner read, write and search rights again on every
  directory in it, top down, judging each by `symlink_status()` and never following a symbolic
  link (the directory itself included, which a module may replace with a link: what a link names
  is not the module host's). What still cannot be removed, such as a file the module made
  immutable (`UF_IMMUTABLE`), is left, and `remove()` says what and why: the owning `ChildModule`
  logs it as a warning (event `module_host.directory_left`, on the step's logger, with the
  directory's path). A runtime that is killed removes nothing: its module hosts' directories stay
  until the system cleans its temporary directory.
- The load request carries the profile's name and the directory; the module host makes the
  profile's text with the runtime's own code (`restricted_profile_text()`) and applies it at the
  start of the load, after the handshake (a module host of another build is refused before it is
  asked anything) and after it has read the module's manifest, which is data, and before any
  provider or module library. The profile applies to the whole process, whose reader and call
  threads exist by then.
- `sandbox_init()` is called with flags 0, which takes the profile's text; its header calls every
  value but `SANDBOX_NAMED` reserved, and the function is deprecated, with no successor for a
  process that confines itself. It sits in one function (`apply_restricted_profile()`), with the
  deprecation warning silenced there. Checked on macOS 15 (Darwin 24.6), arm64. On failure the
  system's library writes a line of its own to standard error ("sandbox initialization failed:
  ..."), which the module host keeps from the runtime's (below).
- The profile's text, exactly: `(version 1)`, `(allow default)`, `(deny network*)`,
  `(deny process-fork)`, `(deny process-exec)`, `(deny job-creation)`, `(deny lsopen)`,
  `(deny appleevent-send)`, `(deny file-write*)`, `(allow file-write* (subpath "<directory>")
  (literal "/dev/null"))`, one rule a line; the last rule that matches an operation decides, so
  the writes allowed at the end are the only ones left. A backslash or a double quote in the
  directory is escaped; a test applies the profile with both, and a character beyond ASCII, in
  the directory's path.
- `(deny job-creation)`: a launchd job, which launchd starts outside the sandbox, would be a new
  process by another way; no engine needs one.
- `(deny lsopen)`: LaunchServices opening an application or a URL in one is a new process too;
  no engine needs it.
- `(deny appleevent-send)`: an Apple event scripts another application, which then acts for the
  module; no engine needs it.
- Links need no rule of their own (checked outside any other sandbox, with the profile's file
  rules): a hard link to a file outside cannot be made in the directory (`EPERM`, and the file
  outside is unchanged), and a symbolic link to one can be made there but not written through
  (`EPERM`). Links within the directory stay allowed. `fx-sandbox-probe` tries both
  (`link_outside`, `write_through_symlink`).
- Opening `/dev/stderr` (and so `/dev/stdout`, `/dev/tty`, `/dev/fd/<n>`) by its path for writing
  is refused, since only `/dev/null` is allowed; writing to the inherited descriptors 1 and 2 works
  (checked before it was built, below). `fx-sandbox-probe` tries to open `/dev/stderr`
  (`open_stderr`), and, with its parameter `write_stderr`, writes a line through descriptor 2:
  `Isolation.TheRestrictedProfileLetsAModuleHostWriteThroughTheStandardErrorItInherits` points its
  own descriptor 2 at a scratch file outside the module host's directory while the module host
  starts (a module host takes descriptors 1 and 2 as they are then), and the line lands in the
  file, which the profile would not let the module host make. A descriptor that the test names could
  not reach the module host instead: the spawn closes every descriptor but 0 to 3.
- The line that macOS's sandbox library writes to standard error when `sandbox_init()` fails is
  kept from the runtime's: the module host points its descriptor 2 at `/dev/null` around that one
  call and restores it after (`QuietStandardError`, `module_host_server.cpp`). That is sound there:
  the reader thread never writes to standard error, and no library of a provider or of the module
  is loaded yet. The error's text still reaches the load's failure. That descriptor 2 is restored
  is checked without writing to it: `fx-sandbox-probe` reports whether its descriptor 2 is
  `/dev/null` (the same device and inode numbers, `stderr_is_null`), and under the profile it must
  be what the runtime's is. A first version of the probe wrote a line to standard error instead,
  which reached the output of every test that ran it and which a descriptor 2 left at `/dev/null`
  would have taken just as well.
- Checked on the machine above, outside any other sandbox, before it was built: under the profile a
  connection to `127.0.0.1` is refused (`EPERM`), and so are `fork()`, `posix_spawn()` and a file
  created outside the directory; a file created in it, reads, writes to inherited descriptors (a
  regular file, a socket pair), `/dev/null`, code written to `MAP_JIT` memory and code made
  executable with `mprotect()` all work. The tests check the same from a module host
  (`fx-sandbox-probe`), and that libRoadRunner simulates, compiling with MCJit, and loads a saved
  state of the shared compiled models, under the profile.
- The errors: `MODULE_HOST_UNAVAILABLE` with `details` `{"reason": "sandbox", "system": <text>}`,
  located in the module's manifest; the module host's message is "module <name>@<version>: the
  module host cannot apply the restricted sandbox profile: <text>" (the system's text is
  "Operation not permitted" in a sandbox already), or "... cannot set its temporary directory:
  ...". The runtime's own refusals use the same reason: "the restricted sandbox profile is
  available only on macOS in this version", "cannot make the module host's temporary directory:
  ...".
- Validation reports one refusal per step and field: a step in mode `in-process` with `restricted`
  gets `CONFIG_INVALID` only, not `UNSUPPORTED` as well off macOS.
- The tests of the profile find out whether their process runs in a sandbox already by asking
  `/usr/bin/sandbox-exec` to run `/usr/bin/true` under a profile that allows everything (it cannot
  in a sandbox), not through the runtime under test; there they skip with a message that begins
  "nested sandbox: this process runs in a sandbox already", which CTest's
  `SKIP_REGULAR_EXPRESSION` names, by that whole start, for the two GoogleTest executables that
  have such tests (`praenora_add_gtest(... SANDBOX_SKIP)`, beside GoogleTest's own skip
  expression), so the skip shows as such and a failing test of another executable cannot be taken
  for one. The test of a sandboxed
  runtime runs either way: in a sandbox it loads under the profile itself; outside it runs a helper
  under `sandbox-exec` (a profile that allows everything), which then is that runtime. The tests of
  a run and of `validate --load` under the profile check the profile's refusals outside a sandbox,
  and the load's `MODULE_HOST_UNAVAILABLE` in one.
- The probe's connection goes to a TCP listener of the test on `127.0.0.1` where the test may open
  one, so that a refusal is the profile's: without a profile the module host must get what the
  test's own process gets for the same connection (a sandbox around the tests may refuse both).
- Writes of the runtime to a module host keep their bounds against a module host that keeps taking
  a few bytes at a time: on macOS the socket's send timeout bounds each wait for room, not a whole
  `sendmsg()`, so a bounded write now waits for room with `poll()` until the end of its slice or
  its deadline, and then sends at most the socket's send low-water mark (`SO_SNDLOWAT`, 2048
  bytes), for which `poll()`'s answer promises room: the send does not wait. The send timeout stays
  as a second bound of one slice. The store's 30 s answer, the 5 ms slices of a call's document
  and the end's request to exit are hard bounds now. 128 MiB pass through a socket pair this way
  in about 0.14 s on the machine above.
- Linux's send timeout does not bound a whole `sendmsg()` either: a stream socket's send takes its
  payload one buffer (about 64 KiB) at a time and restarts the timeout for each buffer it waits
  for, so a peer that frees one every few milliseconds could stretch a send of a large call
  document, or of a store answer of 128 MiB, far beyond its slice or deadline. There a bounded
  write sends with `MSG_DONTWAIT` once `poll()` reported room: the send takes what the stream has
  room for and returns (`EAGAIN` when none, which waits for room again), so it never waits, and
  needs no cut. macOS ignores `MSG_DONTWAIT` on a stream socket pair, so there the cut to the
  low-water mark stays. Written with care for GCC and Clang on Linux, not compiled there in this
  version (OQ-31).
- A stop that comes while the runtime writes a call that its module host has not taken kills the
  module host (ADR-0037, ADR-0038); its instance goes with it. The teardown no longer calls such an
  instance's `finalize` or `destroy` (which would fail, and made the run unhealthy): the instance is
  marked `destroyed` without a call (`ModuleInstance::release_terminated()`, through
  `ModuleCode::terminated()`), a warning says what was left undone (`teardown.skipped`, "is neither
  finalized nor destroyed: its module host was ended when a stop came before it had taken a
  call"), and the run ends `stopped` without a failure.
- A module host that breaks the channel's protocol while it is being ended is killed with a
  warning that names the problem (`module_host.terminated`, "broke the channel's protocol while it
  was being ended: ..."), as the other kills are logged.
- The check that a provider host too short for the store's callbacks offers none runs in a helper
  process of its own (`praenora_isolation_short_host_probe`): a provider is started once in a
  process, and the test process has started the same fixture provider through the provider loader.

## Alternatives considered

- **The system's own profiles** (`kSBXProfileNoNetwork`, `kSBXProfileNoWriteExceptTemporary`): each
  takes one right away, and none refuses new processes; a profile of the runtime's own takes the
  three away at once and names the module host's own directory.
- **`sandbox-exec` around the module host**: a program of its own between the runtime and the
  module host, deprecated too, and the profile would be applied before the module host has checked
  its channel and the build; applying it in the module host keeps the start as it is and still
  comes before any library.
- **The system's temporary directory as the writable place**: a module could then change any of the
  user's temporary files; a directory of the module host's own cannot.
- **Applying the profile in `main()`, before the handshake**: the settings would have to come on
  the command line, and a failure would have no channel to be reported on; at the start of the load
  the failure is an ordinary load error.
- **A profile on Linux** (seccomp, Landlock) **and Windows** (job objects, AppContainer): not in
  this version; validation refuses `restricted` there (OQ-31).

## Consequences

- A step can run a module under the profile on macOS with a line of configuration; the module does
  not change, and its results do not change.
- A module that needs the network, starts programs or writes outside its temporary directory fails
  under the profile; the module developer guide says what it may not do.
- The profile is no boundary against a module meant to do harm: modules that run under it must be
  trusted as code in the runtime's process would be.
- A runtime that itself runs in a sandbox cannot run such a step: its load fails with
  `MODULE_HOST_UNAVAILABLE`, reason `sandbox`.
- Open: Linux and Windows (OQ-31); a signed module host (entitlements documented, not verified;
  OQ-13); finer profiles (allowing a module named reads only, or named network endpoints).

## References

- [docs/specs/process-isolation.md](../specs/process-isolation.md#the-restricted-profile)
- [docs/configuration.md](../configuration.md#isolation)
- [docs/platforms.md](../platforms.md)
- [docs/open-questions.md](../open-questions.md) (OQ-13, OQ-19, OQ-31)
- [ADR-0037](ADR-0037-process-isolation-of-module-instances.md): process isolation of module
  instances
- [ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md): shared compiled models
  between module hosts
