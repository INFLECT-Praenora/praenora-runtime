# Python service `praenora.python@0` and Python modules

- **Version:** service `praenora.python` 0.0 (`PRAENORA_PYTHON_SERVICE_MAJOR` 0,
  `PRAENORA_PYTHON_SERVICE_MINOR` 0), declared in [`python.h`](../../include/praenora/sdk/python.h)
  of the module SDK 0.2.0, offered by the provider `python`
- **Status:** Normative: the directory digest, the Python package `praenora.sdk`, the service's
  table, Python modules, the Python core, the provider `python`, the start of the providers that
  run Python, the providers implemented in Python and the process's environment. Proposed
  ([ADR-0044](../adr/ADR-0044-python-modules-and-providers-through-an-embedded-environment.md),
  [ADR-0045](../adr/ADR-0045-python-environment-pinned-in-ci-and-installation.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

Python modules and providers use the packages of the pinned Python environment that
praenora-runtime-pyenv builds. A module is a Python class derived from `praenora.sdk.Module`, with a
generated native module library that requires the service `praenora.python@0`; the provider
`python` offers that service, which creates Python objects and calls their methods with JSON
documents. The build option that builds what runs Python is `PRAENORA_WITH_PYTHON_ENVIRONMENT`
(off by default); what a module's build needs, `python.h`, `python_module.hpp`,
`praenora_add_python_module()` and the digest function below, is built and installed whatever it
says. The package `praenora.sdk` is in the wheel whatever it says, and in an installation beside the
Python core, so only with the option.

## The directory digest

The Python sources of a module or a provider are identified by one value, the digest of the files
below a directory. The library of a Python module or provider carries the digest of the sources it
was built with, so the library's SHA-256, which the run fingerprint records, changes with them, and
the runtime checks the files it finds against the digest before it imports them.

The digest is 64 lowercase hexadecimal digits, computed as follows.

1. Take every regular file below the directory, at any depth, but those below a directory named
   `__pycache__`.
2. Name each by its path relative to the directory, with `/` as the separator, in UTF-8.
3. Sort the paths by their bytes (so `a.py` comes before `pkg/__init__.py`, and an uppercase letter
   before a lowercase one).
4. For each file in that order write one line: the SHA-256 of the file's bytes as 64 lowercase
   hexadecimal digits, two spaces, the relative path, and a line feed (`\n`, also on Windows).
5. The digest is the SHA-256 of the concatenation of the lines, as 64 lowercase hexadecimal
   digits.

The listing is the one that `sha256sum` prints, so a digest can be computed by hand. In the
directory, with the paths sorted by bytes:

```bash
find . -type f -not -path '*/__pycache__/*' | LC_ALL=C sort | while IFS= read -r f; do
  printf '%s  %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "${f#./}"
done | shasum -a 256
```

A directory with no file has the SHA-256 of the empty string as its digest. A symbolic link, to a
file or to a directory, is not a regular file: the digest is not defined for a tree that holds one
outside a `__pycache__` directory, and `praenora_python_directory_digest()` stops the
configuration with a message that names the link (the shell command above skips links silently,
so it is no check of this rule).

**In CMake**, `PraenoraPythonDigest.cmake` (installed with the CMake package of Praenora, component
`ModuleSDK`) defines

```cmake
praenora_python_directory_digest(<out_var> ROOT <dir> FILES <file>...)
praenora_python_directory_digest(<out_var> ROOT <dir> GLOB_ROOT <dir>)
```

which stores the digest in `<out_var>`. `ROOT` is the directory the paths are relative to; `FILES`
names the files, each absolute or relative to `ROOT` (a file named twice counts once), and
`GLOB_ROOT` takes every file below a directory instead. Files below `__pycache__` are left out
also when `FILES` names them. A file outside `ROOT`, an entry of `FILES` that does not exist or is
a directory, a path that holds a semicolon and a call with neither `FILES` nor `GLOB_ROOT` stop the
configuration.

**In the runtime**, the Python core ([The Python core](#the-python-core)) computes the digest of
the files it finds below a directory, by the same rules, without following a symbolic link. A
symbolic link anywhere below the directory but inside a `__pycache__` directory, one named
`__pycache__` included, makes the digest undefined: the core refuses the directory with
`PRAENORA_STATUS_DATA_INVALID`, as it refuses a file that is not a regular one (a named pipe, for
example). A directory that does not exist, or a file or directory below it that cannot be read, is
`PRAENORA_STATUS_IO_ERROR`. Only a directory named `__pycache__` is left out: a regular file of
that name is a file like any other, for the CMake function too.

## The Python package `praenora.sdk`

`praenora.sdk` is pure Python and imports nothing outside the standard library, so it imports in
any interpreter. Its sources are in `python/praenora/sdk/` and ship in the `praenora-runtime` wheel
and, beside the Python core ([The provider `python`](#the-provider-python)), in an installation
with the Python environment. `praenora` is a namespace package: `praenora.runtime`, the front end,
and `praenora.sdk` are separate packages of it.

| Name | Meaning |
|---|---|
| `Module` | The base class of a Python module. `Module(context)` keeps `context`; `initialize(document)` and `finalize()` do nothing; `invoke(document)` raises `Error(Status.UNSUPPORTED, ...)` until overridden; `diagnostics()` returns `{}`. A class method `self_test(host)` may be defined, returning a document `{"passed": bool, "checks": [...]}`. |
| `CreateContext(host, document)` | What a constructor gets: `.host` (a `Host`) and `.document` (the create document, a dict). |
| `Host(backend)` | `log(level, message)`, `log_enabled(level) -> bool` and `cancel_requested() -> bool`. `backend` supplies the three. |
| `LogLevel` | `IntEnum`: `TRACE` 0, `DEBUG` 1, `INFO` 2, `WARN` 3, `ERROR` 4, `CRITICAL` 5 (the `PRAENORA_LOG_*` values of `abi.h`). |
| `Status` | `IntEnum` of the 13 codes of `status.h`, `OK` 0 to `DATA_INVALID` 12, with the names of `PRAENORA_STATUS_*` without the prefix. |
| `Error(code, message, details=None)` | An exception with `.code` (a `Status`, or an integer for a module-defined code from 10000), `.message` and `.details`. |
| `Cancelled(message="cancelled")` | An `Error` with the code `Status.CANCELLED`; raise it when `host.cancel_requested()` is true. |

Documents are dicts, lists, strings, numbers, booleans and `None`: what JSON holds.

The runtime creates a module's object through the class: `Module._praenora_adapter(args)`, a class
method that module code never calls or overrides, with `args` `{"create": <create document>}`,
returns the adapter object of `praenora.sdk._adapter` for the class (`_adapter.adapter_of(cls,
args)`); its keyword argument `host`, which the runtime never gives, is the test kit's way to hand
the module a host of its own. Its methods `initialize`, `invoke`, `finalize`, `diagnostics` and
`self_test` take and
return JSON-able values. When `"create"` is absent or `null` (or `args` is `null`) the object is
class-only: the class is not constructed, `self_test` calls the class method `self_test(host)`, and
every other method fails with `INVALID_STATE`. The ABI's self test has no instance and no create
document, so the runtime uses a class-only object for it. The runtime names the callable
`<package.module:Class>._praenora_adapter`, so the top-level package that the service imports, and
whose location it checks ([create_object](#create_object), step 4), is the class's own; a class
that does not derive from `Module` has no such attribute and cannot be created (`INTERNAL`, an
`AttributeError` naming `_praenora_adapter`). `praenora.sdk._adapter.create(args)` does the same
for `args` `{"class": "package.module:Name", "create": <create document>}`, importing the class
itself and refusing one that is not a subclass of `Module` with a `TypeError` naming it.

The documents are the PMP documents of [protocol.md](protocol.md): the create document
(`pmp_version`, `step`, `capability`, `module`, `parameters`), the initialize document
(`pmp_version`, `run` with `fingerprint` and `seed`, `time` with `unit`, `start` and `period`) and
the invocation document (`pmp_version`, `step`, `tick`, `logical_time`, `inputs`, a dict of
envelopes by input port: a module reads the payload of port `x` as
`document["inputs"]["x"]["data"]`). `Error` and `Cancelled` pickle, so they cross process boundaries
(a `multiprocessing` pool).

`praenora.sdk._runtime` holds the hooks of the runtime's Python core: `install(native)`,
`capture_output()` and `configure_children()`, which the core calls once as it starts
([In the interpreter](#in-the-interpreter)); `enter_call(host)` and `exit_call()`, which bracket
each call (`exit_call()` turns the calling thread's unfinished line of output into a record);
`current_host()`; and `load_document(text)` and `dump_document(value)`, with which the core reads
and writes documents. Outside the core nothing is installed and the package still works.

### The test kit

`praenora.sdk.testing` runs a module class without the runtime, for unit tests, on the runtime's
path: it creates the module through `Module._praenora_adapter`, as the runtime does, so a class
that does not derive from `Module` fails with the `AttributeError` that fails its create in a run,
and it calls the methods of the adapter object. Every document it sends is written as JSON and
read back (`_runtime.dump_document()`, `_runtime.load_document()`), as the runtime sends it, and so
is every result it gets: a result that the runtime cannot write fails as in a run, with
`Error(Status.INVALID_ARGUMENT)` and the message "the result of `<method>` is not a JSON document:
`<Type>`: `<message>`" (a numpy integer, `float32` or `bool_`, or an array, NaN or an infinity, a
set), and one above 16 MiB with `Error(Status.RESOURCE_EXHAUSTED)`; a tuple comes back as a
list. A document the kit would send that JSON cannot hold (a parameter or an input of a test)
raises the JSON writer's `TypeError` or `ValueError`.

- `FakeHost(cancel_after=None)` implements the host's three methods and records each log record in
  `logs`, a list of `(level, message)`. After `cancel_after` polls of `cancel_requested()` it
  reports cancellation (never, when `None`). It answers alike on every thread, where the runtime's
  host acts only for the call that runs on the calling thread
  ([Threading, records and cancellation](#threading-records-and-cancellation)).
- `ModuleDriver(cls, parameters=None, step="s", seed=1, host=None, capability=None, module=None,
  time=None)` builds the documents as the runtime does. `create()` creates the module with
  `{"create": <create document>}`, which constructs the class with a `CreateContext` that holds
  the host and the create document; `initialize()` calls `initialize` with `pmp_version`, `run`
  (`fingerprint`, `seed`) and `time` (default `{"unit": "1", "start": 0.0, "period": 1.0}`);
  `invoke(inputs, tick=0)` takes a dict of payloads by input port, wraps each in an envelope
  (`pmp_version`, `id`, `type`, `type_version`, `source`, `tick`, `logical_time`, `data`), calls
  `invoke` with `pmp_version`, `step`, `tick`, `logical_time` (`start + (tick + 1) * period`) and
  `inputs`, and returns the result document; `finalize()` calls `finalize`; `diagnostics()`
  returns the diagnostics document (`{}` for a module that returns `None`); and `self_test()`,
  which needs no `create()`, runs the class method `self_test(host)` through a class-only object,
  as the runtime does, and returns the report (`Error(Status.UNSUPPORTED)` for a class without
  one). `driver.module` is the instance, `driver.adapter` the object the runtime would call and
  `driver.logs` the host's records. Every call runs on the calling thread.

## The service `praenora.python@0`

A module declares the service in its manifest (`REQUIRES_SERVICES praenora.python@0.0` in
`praenora_add_module()`) and gets its table from the host API:

```c
const praenora_python_v0* python = (const praenora_python_v0*)host->get_service(
    host->context, PRAENORA_PYTHON_SERVICE, 15, PRAENORA_PYTHON_SERVICE_MAJOR);
```

It MUST accept the table only when `header.abi_major` is `PRAENORA_PYTHON_SERVICE_MAJOR`,
`header.reserved` is 0 and `header.struct_size` is at least `PRAENORA_PYTHON_V0_BASE_SIZE`; a later
minor version only appends functions, which a module uses only when `struct_size` covers them.
The generated library of a Python module uses the service; a C or C++ module MAY use it directly to
reach the packages of the environment.

```c
#define PRAENORA_PYTHON_SERVICE "praenora.python"
#define PRAENORA_PYTHON_SERVICE_MAJOR 0u
#define PRAENORA_PYTHON_SERVICE_MINOR 0u
#define PRAENORA_PYTHON_MAX_DOCUMENT_SIZE ((size_t)16 * 1024 * 1024)

typedef struct praenora_python_object praenora_python_object; /* opaque, the provider's */

typedef struct praenora_python_v0 {
    praenora_api_header header;
    void* context;
    praenora_status (*describe)(void* context, praenora_sink* out_json);
    praenora_status (*create_object)(void* context, const praenora_host_api* host,
        praenora_bytes path, praenora_bytes path_sha256, praenora_bytes name,
        praenora_bytes args_json, praenora_python_object** out_object, praenora_sink* error_json);
    praenora_status (*call)(praenora_python_object* object, praenora_bytes method,
        praenora_bytes args_json, praenora_sink* result_json, praenora_sink* error_json);
    void (*release_object)(praenora_python_object* object);
} praenora_python_v0;
```

### Conventions

- Only fixed-width integers, `size_t`, pointers, length-delimited UTF-8 (`praenora_bytes`) and sinks
  cross the table, as in [module-abi.md](module-abi.md); no exception or `longjmp` leaves a
  function.
- Sinks are the module's: the provider writes to them during the call only, in pieces of at most
  1 MiB. Every sink is required: a missing one, or one without a `write` function, is
  `PRAENORA_STATUS_INVALID_ARGUMENT`, and nothing is written when `error_json` is the one missing. A
  refused write ends the call with the status that `write()` returned, after an error document to
  `error_json` when the refusing sink is another one. An error document that `error_json` refuses
  leaves the call's status as it is.
- Inputs are borrowed for the call; their data MAY be NULL only when their size is 0.
- **Documents** are JSON in UTF-8, each of at most `PRAENORA_PYTHON_MAX_DOCUMENT_SIZE` bytes
  (16 MiB): a larger argument document is refused before it is parsed, and a larger result before
  any of it is written, both with `PRAENORA_STATUS_RESOURCE_EXHAUSTED` and a message that names the
  limit. Empty argument bytes are `null`. Arguments are read with `json.loads`, and `NaN`,
  `Infinity` and `-Infinity`, which JSON does not have, are refused with
  `PRAENORA_STATUS_INVALID_ARGUMENT`. A result is written as `json.dumps` writes it with
  `allow_nan=False`, `ensure_ascii=False` and no white space (`null` for `None`); NaN, an infinity
  or a value JSON cannot hold (a set, an object of a class of its own) is
  `PRAENORA_STATUS_INVALID_ARGUMENT`, with a message that names the method.
- **Text that leaves Python** (documents, error messages, records) is valid UTF-8: a lone surrogate,
  which UTF-8 cannot hold, is written as U+FFFD (not as the `?` that Python's `replace` error
  handler writes when it encodes), and so is each byte sequence that is not UTF-8 in what Python
  code writes to `sys.stdout.buffer` or `sys.stderr.buffer`.

### describe

`describe` writes `{"python": {"implementation", "version", "abi"}, "environment": {"name",
"version", "package", "lock_sha256"}}`, the values of the environment's manifest, as canonical JSON:
the same bytes every time. It is thread-safe and free of side effects.

### create_object

`create_object(context, host, path, path_sha256, name, args_json, &object, error_json)`:

1. `path` is an absolute directory in UTF-8, or empty for none; `path_sha256` is 64 lowercase
   hexadecimal digits, or empty for no check, and needs a `path`; `name` is
   `"package.module:attribute"` (the attribute may be dotted). Anything else is
   `PRAENORA_STATUS_INVALID_ARGUMENT`.
2. **The code of a path is fixed for the process.** The first `create_object` of a path computes
   the digest of the files below it ([The directory digest](#the-directory-digest)) and records
   it, with or without a `path_sha256`. When `path_sha256` is given and differs from it, the call
   fails with `PRAENORA_STATUS_DATA_INVALID`, the message "the Python sources below `<path>` differ
   from those the module was built with" and `details` `{"path", "expected_sha256",
   "actual_sha256"}`. A later `create_object` of the same path compares its `path_sha256` with the
   recorded digest, never with the files, and a different one fails with
   `PRAENORA_STATUS_DATA_INVALID`: the sources changed since the process imported them, and the
   message says to use a new process (a module host is one per step), with `details` `{"path",
   "expected_sha256", "imported_sha256"}`. Imported code is never imported again. A directory that
   does not exist or cannot be read is `PRAENORA_STATUS_IO_ERROR`.
3. `path` is appended to the import path (`sys.path`), once, after the environment's own entries.
   It SHOULD hold one top-level package and nothing else beside it: every path of the process is
   on the one import path, where another top-level name (a `helpers.py`, a directory, which is a
   namespace package) could be another path's, imported in its place.
   `praenora_add_python_module()` refuses such sources ([Python modules](#python-modules)); a C or
   C++ module that calls the service itself gets only the check of step 4.
4. **Import isolation.** The top-level package of `name` MUST come from below `path` when a path is
   given: when it is loaded already from elsewhere, before the import or after it, the call fails
   with `PRAENORA_STATUS_INVALID_ARGUMENT` and the message "package `<p>` is already loaded from
   `<file>`". So two modules that ship the same top-level package are refused, and so is a module
   that would shadow a package of the environment. Only that package is checked.
5. The callable is imported and called with the parsed `args_json` as its one argument; what it
   returns is kept as the object. An import that fails, an attribute that does not exist and a
   constructor that raises are `PRAENORA_STATUS_INTERNAL` with the traceback
   ([Errors](#errors)); an attribute that cannot be called is `PRAENORA_STATUS_INVALID_ARGUMENT`.

On success `*object` holds the object, which `release_object` releases; on failure it is NULL.

### call and release_object

`call(object, method, args_json, result_json, error_json)` calls `object.<method>(document)` with
the parsed `args_json` and writes what it returns to `result_json`. A method that does not exist,
or an attribute that cannot be called, is `PRAENORA_STATUS_UNSUPPORTED`, with the method's name in
the message and in `details.method`. `release_object(object)` drops the object;
`release_object(NULL)` does nothing.

### Threading, records and cancellation

- `describe` may be called from any thread at any time. The calls of one object come from one
  thread at a time (the rule of a module instance); different objects MAY be called from different
  threads at the same time. One interpreter serves the process, and its lock runs their Python code
  one thread at a time; the provider calls the host with that lock released.
- Each thread that calls the service has one Python thread state for its life, made at its first
  call: what Python keeps per thread (context variables, such as numpy's error state and
  `decimal`'s context, and `threading.local` values) lasts from one call on the thread to the next.
  A module instance's calls all run on its one thread, so what `initialize` sets there holds in
  `invoke`. The state is released when its thread ends, but on the process's main thread, while
  the process exits and on Windows, where it is kept until the process exits
  ([The interpreter](#the-interpreter)).
- `host` is the module's host API, borrowed from `create_object` until `release_object` returns; it
  MAY be NULL. Python code's records ([Output and logging](#output-and-logging)) go through the
  host of the call that runs on the thread, and outside calls (on a thread that Python code
  started, for example) to the provider's log. A NULL `host` sends the object's records to the
  provider's log too.
- Cancellation is cooperative: `praenora.sdk.Host.cancel_requested()` polls the `cancel_requested`
  of the host of the call that runs on the thread, and module code that sees it raises
  `praenora.sdk.Cancelled`. A call that never polls runs to its end; in a module host, a call that
  outlives its deadline is ended by killing the module host
  ([process-isolation.md](process-isolation.md)).

### Errors

A failed call writes an error document `{"code", "message", "details"}` to `error_json`:

| Raised by Python code | Status | Message and details |
|---|---|---|
| `praenora.sdk.Cancelled` | `PRAENORA_STATUS_CANCELLED` | its message and details |
| `praenora.sdk.Error(code, message, details)` | its code: a general code (1 to 99) or a module-defined one (from 10000) | its message and its details, unchanged; details that are not a JSON object become `{"invalid_details": "<text>"}` |
| `praenora.sdk.Error` with another code (0, for one) | `PRAENORA_STATUS_INTERNAL` | its message; `details.raised_code` holds the code |
| `MemoryError` | `PRAENORA_STATUS_RESOURCE_EXHAUSTED` | `"MemoryError: <message>"`, `details.traceback` |
| any other exception | `PRAENORA_STATUS_INTERNAL` | `"<Type>: <message>"` (the type with its module, but for built-in ones, as Python's tracebacks write it), `details.traceback`: the lines of `traceback.format_exception()`, one line each |

The other statuses of the service are those of the sections above: `INVALID_ARGUMENT` for arguments,
for documents that are not JSON and for an import that clashes, `RESOURCE_EXHAUSTED` for the size
limit, `DATA_INVALID` and `IO_ERROR` for the sources of a path, `UNSUPPORTED` for a method that does
not exist, and the status of a refusing sink. No error code is added for the service.

## Python modules

A Python module is a class derived from `praenora.sdk.Module`, its Python sources and a module
library of ABI v0 ([module-abi.md](module-abi.md)) that forwards each function of the table to the
class through `praenora.python@0`. The library is the expansion of
`PRAENORA_SDK_DEFINE_PYTHON_MODULE("package.module:Class", "<digest>", <self test>)` of
[`python_module.hpp`](../../include/praenora/sdk/python_module.hpp) (module SDK, header-only), in
one source file that `praenora_add_python_module()` (`PraenoraPythonModule.cmake`, installed with
the SDK's CMake package) generates; it needs neither Python's headers nor libpython. The library
carries the class, the directory digest of the module's Python sources and whether its table has a
`self_test`, so its SHA-256, which the run fingerprint records, changes with every byte of the
sources ([reproducibility.md](reproducibility.md#run-fingerprint)). Its sources are in
`<directory of the library>/<name>.python/`, `<name>` being the module's name, which holds them and
nothing else; their one top-level entry is the top-level package of the class (`<package>/` or
`<package>.py`), and `praenora_add_python_module()` refuses sources with another, which would be
on the import path of every module of the process ([create_object](#create_object), step 3). The
manifest requires `praenora.python@0.0`, first in `requires.services`, so a step
of a Python module that no provider serves is refused by validation (`SERVICE_UNAVAILABLE` at
`/requires/services/0`). The
[module developer guide](../module-developer-guide.md#modules-in-python) describes the class, the
function and the layout.

The table:

| Function | What it does |
|---|---|
| `describe` | The identity of `PRAENORA_MODULE_NAME`, `PRAENORA_MODULE_VERSION` and `PRAENORA_MODULE_CAPABILITIES`, as the C++ adapter writes it; no call into Python. |
| `create` | Gets `praenora.python@0` from the host API; without it, or with a table it cannot use (another major, `reserved` not 0, shorter than `PRAENORA_PYTHON_V0_BASE_SIZE`, a NULL function), it fails with `PRAENORA_STATUS_UNSUPPORTED` ("the service praenora.python@0 is not available"). It finds its own library file (`dladdr()`; on Windows `GetModuleHandleExW()` and `GetModuleFileNameW()`, the path converted to UTF-8) and calls `create_object(host, <library directory>/<name>.python, <digest>, "<package.module:Class>._praenora_adapter", {"create": <create document>})`, whose status and error document are its own. An empty create document is `PRAENORA_STATUS_INVALID_ARGUMENT`; a library whose file cannot be found is `PRAENORA_STATUS_INTERNAL`. |
| `initialize`, `invoke` | Call the object's method of that name with the call's document; `invoke`'s result goes to the host's result sink as the service writes it. |
| `finalize`, `diagnostics` | Call the object's method of that name with `null`; `diagnostics` writes its result to the output sink. |
| `self_test` | Only with a self test (NULL otherwise): creates a class-only object (`{"create": null}`), calls its `self_test` and releases it. A failure's error document, for which the ABI's `self_test` has no sink, becomes a log record of level error, `"self_test: <error document>"`. |
| `destroy` | Releases the object. |

The arguments, statuses and error documents of the calls are the service's
([The service `praenora.python@0`](#the-service-praenorapython0)): the Python sources edited after
the build fail `create` with `PRAENORA_STATUS_DATA_INVALID`, another build of the same path in the
same process too, and a top-level package that another module's sources loaded already with
`PRAENORA_STATUS_INVALID_ARGUMENT`. A module instance's calls come from one thread at a time, as the
ABI says; two instances of a library whose manifest declares `features.concurrency: "instance"` may
be called at the same time, the interpreter running their Python code one thread at a time.

## The Python core

The core is the one library of the runtime that embeds CPython and links the environment's
libpython: `praenora-python-core` (`.dylib`, `.so`, `.dll`), in the directory of the provider
`python`. The providers that run Python load it, and so a process holds one interpreter and one copy
of its state, whichever providers start it; one core file exists per tree (the build tree, an
installation). Its interface, `src/providers/python/core/python_core.h`, is internal, built only in
this repository: it follows the conventions of the provider interface, and its one exported symbol
is `praenora_python_core_get_api`, which returns the table:

| Function | What it does |
|---|---|
| `start(provider_host, environment_dir, sdk_dir, error_json)` | Starts the interpreter or checks the running one ([Starting](#starting)). |
| `describe(out_json)` | The service's `describe`, after a successful start (`PRAENORA_STATUS_INVALID_STATE` before one). |
| `create_object(provider_host, module_host, path, path_sha256, name, args_json, &object, error_json)` | The service's `create_object`; the object's records go to `module_host`, or to `provider_host` when it is NULL. |
| `call(object, module_host, method, args_json, result_json, error_json)` | The service's `call`; a non-NULL `module_host` replaces the object's host for the call: its records and its cancel flag. |
| `release_object(object)` | The service's `release_object`. |
| `build_id(out_text)` | The core's build identity, 64 lowercase hexadecimal digits. |

The core links neither the runtime nor its objects (a module host holds the runtime's objects
itself), and it never sets a variable of the process's environment.

**The build identity** is the SHA-256 of the two lines `sources <digest>\n` and
`praenora.sdk <digest>\n`: the directory digest of the files the core is built from, named by their
paths in the source tree (its sources, the runtime's sources it compiles, the headers of other
directories it compiles against, such as `python.h` and the provider interface's, and its build
files), with those of what is built with it and must agree with it, and the digest of the
`praenora.sdk` sources. The build computes it when it is configured, and stops when one of those
files includes a header of the source tree, directly or not, that is not among them; editing one
of them configures the build again, and the identity follows. A provider built with the core
carries the same identity and compares it with the core's `build_id`.

### Starting

`start` takes the environment's directory (absolute, holding `praenora-runtime-pyenv.json`) and the
directory that holds `praenora/sdk` (absolute). `provider_host` stays valid for the rest of the
process, as a provider's own host does. In order:

1. The environment's manifest is read: `PRAENORA_STATUS_IO_ERROR` when it cannot be, and
   `PRAENORA_STATUS_DATA_INVALID` when it is not JSON or lacks a value the core needs.
2. Its `package` and `lock.sha256` MUST be those the core was built against, whose libpython it
   links: otherwise `PRAENORA_STATUS_UNSUPPORTED`, with a message that names both values.
3. A later start of the process MUST name the directory of the running environment and the
   directory of the `praenora.sdk` the interpreter imported: otherwise
   `PRAENORA_STATUS_UNSUPPORTED`, naming both. It starts nothing.
4. The first start fails with `PRAENORA_STATUS_UNSUPPORTED` when the libpython it links runs an
   interpreter that the core did not start: a process holds one interpreter, and the step belongs
   in a module host (isolation mode `process`).
5. The first start checks that `sdk_dir` holds `praenora/sdk` and nothing else (directories named
   `__pycache__` aside), since it comes first on the import path, before the standard library, and
   no digest covers anything else there: `PRAENORA_STATUS_DATA_INVALID` naming the first other
   entry, with `details` `{"sdk_dir", "path"}`. It then checks the digest of
   `<sdk_dir>/praenora/sdk` against the one the core was built with: `PRAENORA_STATUS_DATA_INVALID`
   when the sources differ, with `details` `{"path", "expected_sha256", "actual_sha256"}`. A start
   refused so far changes nothing, and a later one may succeed.
6. The first start initializes the interpreter ([The interpreter](#the-interpreter)) on the calling
   thread, prepares it ([In the interpreter](#in-the-interpreter)) and releases its lock. A failure
   from here on is `PRAENORA_STATUS_INTERNAL`, or `PRAENORA_STATUS_DATA_INVALID` when
   `praenora.sdk` was imported from another directory than `sdk_dir` (the sources the interpreter
   would run are not those step 5 checked), and every later start of the process returns it again:
   the interpreter cannot be started twice.
7. Every start that succeeds checks the variables of the process's environment and logs, to its
   `provider_host`, a warning for each variable of the manifest's `environment.paths` but
   `PYTHONHOME` that is unset or names another path than the environment's, and for
   `PYTHONHASHSEED` when it is unset or not `0`. The run goes on. The runtime's own programs set
   them when they are unset; another program that embeds the runtime sets them before it starts
   threads ([The process's environment](#the-processs-environment)).

Records written outside calls go to the provider host of the first start that succeeds.

### The interpreter

The core initializes the interpreter with `PyPreConfig_InitIsolatedConfig` and UTF-8 mode, then
`PyConfig_InitIsolatedConfig` with:

| Setting | Value |
|---|---|
| `home` | the manifest's `python.home`, below the environment's directory |
| `executable` (so `sys.executable`) | the manifest's `python.executable`, below the environment's directory |
| `write_bytecode` | 0: no `__pycache__` is written |
| `pycache_prefix` | `<environment>/praenora-runtime-pyenv.json/no-bytecode`, a path below a regular file, which can hold no file: no cached bytecode is read, neither there nor in a `__pycache__` beside a source, so every module is compiled from its source (so `py_compile.compile()` without `cfile=` in module code raises `NotADirectoryError` and `compileall` without `legacy=True` reports every file as failed) |
| `install_signal_handlers` | 0: the runtime keeps its own |
| `use_hash_seed`, `hash_seed` | 1 and 0: the iteration order of sets and of dicts of strings is the same in every run |
| `parse_argv`, `argv` | 0 and `[""]` |

The configuration is isolated: the variables of the process's environment that Python reads
(`PYTHONPATH`, `PYTHONHOME`, ...) and the user's site directory are ignored, and `site` still adds
the environment's `site-packages`. Since no cached bytecode is read, the code the process runs is
the code of the sources, which the directory digests cover; children that `multiprocessing` starts
are not given the prefix, and read the cached bytecode that lies beside a source, where there is
some ([Child processes](#child-processes)).

The interpreter is never finalized. Every call takes its lock with `PyGILState_Ensure` on the
calling thread, with the thread's own thread state: the core's first call on a thread that has
none makes one, binds it to the thread and keeps it, with the lock released, so each later call on
the thread finds it. It is cleared and deleted when its thread ends, as CPython ends the threads it
starts, except on the process's main thread, once the process exits and on Windows, where a
thread's end runs under the loader's lock: there it stays with the interpreter. The thread that
started the interpreter keeps the thread state it started with.

### In the interpreter

Before it serves calls, the core:

1. puts `sdk_dir` first on `sys.path`, before the environment's own entries, so that the
   environment cannot hide `praenora.sdk` (`praenora` stays a namespace package), and checks that
   `praenora.sdk` was imported from it (`PRAENORA_STATUS_DATA_INVALID` otherwise);
2. creates the module `praenora.sdk._native` (`log(level, message)`, `log_enabled(level)` and
   `cancel_requested()`, which act for the call that runs on the calling thread), puts it in
   `sys.modules` and gives it to `praenora.sdk._runtime.install()`;
3. calls `praenora.sdk._runtime.capture_output()` ([Output and logging](#output-and-logging)) and
   `configure_children()` ([Child processes](#child-processes)), which fails the start when it
   finds no `praenora.sdk._child_main`: the children would print to standard output.

Each call (`create_object` and `release_object` too) is bracketed by
`praenora.sdk._runtime.enter_call(host)` and `exit_call()`, with a `praenora.sdk.Host` whose backend
is the native module.

### Output and logging

Nothing that Python code writes through `sys.stdout`, `sys.stderr`, `logging` or `warnings` reaches
the process's descriptors 1 and 2, which carry the documents of the tool's `--json` output:

- `sys.stdout` and `sys.stderr` are text streams (UTF-8, with a binary `buffer`) whose lines become
  records at the levels `INFO` and `WARN`: one record per line, without its line feed. Each thread
  has its own unfinished line, which `flush()` and the end of the call that wrote it turn into a
  record of that call. Bytes written to the `buffer` are read as UTF-8, each sequence that is not
  UTF-8 becoming U+FFFD, and a lone surrogate in a record is written as U+FFFD. A sequence that a
  thread's bytes leave unfinished waits for its next bytes, past a `flush()`, until the end of the
  call, which writes U+FFFD for it into that call's record: it never reaches the next call.
- A handler on the root logger, whose level is `NOTSET`, turns each `logging` record into a record
  `"<logger name>: <message>"` (with the traceback of an exception that the record carries), at the
  level that maps it: `DEBUG` (10) to `PRAENORA_LOG_DEBUG`, `INFO` (20) to `PRAENORA_LOG_INFO`,
  `WARNING` (30) to `PRAENORA_LOG_WARN`, `ERROR` (40) to `PRAENORA_LOG_ERROR`, `CRITICAL` (50) and
  above to `PRAENORA_LOG_CRITICAL`, a level between two to the lower one and a level below 10 to
  `PRAENORA_LOG_TRACE`. It asks `log_enabled` first, so a record nobody keeps is not formatted.
- Warnings go through `logging` (`logging.captureWarnings`), one record each.

A record goes to the host of the call that runs on the writing thread, or, outside calls, to the
provider's log. `sys.__stdout__` and `sys.__stderr__` are left as they are, and so is a descriptor
that code writes to itself (`os.write(1, ...)`): code that uses them reaches the process's own
streams.

### Child processes

Modules and providers MAY start more interpreters as child processes (`multiprocessing`, which
`concurrent.futures.ProcessPoolExecutor` uses, and `subprocess` with `sys.executable`):

1. **The runtime is never forked.** `configure_children()` makes `spawn` the default start method of
   `multiprocessing` (`set_start_method("spawn", force=True)`), where Linux would fork the runtime's
   multi-threaded process. Code MAY still ask for `forkserver` with `get_context()`; `fork` is not
   supported. A child runs `sys.executable`, the environment's interpreter, with the flags of the
   parent, which `multiprocessing` passes on: isolated (`-I`), so the `PYTHON*` variables of the
   caller's environment never reach it, and without writing bytecode (`-B`). The parent's
   `pycache_prefix` is not among those flags, so a child reads the cached bytecode that lies beside
   a source (in a `__pycache__` directory, which the digests leave out) where there is some; the
   runtime writes none. It gets the parent's `sys.path`, the directories of modules included.
2. **Hash seeds.** The embedded interpreter's hash seed is 0, but an isolated interpreter ignores
   `PYTHONHASHSEED`, so the children of `multiprocessing` pick hash seeds of their own: the
   iteration order of a set of strings MAY differ between children and between runs, and code that
   runs in them MUST NOT depend on it. A process started with `subprocess` and the process's
   environment, without `-I` or `-E`, reads `PYTHONHASHSEED`: that is why `praenora` and
   `praenora-module-host` set it to `0` when it is unset, and why the core warns when it is not
   `0`.
3. **Children's output stays off standard output.** Children inherit the process's descriptors 1
   and 2. `configure_children()` sets `sys.modules["__main__"].__spec__` to the spec of
   `praenora.sdk._child_main`, so that `multiprocessing` runs that module as `__mp_main__` in every
   child it starts, before the child's work: it points descriptor 1 at descriptor 2 and
   `sys.stdout` at `sys.stderr`. What a child prints thus appears on the process's standard error,
   not in the step's records. A process started with `subprocess` is the module's own: give it
   `stdout=`.
4. **Children can import `praenora.sdk`** (its directory is on `sys.path`), but a child has no host:
   its `praenora.sdk.Host` drops records and never reports cancellation.

## The provider `python`

The provider `python` offers `praenora.python@0.0`. It is built with the Python core, only with
`PRAENORA_WITH_PYTHON_ENVIRONMENT`, and lies in the core's directory: in a build tree where its
runtime library looks for its installation's providers
(`<directory of the runtime library>/praenora/providers/python`; on Windows
`<runtime DLL directory>/../lib/praenora/providers/python`), so the build tree's tool and runtime
find it without a provider path, as an installation's are found. That directory holds the provider's
library and manifest, the core, the environment file and the core's `python/praenora/sdk/`.

Its version is the runtime's, and its engine object (in the manifest and in `describe`) names the
interpreter and the environment exactly, so a run's fingerprint
(`services.praenora.python@0.engine`) names the environment it used:

```json
{"name": "cpython", "version": "<python.version>",
 "environment": {"name": "praenora-runtime-pyenv", "version": "<version>",
                 "package": "<package>", "lock_sha256": "<lock.sha256>"}}
```

with the values of the environment's manifest when the build was configured. `praenora providers
list` lists it as any provider. Its `start()` starts the core as the next section says
([Loading and starting the core](#loading-and-starting-the-core)). Its table forwards each
function to the core: `create_object` passes the module's host API as the object's host, and
`call`, which takes none, leaves the object's own host for the call. Before a successful start,
`describe` returns `PRAENORA_STATUS_INVALID_STATE` and `create_object` and `call` fail with it.

An installation holds the same directory, `<libdir>/praenora/providers/python/`: the provider's
library and manifest, the core, the installation's environment file and the core's
`python/praenora/sdk/`, which are the build's copies (the core's digest checks them at its first
start); and the environment the build used, unchanged, in `<libdir>/praenora/python-environment/`,
where the installed core finds its interpreter library through a run path relative to itself
(`@loader_path/../../python-environment/python/lib`, `$ORIGIN/...`). An installation's environment
file names both relative to itself ([The environment file](#the-environment-file)), so the
installation can be moved ([developer-guide.md](../developer-guide.md#the-installation)).

## The providers that run Python

A provider that runs Python, the provider `python` among them, is a native library that links no
Python: it loads the process's one Python core when it starts, so the library loads in a process
without the environment, and a missing or wrong environment is a failure of its start with a clear
cause. The build gives it the environment's package and lock and the core's build identity, and the
code that loads the core (the loader, a static library that every such provider links) is part of
the files of the core's build identity: a provider and a core of one build agree.

### The environment file

Beside the provider's library lies `python-environment.json`:

```json
{"environment": "<directory>", "core": "<library file>"}
```

`environment` is the directory of the Python environment (the one that holds
`praenora-runtime-pyenv.json`) and `core` the core's library file, each relative to the file's own
directory or absolute. A build tree's file names the environment the build used and the build tree's
core by their absolute paths. An installation's names them relative to itself, so that the
installation can be moved: `../../python-environment` (from `<libdir>/praenora/providers/<name>`)
and the core's file in `<libdir>/praenora/providers/python`, which for the provider `python` is the
core's file name alone.

### Loading and starting the core

A provider's `start()` loads and starts the core in four steps; each failure fails the start, and
the runtime reports it as `LIBRARY_LOAD_FAILED` with `details.reason` `start` and the provider's
error document as `details.cause` ([runtime services](runtime-services.md)):

1. It finds its own library file (`dladdr()`; on Windows `GetModuleHandleExW()` and
   `GetModuleFileNameW()`), reads the environment file beside it and the environment's manifest,
   and checks that the environment's `package` and `lock.sha256` are those the provider was built
   for, and that the core and the environment's libpython exist.
2. Unless the core is loaded in the process already (another provider started it), it refuses a
   process that runs another Python interpreter: on POSIX systems, when the process's global scope
   holds the function `Py_IsInitialized` of a file that is not the environment's libpython (an
   interpreter that the program links, as the environment's own `python3.12` does, or that a
   library loaded globally; the Python front end imported into any interpreter is such a
   process); on Windows, when a `python3*.dll` is loaded from another directory than the
   environment's Python directory. The start fails with `PRAENORA_STATUS_UNSUPPORTED`, and the
   message says to run the step in a module host (isolation mode `process`), a process of its own.
3. Unless the core is loaded already, it loads the environment's libpython by its absolute path into
   the process's global scope (`dlopen(RTLD_NOW | RTLD_GLOBAL)`, which the extension modules of
   Linux need; on Windows `LoadLibraryExW()` of the DLL in the environment's Python directory, with
   `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`).
4. It loads the core by its absolute path (`RTLD_NOW | RTLD_LOCAL`; on Windows the flags of step 3,
   so that the core's import of the Python DLL is the one loaded), checks its table and its build
   identity against the provider's before it calls anything else of it, and calls the core's
   `start` with the environment's directory, the directory beside the core that holds
   `praenora/sdk` (`<core directory>/python`) and the provider's host ([Starting](#starting)).

| Situation | The cause's code | The cause names |
|---|---|---|
| No environment file beside the library | `PRAENORA_STATUS_NOT_FOUND` | the file (`details.file`) and the library |
| The environment file cannot be read; it is not a JSON object, or lacks `environment` or `core` | `PRAENORA_STATUS_IO_ERROR`; `PRAENORA_STATUS_DATA_INVALID` | the file (and the key) |
| The environment's directory holds no `praenora-runtime-pyenv.json` | `PRAENORA_STATUS_NOT_FOUND` | the environment file, the directory and the manifest |
| The manifest is not JSON, or lacks a value | `PRAENORA_STATUS_DATA_INVALID` | the manifest and the key |
| Another package or lock than the provider's | `PRAENORA_STATUS_UNSUPPORTED` | both packages and both locks (`details.lock_sha256`, `expected_lock_sha256`, ...) |
| The core or the environment's libpython does not exist | `PRAENORA_STATUS_NOT_FOUND` | the file (`details.core` or `details.library`) |
| Another Python interpreter in the process | `PRAENORA_STATUS_UNSUPPORTED` | its file (`details.interpreter`); says to use a module host |
| libpython or the core does not load | `PRAENORA_STATUS_INTERNAL` | the file and the system's text |
| The core is not a core of this interface, or of another build | `PRAENORA_STATUS_UNSUPPORTED` | the core, and both build identities (`details.build_id`, `expected_build_id`) |
| The core's start fails | the core's ([Starting](#starting)), such as `PRAENORA_STATUS_DATA_INVALID` for `praenora.sdk` sources changed after the build | as the core's document says |

A provider starts once in a process, so a process whose provider failed to start does not try
again: a run in a new process does.

## Providers implemented in Python

A service implemented in Python keeps its typed table and its header: the provider is a native
library of this repository whose table functions forward to a Python object of its own, and its
glue, written in C++ for each service, turns the arguments of a table function into a JSON
document and the document the object returns into the function's results. No service of documents
is added for them. What builds them is internal, as the provider interface is.

**The build.** `praenora_add_python_provider()`
([`cmake/PraenoraPythonProvider.cmake`](../../cmake/PraenoraPythonProvider.cmake)) calls
`praenora_add_provider()` and links the provider to `praenora_python_provider_support`, the loader
and the glue (no Python header, no Python link):

```cmake
praenora_add_python_provider(<target>
    NAME <name> VERSION <semver> SERVICES <service>@<major>.<minor>... SOURCES <source>...
    PYTHON_SOURCES <file-or-directory>... [PYTHON_ROOT <dir>] [ENGINE_JSON <json>]
    [OUTPUT_DIRECTORY <dir>] [INSTALL_DESTINATION <dir>])
```

- The Python sources, files and directories relative to `PYTHON_ROOT` (by default the current
  source directory), every file below a directory but those below `__pycache__`, are copied at
  build time into `python/` beside the library, which then holds them and nothing else, keeping
  their paths below `PYTHON_ROOT`. Their directory digest
  ([The directory digest](#the-directory-digest)) is compiled into the library
  (`PRAENORA_PYTHON_PROVIDER_SOURCES_SHA256`), so the library's SHA-256, which the run fingerprint
  records, changes with every byte of them. An edit of a source configures the build again.
- The engine object of the manifest and of `describe()` is `ENGINE_JSON` as written (a line break
  and the white space around it one space, as `praenora_add_provider()` writes it), with the entry
  `"environment": {"lock_sha256", "name", "package", "version"}` of the environment after its last
  one, so that the fingerprint (`services.<service>@<major>.engine`) names the environment exactly;
  the text of both is kept byte for byte. `ENGINE_JSON` MUST NOT have an `environment` of its own
  (the configuration stops). Without `ENGINE_JSON` the engine object is the provider `python`'s.
- The environment file ([The environment file](#the-environment-file)) is written beside the
  library; with `INSTALL_DESTINATION` the library, its manifest, the installation's environment
  file and the sources (in `python/`) are installed there.

**The start.** `PythonProviderObject::start(host, callable, error_json, &status)`
([`service_glue.hpp`](../../src/providers/python/common/service_glue.hpp)), called by the
provider's `start()`, loads and starts the core
([Loading and starting the core](#loading-and-starting-the-core)), then creates the provider's
Python object with the core's `create_object` ([create_object](#create_object)): `callable`
(`"package.module:attribute"`) is called with the document `null`, from `<directory of the
provider library>/python` and with the digest compiled into the library, without a module's host,
so the object's records go to the provider's log. The object is created once and kept for the rest
of the process. Each failure fails the provider's start (`LIBRARY_LOAD_FAILED`, `details.reason`
`start`, the error document below as `details.cause`):

| Situation | The cause's code | The cause names |
|---|---|---|
| A failure of the loader or of the core's start | as in [Loading and starting the core](#loading-and-starting-the-core) | as there |
| The files below `python/` differ from those the library was built with (edited, added or removed after the build) | `PRAENORA_STATUS_DATA_INVALID` | the directory and both digests (`details.path`, `expected_sha256`, `actual_sha256`) |
| `python/` does not exist or cannot be read | `PRAENORA_STATUS_IO_ERROR` | the directory |
| The provider's top-level package is loaded already from elsewhere | `PRAENORA_STATUS_INVALID_ARGUMENT` | the package and where it was loaded from |
| The callable cannot be imported, does not exist or raises | `PRAENORA_STATUS_INTERNAL` | the exception, with `details.traceback` |
| The provider was built without Python sources | `PRAENORA_STATUS_INVALID_STATE` | |

**The calls.** `call(module_host, method, args_json, result_json, error_json)` calls a method of
the object as the service's `call` does ([call and release_object](#call-and-release_object)): the
same documents, limits and error documents. `module_host` is the host API of the module that called
the table function, when the function has one, and the host of the call: the call's records and its
cancel flag are that module's; without one (NULL) they are the provider's. Before a successful
start it fails with `PRAENORA_STATUS_INVALID_STATE`. Every module's calls may come at once, from
their own threads: the core keeps each thread's records apart and the interpreter runs their
Python code one thread at a time, so the object serves them when its methods keep no state from one
call to the next, or guard it.

**One interpreter.** The provider's package is imported into the process's one interpreter, which
the provider `python` and every other provider that runs Python start together (the core's start
called once for each, the first one starting the interpreter): its top-level package MUST be its
own, since the import isolation of `create_object` refuses a package of one name from two
directories, and the Python code of the process's modules sees it in `sys.modules`. Its code is
fixed for the process, as a module's is. In a module host, a process of its own, the provider
starts for the step.

**The fixture provider `test-python-echo`** (`tests/fixtures/providers/test-python-echo/`) is the
example; no product provider is implemented in Python in this version. It offers the test service
`test.echo@0.0` (`tests/support/test_echo.h`), its echo implemented by the class
`test_python_echo:Echo`; its glue sends the input's bytes in hexadecimal (`{"input": "<hex>"}`),
which carries any bytes in a JSON document, passes the calling module's host API when the echo is
given one, and writes the bytes that the hexadecimal text the method returns spells; a failed call
returns its status and logs its error document to the provider's log, as the echo has no error
sink. Its engine object is `{"name": "test-python-echo-engine", "version": "1.0.0",
"environment": {...}}`.

**Beside the roadrunner provider.** The environment's `roadrunner` package brings a libRoadRunner
and an LLVM of its own. A run whose step is served by the roadrunner provider
([simulation-service.md](simulation-service.md#the-roadrunner-provider)) and whose Python step
simulates with that package runs in one process (`PythonRoadrunner.CoexistInOneProcess`, verified
on macOS arm64 with the provider loaded first, as a run loads it, before the Python step imports
the package; the other order and Linux and Windows not yet verified). Module hosts keep them apart
in any case, and are where Python steps should run
([user-guide.md](../user-guide.md#finding-service-providers)).

## The process's environment

No provider and no core sets a variable of the process's environment: they run where other threads
may read it. The runtime's own programs set what the environment needs first thing in `main()`,
before any thread starts, when their tree has the provider `python`, that is when the provider's
environment file exists. Each program is built with the path of an installation's file relative
to its own installed directory, from the installation's directories (GNUInstallDirs): `praenora`,
in `bin/`, at `<program directory>/../<libdir>/praenora/providers/python/python-environment.json`,
and `praenora-module-host`, in `libexec/praenora/` (`<libdir>/praenora/../../libexec/praenora`),
at the corresponding path, `<libdir>` being `lib`, `lib64` or the like. When no file is there, it
looks where a build tree has it, below `lib/`: `praenora` at
`<program directory>/../lib/praenora/providers/python/python-environment.json`, and
`praenora-module-host` at
`<program directory>/../../lib/praenora/providers/python/python-environment.json`. Then:

- each variable of the environment manifest's `environment.paths` but `PYTHONHOME` (the core gives
  the interpreter its home itself) is set to its absolute path below the environment's directory;
- `PYTHONHASHSEED` is set to `0` ([Child processes](#child-processes));

each only when it is unset: a value the caller set stays, and the core's warning names it when it
differs. The code names no variable of the manifest. A file that cannot be read sets no path (the
provider's start reports it); `PYTHONHASHSEED` is set whenever the file exists.
`environment.values` are not set: the core's isolated configuration has their effect. A module host
started by another program that embeds the runtime sets them too. Any other process (one that
uses the C++ API, or the Python front end) sets them itself before it starts threads, or the core
warns at its start about each that is unset or has another value.
