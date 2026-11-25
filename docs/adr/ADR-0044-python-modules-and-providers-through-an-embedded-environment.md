# ADR-0044: Python modules and providers through an embedded Python environment

- Status: Proposed
- Date: 2026-11-24
- Supersedes: none
- Superseded by: none

## Context

Modules and service providers are native libraries. A scientist who writes a model in Python, or a
provider that wants a Python package behind a service's C table, has no path into the runtime. The
repository praenora-runtime-pyenv builds an environment for that: CPython 3.12 with a set of
distributions pinned by a lock file, released as one archive per platform key, a bundle that the
runtime can fetch the way it fetches the dependency bundles of praenora-runtime-deps
([ADR-0041](ADR-0041-ci-and-releases-against-pinned-dependency-bundles.md)).

A Python module should look to its author as a native module looks to a C++ author: a class against
a small adapter, run, validated and checked like any module, in the runtime's process or in a
module host. A run's fingerprint must name the Python code it ran. A process holds one interpreter,
and the interpreter of a process that already runs another one (the Python front end, for example)
cannot be shared.

## Decision

Proposed. The first two choices below were made with the maintainer on 2026-11-24; the details and
the builder notes are Proposed.

- **Services implemented in Python keep their typed C tables.** A provider implemented in Python is
  a native provider library, built in this repository, whose table functions forward to a Python
  object through shared helpers. There is no generic document service in place of a service's table.
- **The pinned environment ships, unchanged, inside the runtime's installation** and so inside its
  release archives ([ADR-0045](ADR-0045-python-environment-pinned-in-ci-and-installation.md)).
- **A runtime service `praenora.python@0`** creates Python objects and calls their methods with JSON
  documents. The adapter of Python modules is built on it, and a C++ module may use it to reach
  Python packages. The provider `python` offers it.
- **One shared internal Python core library** is the only code that links libpython; the providers
  that run Python load it at start, so a process holds one interpreter, one copy of its state and
  one engine per provider. The provider libraries link no Python and load in a process that has no
  environment.
- **A Python module is a class derived from `praenora.sdk.Module`** plus a generated native module
  library that requires `praenora.python@0` and needs no Python to build. `praenora.sdk` is pure
  Python (standard library only), ships in the `praenora-runtime` wheel and, beside the Python
  core, in the installation, and has a test kit, `praenora.sdk.testing`, that runs a module class
  without the runtime.
- **The code a run uses is fixed by its fingerprint.** The directory digest of a module's or
  provider's Python sources is compiled into its library, whose SHA-256 the fingerprint records, and
  checked when an object is created; a path's code is fixed for the life of a process
  ([python-service.md](../specs/python-service.md#the-directory-digest)).
- **The build option `PRAENORA_WITH_PYTHON_ENVIRONMENT`** (off by default) builds what runs Python
  and the tests that need it. The SDK parts that a module's build uses, `python.h`,
  `python_module.hpp`, `praenora_add_python_module()` and the digest function, need no environment
  and are built and installed whatever the option says. The wheel holds `praenora.sdk` whatever it
  says; an installation holds it beside the Python core, so only with the option.
- **The interpreter is configured for repeatable runs.** It is isolated, in UTF-8 mode, writes no
  bytecode and reads none, installs no signal handlers and has the hash seed 0, so the iteration
  order of sets and dicts of strings does not vary between runs; it is never finalized.
- **`print()` and `logging` become log records** of the step whose call runs on the thread, and
  nothing Python writes reaches the process's standard streams, which carry the tool's `--json`
  documents.
- **A process holds one interpreter.** A process that already runs another one (the Python front
  end imported into any interpreter, the bundle's own `python3.12`) cannot start the core: the
  provider's start fails with `UNSUPPORTED` and says to run the step in a module host (isolation
  mode `process`), which is the way out.
- **The environment's path variables and `PYTHONHASHSEED` are set only by `praenora` and
  `praenora-module-host`**, at the start of `main()`, before any thread starts, and only when
  unset; other embedders set them, and the core warns at start about each that is unset or has
  another value. No provider and no core changes the process environment.
- **Child processes** (decided with the maintainer on 2026-11-24): the default start method of
  `multiprocessing` is `spawn`, because `fork` would copy a multi-threaded runtime process and is
  documented as unsupported; children run the environment's own interpreter and import
  `praenora.sdk._child_main` as their main module, which points their standard output at the
  standard error of the runtime's process. There are no sub-interpreters in a process: the numeric
  packages of the environment refuse to load in more than one. The rule approved with these, that
  children are deterministic with the hash seed 0, does not hold: children run isolated (`-I`), as
  their parent is, and an isolated interpreter ignores `PYTHONHASHSEED` and picks a hash seed of
  its own, so code that runs in children must not depend on the iteration order of sets of
  strings. That outcome is Proposed: the builder note **Children of `multiprocessing` stay
  isolated and pick their own hash seeds** gives the call and its reason.
- **A module's self test is opt-in** (`SELF_TEST` in the build call): without it the table's
  `self_test` is absent and the contract checks skip it.
- **No version changes.** The new service starts at `praenora.python@0.0`; no schema file or catalog
  entry is added.

### Builder notes (Proposed)

- **`praenora.sdk.Error.code`** is a `Status` when the number is one of the 13 codes of `status.h`
  and a plain integer otherwise, so module-defined codes from 10000 up pass through unchanged and
  every code compares equal to its integer.
- **`Module` has working defaults**: `__init__(context)` keeps the context, `initialize` and
  `finalize` do nothing, `diagnostics` returns `{}`, and `invoke` fails with `UNSUPPORTED` rather
  than returning nothing. `self_test` has no default: a class that defines none is refused by the
  adapter with `UNSUPPORTED`, which keeps "built with a self test" an explicit statement.
- **Outside the core the package still works.** `_runtime.backend()` gives a backend that drops log
  records and never cancels when no native module is installed, so `_adapter.create()` and the test
  kit run in any interpreter; the test kit hands the adapter a `Host` of its own `FakeHost` and
  never needs the backend.
- **The hooks of the core** are the functions of `praenora.sdk._runtime`: `install(native)`,
  `capture_output()` and `configure_children()`, called once at the core's start;
  `enter_call(host)` and `exit_call()`, which bracket each call; `current_host()`; and
  `load_document(text)` and `dump_document(value)`. Calls nest, the stack is per thread, and
  `current_host()` is `None` outside a call.
- **`Error` and `Cancelled` pickle** from all their fields, so that an error raised in a worker of
  a `multiprocessing` pool reaches the parent instead of killing the pool's result thread.
- **The adapter keeps the host and may be class-only.** The ABI's self test has no instance and no
  create document, so the adapter made without a create document (by `Module._praenora_adapter`,
  through `_adapter.adapter_of()`, or by `_adapter.create()`) does not construct the class and
  serves `self_test` through the class method alone.
- **The test kit takes the runtime's path.** It builds the documents the runtime sends (PMP
  create, initialize and invocation documents, inputs wrapped in envelopes), creates the module
  through `Module._praenora_adapter`, which takes a `host` argument that the runtime never passes,
  calls the adapter's methods, the self test's on a class-only adapter, and writes every document
  and every result as JSON with `praenora.sdk._runtime`'s helpers and reads it back, refusing a
  result above 16 MiB as the core does. A module whose tests pass then reads the same documents in
  a run, and a result that the runtime cannot write (a numpy integer or array, NaN) or a class that
  does not derive from `Module` fails in the kit as in a run, with the runtime's code and message,
  rather than only in a run.
- **A symbolic link in the sources is refused** by the digest function rather than followed or
  skipped: the digest is defined over regular files only, and a rule that a shell command could
  not check would let the runtime and the build disagree.
- **The directory digest in CMake** takes `ROOT` and either `FILES` (absolute, or relative to
  `ROOT`) or `GLOB_ROOT`; it sorts the paths with CMake's case-sensitive string comparison, refuses
  a path that holds a semicolon (CMake lists cannot carry it) and a file outside `ROOT`, and leaves
  out any file below a `__pycache__` directory, also one that `FILES` names.
- **The digest function is installed with the `ModuleSDK` component**
  (`PraenoraPythonDigest.cmake`), because the helper that builds Python modules,
  `PraenoraPythonModule.cmake`, which that component carries, calls it.
- **Its test needs no Python and no option**: `python.digest_cmake` is registered in every tree. The
  fixture's `__pycache__` file is ignored by version control, so the test builds its own copies of
  the fixture and never relies on that file.
- **The core's interface** (`python_core.h`) is `start`, `describe`, `create_object`, `call`,
  `release_object` and `build_id`: the service's functions with the provider's host added to
  `start` and `create_object` and the calling module's host API to `call`, so a provider's table
  forwards to the core without keeping state of its own.
- **Records follow the calling thread.** The core keeps a stack of call targets per thread (the
  call's module host, else the object's, else the provider's), and `praenora.sdk._native` acts for
  the innermost one; outside calls, records go to the provider host of the first start. `print()`,
  `logging` and a module's `Host` then need no object of their own, and a record written on a
  thread that Python code started still reaches a log, never a module host that may be gone.
- **The host is called without the interpreter's lock**, and the sinks are written after it is
  released: a module host's log writes to its channel and must not hold up other Python threads,
  and a large result is copied out once.
- **A lone surrogate becomes U+FFFD**, not the `?` of Python's `replace` handler when it encodes, so
  text that could not be encoded and bytes that could not be decoded look the same in documents
  and records.
- **The output streams, the logging handler and the document helpers are Python code** in
  `praenora.sdk._runtime`, under the package's digest; the core's C++ keeps only what needs the
  host. They are tested without the core, with a fake native module.
- **Each thread has its own unfinished line**, and the end of a call turns it into a record of that
  call, so prints of concurrent calls never mix and a `print(..., end="")` belongs to its call. So
  do bytes that a call leaves in the middle of a UTF-8 sequence: a `flush()` keeps them, since the
  next bytes may complete them, and the end of the call writes U+FFFD for them, so the next call
  on that thread, perhaps another module's, never starts with them.
- **The root logger's level is `NOTSET`** and the handler asks `log_enabled` before it formats a
  record: the host decides what is kept, which a Python level cannot know for each call. Warnings go
  through `logging`, one record each.
- **Documents:** empty argument bytes are `null`; NaN and the infinities are refused in arguments as
  in results, since JSON has neither; results are written without white space, which the runtime
  parses again anyway.
- **An `Error` raised with a code a failure cannot have** (0, or 100 to 9999) fails as `INTERNAL`
  with `details.raised_code`, since 0 would read as success; details that are not a JSON object
  become `{"invalid_details": "<text>"}`.
- **The message of an exception is `"<Type>: <message>"`**, the type with its module unless built
  in, written by the core: `traceback.format_exception_only()` adds notes and, for a `SyntaxError`,
  lines of source.
- **A path's digest is recorded at its first `create_object`, with or without a `path_sha256`**, and
  a later create compares its `path_sha256` with that record, never with the files: the process
  runs the code it imported first, and its record says which.
- **Import isolation is checked before the import and after it**: a package loaded from elsewhere
  already is refused before an import is tried, which would fail with an unrelated error.
- **The core's digest refuses a symbolic link before it skips `__pycache__`**, as the CMake function
  does, so a link named `__pycache__` is refused by both; a link or a special file is `DATA_INVALID`
  (the digest is not defined), a missing or unreadable file `IO_ERROR`.
- **Every start checks the environment**: its package and lock against the core's build, whose
  libpython it links, and, after the first, the directories of the running environment and
  `praenora.sdk` (`UNSUPPORTED` either way). A first start refused before the interpreter starts
  leaves the process able to start it later; a failure after that is returned to every later start.
- **Children of `multiprocessing` stay isolated and pick their own hash seeds.** `multiprocessing`
  passes the parent's `-I` to its children, so the `PYTHON*` variables of the caller's environment
  never reach them, and an isolated interpreter ignores `PYTHONHASHSEED`. Patching
  `multiprocessing`'s spawn internals to drop `-I` and pass a scrubbed environment was rejected: it
  would depend on private code of the standard library, and a child that reads its environment is
  open to every `PYTHON*` variable a caller leaves in it. `praenora` and `praenora-module-host`
  still set `PYTHONHASHSEED=0` when it is unset, for the processes started without `-I`, which
  read it, and the core's warning about `PYTHONHASHSEED` speaks of those processes, the ones
  started with the process's environment and without `-E` or `-I`. This departs from the rule
  approved with the maintainer that children run with the hash seed 0, which isolated children
  cannot follow.
- **The build identity** is the SHA-256 of the lines `sources <digest>` and `praenora.sdk <digest>`,
  the directory digest of the files the core is built from (its sources, the runtime sources it
  compiles, the headers of other directories it compiles against, its build files) and of
  `praenora.sdk`, computed when the build is configured, with those files in
  `CMAKE_CONFIGURE_DEPENDS`, by one CMake function over one list of files, to which the sources
  that must agree with the core are added; a target property gives the same value to what is built
  with the core. The configuration stops when a file of the list includes a header of the source
  tree that the list lacks, so an edit of a limit or a layout the core compiles against cannot
  leave the identity as it was.
- **The core compiles the runtime's SHA-256**: `sha256.cpp` holds SHA-256 alone, with the digest of
  a stream (`sha256_stream_hex()`, which throws nothing of the runtime's), and the digest of a file
  as the runtime reports its failure moved to `sha256_file.cpp`, so the core needs no other part of
  the runtime and has one implementation, with hidden symbols; each caller reports a failed read
  in its own terms.
- **The build tree's core has `praenora.sdk` beside it**, in `python/praenora/sdk`, copied from the
  files its digest covers, so the build tree has an installation's layout.
- **`python.h` is checked as C99 in every build** (`PythonAbi.*`), as the other service headers are:
  it needs no Python.
- **The loader is one static library, compiled with the build's values**: the environment's
  package and lock and the core's build identity are compile definitions of
  `praenora_python_provider_support`, which hands them to every provider that links it, rather than
  definitions each provider passes in. Every provider of a build has the same values, and they end
  up inside each provider's library either way; one place keeps them from disagreeing. The
  loader's sources and `cmake/PraenoraPythonProvider.cmake` are files of the core's build identity,
  so a provider and a core of one build agree on how the environment file is read and what the
  core's `start` is given.
- **The core's build identity is compared before anything else of the core is called**: a core of
  another build is never started, so it cannot initialize the interpreter with code the
  fingerprint does not name.
- **Whether the core is loaded is asked of the system** (`dlopen(RTLD_NOLOAD)`,
  `GetModuleHandleExW()`), not kept by the loader: each provider that runs Python links its own
  copy of the loader, and the second provider of a process, whose copy knows nothing of the first
  start, skips the check for another interpreter and the load of libpython, which the first start
  did, and calls the core's `start`, which accepts only the running environment.
- **Another interpreter is one whose `Py_IsInitialized` comes from another file than the
  environment's libpython** (POSIX; on Windows a `python3*.dll` outside the environment's Python
  directory): the environment's own libpython, loaded by a start of this process that failed
  later, is not refused for it. An interpreter that some other code started on the environment's
  own libpython is the core's to refuse, at its start.
- **Start failures use the general codes of `status.h`**: `NOT_FOUND` for a missing file,
  `IO_ERROR` for one that cannot be read, `DATA_INVALID` for wrong contents, `UNSUPPORTED` for
  another environment, core or interpreter, `INTERNAL` for a library that does not load; the
  message names the files and the details carry them. No new code is needed, and the runtime
  reports every one as `LIBRARY_LOAD_FAILED` with the cause.
- **The provider's functions before a start return `INVALID_STATE`**: the runtime calls none of
  them before a successful start, but nothing else stops a caller that holds the table.
- **The programs find their own file through the system** (`_NSGetExecutablePath()` with links
  resolved, `/proc/self/exe`, `GetModuleFileNameW()`), not from `argv[0]`, and each says where it
  lies in its tree (`bin`, `libexec/praenora`) rather than the helper trying both: a program
  started through a link or by a relative path still finds its own tree, and never the parent of
  its tree.
- **`PYTHONHASHSEED` is set whenever the tree's environment file exists; the paths only when the
  file and the manifest read**: the hash seed needs nothing of the environment, and a broken file
  is the provider's start to report, with its cause, rather than the program's to stop on.
- **On Windows the variables are set with `_wputenv_s()`** after `GetEnvironmentVariableW()` says
  they are unset: it changes both the C runtime's copy, which `os.environ` and C code read, and the
  process's environment block, which child processes inherit and the core reads.
- **`praenora_add_python_provider()`**: without `ENGINE_JSON` the engine object is exactly the
  environment's (`PRAENORA_PYTHON_ENGINE_JSON`); with it, that object with `environment` added,
  and refused when it has an `environment` of its own, so the fingerprint names the environment
  the same way for every provider. The installation's environment file is written with paths
  relative to its own directory (`INSTALL_DESTINATION` must be relative), so a moved installation
  keeps working. `PYTHON_SOURCES` is refused for a provider in the core's own directory, whose
  `python/` is the core's `praenora.sdk`.
- **A Python module's object is created through its class**: the generated library calls
  `create_object()` with the callable `<package.module:Class>._praenora_adapter`, a class method
  of `praenora.sdk.Module` that returns the adapter object of `praenora.sdk._adapter`, rather than
  with `praenora.sdk._adapter:create`. The import-isolation rule checks the top-level package of
  the callable's name, which for `praenora.sdk._adapter:create` is `praenora`, always loaded from
  the core's own directory, so every create would be refused; through the class, the rule applies
  unchanged to the class's own package, which is what refuses two modules with one package.
  Exempting the core's own package and checking the class's package again in Python would have
  duplicated the rule. A class that does not derive from `Module` then fails `create` with
  `INTERNAL` (an `AttributeError` naming `_praenora_adapter`).
- **The library finds its file from an object of its own**, a marker that the module macro
  defines, with `dladdr()` (or `GetModuleHandleExW()` and `GetModuleFileNameW()`), as the runtime
  and the loader find theirs: an object's address names the library as well as the entry point
  does, without converting a function pointer to `void*`, which ISO C++ leaves conditionally
  supported. A relative file name is made absolute with the working directory, and glibc's empty
  name for the program itself is read from `/proc/self/exe`, so a module driven in a test program
  finds its sources too. `praenora::sdk::library_directory()` is public, for C and C++ modules that
  ship Python sources of their own.
- **The native adapter reuses the C++ adapter's helpers** (`module.hpp`'s `detail` namespace: the
  identity writer of `describe()`, the checks of the host API and of input bytes, sink writing,
  error documents, the host wrapper), so the identity document and the errors are written in one
  place for both adapters; it passes the host's result and diagnostics sinks to the service as they
  are, so a result of up to 16 MiB is never copied by the module.
- **A failed `self_test` logs its error document** at level error ("self_test: <document>"):
  the ABI's `self_test` has no error sink, and the contract checks then show only the status.
- **An empty create document is refused** (`INVALID_ARGUMENT`) before the service is called: an
  object without a create document is class-only, which only `self_test` asks for.
- **`praenora_add_python_module()` copies the sources by running its own file as a script**
  (`cmake -P PraenoraPythonModule.cmake`), so the SDK's package installs one file for it; the copy
  empties its directory first, so a source removed from the list leaves the copy too and the copy's
  digest stays the one compiled in. An installation takes that copy, not the source tree, so it
  holds exactly the files whose digest the library carries, also when a source was edited after
  the build.
- **The function refuses at configure time what would fail later**: a `CLASS` whose top-level
  package is not among the sources, `praenora.python` named in `REQUIRES_SERVICES` or
  `OPTIONAL_SERVICES` (the function adds it, first), a configured manifest that is not JSON (said
  as such, not as a missing self test), and a template that declares `features.self_test` without
  `SELF_TEST` (the contract checks would fail it), besides a `SELF_TEST` whose template does not
  declare it. `CLASS` is restricted to ASCII identifiers, since it becomes a C string literal.
- **A Python module's sources hold one top-level entry, the package of its `CLASS`**
  (`<top>/` or `<top>.py` below `PYTHON_ROOT`): every Python module of a process adds its
  directory to the one import path, so a `helpers.py` or a directory (a namespace package) beside
  the package could be another module's, imported in its place. The rule is the build function's,
  where it costs a few lines and refuses at configure time; the core still checks the location of
  the callable's top-level package alone, which is all that a C or C++ caller of the service gets.
  A registry in the core of every top-level name of every path was not chosen: it is heavier, and
  it would still miss what a module imports later.
- **The self-test check reads the manifest as `praenora_add_module()` configured it** (the target
  property `PRAENORA_MODULE_CONFIGURED_MANIFEST`, added for it): that is the one place where a
  template's variables are filled in, so the check sees what the manifest will declare. Without a
  template and with `SELF_TEST`, `praenora_add_module()` generates its default manifest with
  `"features": {"self_test": true}`, given through its internal argument `INTERNAL_FEATURES_JSON`,
  so the default manifest is written in one place for both functions.
- **A Python module's generated manifest keeps the default concurrency, `module`**: the instances
  of one library share their Python modules' global state in a process, so concurrent calls are a
  template's explicit declaration (`"concurrency": "instance"`), as for a native module.
- **The library links `${CMAKE_DL_LIBS}`**, for `dladdr()` on Linux before glibc 2.34; elsewhere
  the variable is empty.
- **A provider's Python object is the glue's, one for the provider library**:
  `PythonProviderObject::start()` starts the core and creates the object at the provider's
  start, with the document `null` and without a module's host, and keeps it for the process, as
  the provider is kept. A service function may be called from every module's thread at once; the
  core keeps each thread's records apart, so one object serves them all, and a table's glue keeps
  no Python state of its own.
- **The digest of a provider's Python sources reaches the glue through the provider's own
  compilation**: `PythonProviderObject::start()` is defined in `service_glue.hpp` for the sources
  that `praenora_add_python_provider()` compiles with `PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256`,
  and hands it to the support library. That library is built once for every provider, so it
  cannot carry one provider's digest, and a provider can start no other provider's sources.
- **The calling module's host API is the host of the call when the table function has one**, so a
  service's records and cancel flag are the module's that called it; without one (test.echo's
  echo given NULL) they are the provider's, as for records outside calls.
- **A table function without an error sink logs the error document** of a failed Python call to the
  provider's log and returns its status, so the cause is not lost.
- **`test-python-echo` carries bytes as hexadecimal text**: JSON documents hold UTF-8 text, and
  `test.echo` echoes any bytes; hexadecimal is exact for every byte and needs no codec in the
  glue, which base64 would. Its Python class logs through `praenora.sdk.Host` with the backend of
  `praenora.sdk._runtime`, as the core's own fixture classes do: the provider is this
  repository's, and `praenora.sdk` has no adapter for a provider's class.
- **The engine object of a provider with an `ENGINE_JSON` is composed as text**: the given object
  as written, with the environment's entry after its last one, and the environment's object, like
  the provider `python`'s engine object, written as text from the manifest's values, which are
  refused unless they consist of letters, digits, `.`, `_`, `+` and `-`. CMake's JSON writer
  changes spacing, escapes text that is not ASCII and spells numbers its own way, so its output
  would not be the text that `ENGINE_JSON` gives, and would not be the same on every CMake.
- **The paths of the environment files are JSON-escaped** (quotes, backslashes, every control
  character); the core's path is its directory, escaped, and its file name from
  `$<TARGET_FILE_NAME>`, since `$<TARGET_FILE>` is known only when the build is generated and
  cannot be escaped then.
- **The support library has one sink helper**, `TextSink`, for the loader, the glue and the
  providers: it refuses data that is NULL with a size, which the loader's copy would have read.
- **`praenora_add_python_provider()`'s decisions are tested in script mode**
  (`python.provider_cmake`): the composition of the engine object and the JSON strings are
  functions of their own, and the refusals come before anything is built, so the test needs no
  project and no Python and runs in every tree.
- **The print tool of the processes tests has a provider mode** for a provider whose typed table is
  implemented in Python (`praenora_python_print <manifest> --provider test.echo@0 <text>`), rather
  than a tool per fixture: it knows test.echo@0 alone, prints the table's header and the echoes in
  hexadecimal, which keeps the output text whatever the bytes.
- **The coexistence test with the roadrunner provider simulates in Python with the environment's
  `antimony` and `roadrunner` packages**, not through `tellurium`, which imports matplotlib; it
  runs both steps in the runtime's process, where the two libRoadRunners and LLVMs would meet. A
  run starts its providers before it creates its modules, so the roadrunner provider is loaded
  before the Python step imports the package: that is the order the test covers. Its fixture is
  the one that uses more of the environment than numpy, and the test runs only in a build with the
  roadrunner provider.
- **The programs find an installation's environment file through the installation's library
  directory**: each is compiled with the file's path relative to its own installed directory
  (`PRAENORA_PYTHON_ENVIRONMENT_FILE`, from `CMAKE_INSTALL_LIBDIR`, `CMAKE_INSTALL_BINDIR` and the
  module host's directory), and falls back to the build tree's `lib/` when nothing is there: an
  installation's library directory may be `lib64` or a multiarch directory, while a build tree's
  is always `lib`, and one binary serves both. The installation's path is tried first, so a stray
  file below `lib/` never wins over it.
- **The example predictor in Python integrates every estimate at once**, as numpy arrays with one
  element per estimate, each step updating the estimates that have not reached the output time:
  each element still goes through the C++ predictor's operations in the same order, each rounded
  once, so its outputs are the C++ predictor's, bit for bit, and its test compares them byte for
  byte, envelope ids aside (they name the run, whose fingerprint differs with the module). It has a
  manifest template of its own, with the C++ predictor's parameter schema, so that its parameters'
  defaults resolve as there; and it is built only with the option, as the provider that runs it.
  It reads the estimates up to the first invalid one and integrates those before it fails, since
  the C++ predictor integrates each estimate before it reads the next: its diagnostics count the
  same steps after a failed invocation too.
- **`fx-py-pool` reports its children's interpreters on request** (`report_executables`), so that a
  test can tell which interpreter a spawn pool started (a moved installation's); its outputs
  without the parameter are as before.
- **Each thread that calls the core keeps one Python thread state** for its life: the core's first
  `PyGILState_Ensure()` on a thread is kept, with the lock released, so each later call on the
  thread finds the same state, and what Python keeps per thread (context variables, among them
  numpy's error state and `decimal`'s context, and `threading.local` values) lasts from
  `initialize` to `invoke`, as in a Python program and in the test kit, rather than being lost
  with a state made and deleted for each call. The state is cleared and deleted when its thread
  ends, as CPython ends the threads it starts, except where taking the interpreter's lock then is
  not safe: while the process exits (an exit handler of the core says so), on the process's main
  thread (whose thread-local objects are destroyed as it exits), and on Windows, where a thread's
  thread-local objects are destroyed under the loader's lock, which a thread holding the
  interpreter's lock may wait for. There the state stays with the interpreter, which is never
  finalized. The thread that started the interpreter keeps the state it has. The stack of call
  targets is a chain of objects on the calling thread's stack, so Python code that runs as a
  thread's state is cleared finds no target that was destroyed before it.
- **The threads that run a module's code have a main thread's stack**: each instance's worker
  thread and a module host's call thread are `ModuleThread`s (`src/runtime/platform/`), with the
  stack that a module host's load thread had already, the soft limit of `RLIMIT_STACK` within 1 MiB
  and 1 GiB, 8 MiB when there is none, and on Windows an 8 MiB reservation. Python code takes the
  C stack whenever a call passes through C (`functools.lru_cache`, a `__getattr__`), and so does
  the C JSON encoder on nested data: on macOS's 512 KiB default such code crashes the process well
  below Python's recursion limit. The stack is reserved address space and costs memory only as it
  is used. It applies to every module, not only Python modules: native code expects a main
  thread's stack too.
- **The interpreter reads no cached bytecode**: its `pycache_prefix` is a path below the
  environment's manifest, a regular file, so no file can ever be there on any system, and Python
  then looks for no cached bytecode beside a source either. Every module the process imports is
  compiled from its source, which the digests cover; a stale or foreign `.pyc` in a `__pycache__`
  directory, which the digests leave out, cannot run. An empty directory would do it until
  something wrote there; a path that can hold no file cannot change. The environment ships few
  compiled files, so imports cost about what they did. Children that `multiprocessing` starts are
  not given the prefix (it is not among the flags that the standard library passes on), so they
  read cached bytecode beside a source where there is some; the runtime writes none.
- **The directory of `praenora.sdk` holds nothing else**: the core puts it first on the import
  path, before the standard library, and its digest covers `praenora/sdk` alone, so the first start
  refuses a directory that holds anything but `praenora/sdk` (`__pycache__` directories aside),
  rather than run a stray `json.py` that no digest names.
- **The readers of the environment's files share one header-only helper**
  (`src/providers/python/common/environment_files.hpp`: comparing paths, reading a manifest's
  strings, resolving a path relative to a file, a module's file on Windows), included by the core,
  its loader and the programs' environment setup, which must read the files alike; it is one of the
  files of the core's build identity.
- **`praenora.sdk`'s tests import nothing but the standard library** (`python.sdk` runs with `-I
  -S -B`), which makes the package's promise a test, and run with the interpreter of the Python
  bindings too (`python.sdk_bindings_interpreter`, in every tree that builds them, but sanitizer
  trees, as `python.unittest`), since module authors test their classes with interpreters of their
  own.

## Alternatives considered

- **A generic document service for every provider implemented in Python (option B).** One table
  (`call(method, json)`) would let any service be served from Python without a C table per service.
  Not chosen: every public service keeps its typed table and header, so a module's C or C++ code
  checks them at compile time, and a service implemented in Python is indistinguishable from one
  implemented natively.
- **An environment outside the installation's archive** (found on the machine, or fetched at first
  run). Not chosen: a run's fingerprint and a release must name one environment; an installation
  that depends on a package manager's Python, or on the network, is not reproducible.
- **One interpreter per module library**, or sub-interpreters in one process. Not chosen: the
  numeric packages of the environment refuse to load in more than one interpreter of a process, and
  one interpreter per library would load libpython several times.
- **Each provider linking libpython itself**, delay-loaded where the environment may be missing.
  Not chosen: MSVC cannot delay-load a DLL whose data symbols are imported, and CPython's headers
  import data symbols; a provider linked to libpython would fail to load where the environment is
  missing, before the checks of its start that name the environment file, the environment and its
  lock; and each provider would carry its own copy of the embedding code and of its state for the
  one interpreter of the process.
- **Generating the module's native library with Python's headers.** Not chosen: a module author
  should build a Python module with any C++17 compiler and no Python.

## Consequences

Easier: a module in Python is a class and a build call; a run records which Python code it used;
tests of a module's class need no runtime; a C or C++ module can reach the environment's packages
through the same service.

Harder: the runtime's process holds an interpreter once a Python step runs in it, and a process that
runs another interpreter, such as the Python front end's, must use a module host; Windows still
refuses process isolation, so there a Python module shares the runtime's process, and the Python
front end cannot run it. Two modules of one process cannot ship a top-level package of one name, and
code of a path cannot change within a process: module hosts, one process per step, are the way out
of both. Code that runs in a `multiprocessing` child must not depend on the iteration order of sets
of strings. The core's interface and the way a provider pins the core's identity (its build
identity, compared at start) are written in [python-service.md](../specs/python-service.md), and so
is the library of a Python module ([Python modules](../specs/python-service.md#python-modules)).

Open: Linux and Windows, where none of it is verified yet
([platforms.md](../platforms.md#verification-status)); a product provider implemented in Python,
which this version does not add; and the wheel, which holds `praenora.sdk` but neither the
environment nor a module host, so that the Python front end runs Python steps only with the runtime
of a build tree or of an installation (OQ-13 covers the wheel's contents).

## References

- [Python service](../specs/python-service.md), [runtime services](../specs/runtime-services.md),
  [process isolation](../specs/process-isolation.md), [reproducibility](../specs/reproducibility.md)
- [ADR-0021](ADR-0021-runtime-services-through-host-api.md),
  [ADR-0026](ADR-0026-runtime-service-providers.md),
  [ADR-0037](ADR-0037-process-isolation-of-module-instances.md),
  [ADR-0045](ADR-0045-python-environment-pinned-in-ci-and-installation.md)
