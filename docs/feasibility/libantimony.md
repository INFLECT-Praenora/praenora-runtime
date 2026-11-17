# libAntimony model conversion service: feasibility design

- **Status:** Feasibility design, informative. Nothing in this document is implemented. The build
  option `PRAENORA_WITH_ANTIMONY` is a name proposed here; it does not exist
  ([`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake)). The service table below is a
  draft: it is not part of ABI v0, and adopting it is a versioned change with its own ADR
  ([compatibility-policy.md](../specs/compatibility-policy.md)).
- **Decision record:** [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md) (Proposed).
  The companion simulation service is in [libroadrunner.md](libroadrunner.md).
- **Sources:** upstream facts were checked on 2026-11-11 against libAntimony 3.3.0. They are cited
  as [A1] to [A17] and listed under Sources at the end. A claim that could not be checked is marked
  "(unverified)".

## Question and recommendation

**Question.** Can the runtime offer translation between Antimony and SBML to modules as the runtime
service `praenora.model-conversion@0`, backed by libAntimony? libAntimony keeps process-global state
and opens the files that an Antimony `import` statement names, and `ode-uq-spec` requires Antimony
sources to be checked for `import` before they are translated. What would a safe service look like,
and how would it be built and packaged?

**Recommendation.** It is feasible as an optional runtime service:

- Build it only with `PRAENORA_WITH_ANTIMONY=ON` (default OFF), with libAntimony and libSBML linked
  statically and PRIVATE into the runtime, behind its hidden symbol visibility. The runtime's copy of
  libAntimony's global state is then its own: no module can reach it.
- Serialize every use of libAntimony behind one process-wide mutex, and run each conversion as a
  complete sequence under that lock (clear, load, translate, copy out, free, clear), so that no state
  survives a call.
- Apply the `ode-uq-spec` rules before anything is translated: refuse an Antimony source that
  contains an `import` statement, and refuse SBML text that contains a document type declaration or
  requires a Level 3 package. Load text only, through the loader for the declared format, and never
  through a function that takes a file name or guesses the format.
- When both services are built, each has its own libSBML: the simulation service's is inside its
  provider, so a process holds two copies unless both services come from one provider.
- The service can be part of a WebAssembly build of the runtime: libAntimony has been compiled with
  Emscripten ([webassembly.md](webassembly.md)).

## Options compared

| Option | Summary | For | Against |
|---|---|---|---|
| C1. Runtime service (recommended) | `PRAENORA_WITH_ANTIMONY=ON` compiles the service into `praenora_runtime`; it is registered when a `Runtime` is constructed. | One audited place applies the `ode-uq-spec` checks and serializes the global state; modules stay small. | The runtime carries libAntimony and libSBML when built with the option. |
| C2. Each module bundles libAntimony | Possible today. | No runtime change. | Every module must implement the import check itself. Two modules that load the same shared libAntimony share one copy of its global state, because the dynamic loader returns the same handle for the same library [A14], and no lock covers both. |
| C3. Out-of-process conversion | A helper executable per conversion. | Isolates the global state and could run without file system access. | The runtime has no subprocess facility; process start per conversion; out of scope for the MVP (ADR-0021). |
| C4. libAntimony compiled to WebAssembly, run by an embedded engine | A converter module inside the engine of [webassembly.md](webassembly.md), Model B, with no file system imports. | The sandbox makes `import` harmless: there is no file to open. | Depends on Model B existing; slower than native. A later option. |
| C5. No conversion | Modules accept SBML only. | Nothing to build. | `ode-uq-spec` allows Antimony model sources, so a module that reads `ode-uq-spec` models would bundle a translator anyway (C2). |

## Integration design

### Where the service plugs in

As the simulation service was first planned, before service providers took it out of the runtime
library ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md)): the runtime registers one
static table in its [`ServiceRegistry`](../../src/runtime/instance/services.hpp) as
`praenora.model-conversion` at major 0 before instances use the registry; a module looks it up with
`get_service(context, "praenora.model-conversion", 25, 0)` and gets NULL from a runtime built
without the option. Lookups are logged as `service.lookup`
([module-abi.md](../specs/module-abi.md#host-api)). The table and the service's state live until the
process ends, because an abandoned instance may still be inside a call
([threading-and-ownership.md](../specs/threading-and-ownership.md#unresponsive-instances)).

### Draft service table (not part of ABI v0)

```c
/* DRAFT: praenora.model-conversion@0, for discussion only. Not part of ABI v0 and not
 * implemented. */
#include <praenora/sdk/abi.h>

#define PRAENORA_MODEL_CONVERSION_SERVICE "praenora.model-conversion"
#define PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR 0u

typedef struct praenora_model_conversion_v0 {
    praenora_api_header header; /* abi_major: the service major (0); abi_minor; struct_size of
                                   this table as compiled into the runtime; reserved: 0 */
    void* context;              /* service-owned; pass it to every function */

    /* Writes the service's identity: {"service", "version" {major, minor}, "engine" {name,
     * version, libsbml}, "formats": ["antimony", "sbml"]}. Thread-safe, free of side effects. */
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);

    /* Translates one self-contained model source. `options_json` is a JSON object
     * {"from": "antimony" | "sbml", "to": "sbml" | "antimony", "module": optional name}; `source` is
     * the model text (UTF-8, not NUL-terminated). On success writes the translated text to
     * out_text and {"module", "warnings": [...], "engine" {...}} to report_json. On failure returns
     * a status other than OK and writes an error document to error_json. `host` is the calling
     * instance's host API: the service logs through it and polls cancel_requested() while it waits
     * for the lock. Thread-safe: calls are serialized inside. */
    praenora_status (PRAENORA_CALL *convert)(void* context, const praenora_host_api* host,
                                             praenora_bytes options_json, praenora_bytes source,
                                             praenora_sink* out_text, praenora_sink* report_json,
                                             praenora_sink* error_json);
} praenora_model_conversion_v0;
```

The rules are those of the simulation table, which mirror the module ABI: the header carries the
service's version and minor versions only append; the module passes sinks it owns and the service
writes through them during the call; inputs are borrowed for the call; statuses are the general
codes of [`status.h`](../../include/praenora/sdk/status.h); errors have the `pmp-error` shape with
`details.service` and, for a libAntimony failure, `details.engine_error`; no exception or `longjmp`
leaves a service function. The source travels as raw text rather than inside JSON, so a large SBML
document need not be escaped into a JSON string.

### The conversion sequence

`convert` does this, in this order:

1. **Check the request** without the lock: the options are a JSON object with known fields, the
   source is valid UTF-8, contains no NUL byte (libAntimony's loaders take NUL-terminated strings
   [A4]) and is within the size limit (for example 16 MiB).
2. **Apply the security checks** described under Security below, still without the lock. A refused
   source never reaches libAntimony or libSBML.
3. **Take the lock**, polling `cancel_requested()` while waiting; return
   `PRAENORA_STATUS_CANCELLED` if it turns nonzero.
4. `clearPreviousLoads()` and `clearDirectories()`, so no earlier load and no search directory is
   left over.
5. Load with `loadAntimonyString()` or `loadSBMLString()`, according to `from`. Both return the index
   of the stored set, or -1 with the error saved [A4].
6. Choose the module: `module` from the options, else `getMainModuleName()`.
7. Translate with `getSBMLString(module)` or `getAntimonyString(module)` [A4], and copy the text into
   `out_text`; copy `getWarnings()` and the error text, if any, into the report or the error
   document. Error text is made valid UTF-8 first: `ode-uq-spec`'s reference validator
   (`tools/validate.py`) handles libAntimony error text that quotes part of a non-ASCII character.
8. `freeAll()`, then `clearPreviousLoads()`, then release the lock.

The service never calls `free` on a pointer libAntimony returned: `freeAll()` frees every pointer the
library handed out, and calling it after freeing any of them yourself crashes the program [A4]. Using
`freeAll()` inside the lock also avoids freeing across C runtimes on Windows, a pitfall libSBML
documents for static MSVC runtimes [A12].

### The C API used

| Function | Upstream behavior [A4] | Use |
|---|---|---|
| `long loadAntimonyString(const char*)` | Parses Antimony; returns the index of the stored set, or -1 with the error saved and nothing stored. | Antimony input. |
| `long loadSBMLString(const char*)` | Parses SBML with libSBML; "By default, function definitions are removed." | SBML input. |
| `loadString`, `loadFile`, `loadAntimonyFile`, `loadSBMLFile` | Any format the library knows, CellML included; file names are absolute or relative to the directory the executable runs from. | Never used. |
| `char* getMainModuleName()` | NULL only when there are no modules. | Default module. |
| `char* getSBMLString(const char*)`, `char* getAntimonyString(const char*)` | The translated text of a module; `getAntimonyString` returns NULL on failure and sets an error. | Output. |
| `char* getLastError()`, `char* getWarnings()` | No handle: the last error and warnings of the process. | Error document and report. |
| `void clearPreviousLoads()` | Clears memory of all loaded files; every successful load is kept until then. | Start and end of each call. |
| `void addDirectory(const char*)`, `void clearDirectories()` | Directories searched for imported files and for a `.antimony` file of rules about where imports are found. | `clearDirectories()` at the start of each call; `addDirectory` never. |
| `void freeAll()` | Frees every pointer the library handed out. | End of each call. |
| `char* getVersionStr()` | The library's version. | `describe` and the report. |

The header, generated from the template `src/antimony_api.h.in` [A6], calls these functions "a
plain C API" and declares them with C linkage macros. Returned strings are allocated with `malloc`
and owned by the caller, and the library returns NULL when it runs out of memory for a returned
pointer [A4].

### Error mapping

| Condition | Outcome |
|---|---|
| Options are not a JSON object, have an unknown field, or `from` and `to` are not a supported pair | `PRAENORA_STATUS_INVALID_ARGUMENT` |
| The source is not valid UTF-8 or contains a NUL byte | `PRAENORA_STATUS_INVALID_ARGUMENT` |
| A security check refuses the source (import, document type declaration, required package, external model reference) | `PRAENORA_STATUS_DATA_INVALID`, with `details.rule` naming the check |
| libAntimony cannot parse or translate the source (a load returns -1, or a translation returns NULL with an error set) | `PRAENORA_STATUS_DATA_INVALID`, with libAntimony's message in `details.engine_error` |
| The requested module does not exist | `PRAENORA_STATUS_NOT_FOUND` |
| The source exceeds the size limit, `std::bad_alloc`, or NULL from the library for lack of memory | `PRAENORA_STATUS_RESOURCE_EXHAUSTED` |
| A sink refuses a write | The status `write` returned |
| `cancel_requested()` turned nonzero while waiting for the lock | `PRAENORA_STATUS_CANCELLED` |
| Any other exception | `PRAENORA_STATUS_INTERNAL` |

## The libSBML dependency

- libAntimony depends on libSBML and on SBMLNetwork; its README points to prebuilt versions in
  `libroadrunner-deps` and recommends expat as libSBML's XML library [A1].
- libSBML is at release 5.21 (the repository badges show 5.21.0; the API reference documents
  5.21.2), is distributed under the LGPL 2.1, and can use Xerces, Expat or libxml2 [A10][A11]. The
  parsers "behave slightly differently when reading files" [A12], so the service is built with one,
  expat, as the Emscripten build of libAntimony is [A8].
- libRoadRunner finds a static libSBML of at least 5.20.5 [A16]. The simulation service's libSBML
  is linked into the provider `roadrunner`, a separate library that exports only its entry point
  ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md)), and that provider also calls libSBML
  itself. A libSBML linked into the runtime for this service would be a second, separate copy with
  its own process-wide state, so the two neither collide at link time nor share state (unverified
  with both built); they would share one copy only if both services came from one provider.
- libSBML keeps a process-wide singleton, the package extension registry [A11]. Neither the
  registry's page nor the list of known pitfalls says anything about thread safety [A11][A12]; the
  service's lock covers its own libSBML use. The simulation service's provider uses its own copy,
  with its own registry, so this lock need not cover it (unverified with both built).
- Linking LGPL-2.1 code statically into a distributed binary brings the conditions of section 6 of
  that license, such as means to relink with a modified library [A15]. This needs the same legal
  review as for the simulation service.

## Global state and thread safety

- libAntimony's state is global: its registry is declared `extern Registry g_registry;`, holds the
  files, directories and current imports of the parse, and its header has no mutex or thread
  declaration [A5]. The last error and the warnings have no handle [A4], and loaded models stay in
  memory until `clearPreviousLoads()` [A4].
- Hence one process-wide mutex, held for the whole sequence. Concurrent callers queue; the wait is
  cancellable, the translation is not (the header documents no way to interrupt it, unverified), and
  the size limit bounds how long the lock is held.
- The mutex protects only the runtime's copy. A module that loads a shared libAntimony of its own,
  at the same path as one the runtime loaded, would share that copy's state, because `dlopen()` of
  the same object returns the same handle [A14]; Windows behaves the same way for a DLL already
  loaded (unverified). Static linking with hidden symbols gives the runtime a private copy, which is
  why it is recommended.
- The calls are short and serialized, so the service does not care which thread calls it: a module's
  worker thread or one of the module's own threads, which must be joined before its `destroy`
  returns ([threading-and-ownership.md](../specs/threading-and-ownership.md#what-a-module-must-do)).

## Determinism, threading and security

### Determinism

- A translation is a pure function of the source text, the options and the library versions:
  nothing else enters it, because each call starts and ends with `clearPreviousLoads()`. That two
  identical calls give byte-identical text, in one process and across processes, is expected and
  checked by the spike (unverified).
- The report carries the libAntimony and libSBML versions. Offered by a service provider, the
  service would enter the run's reproducibility section as the simulation service does (its
  `services` entry: provider identity, digests and the manifest's `engine` object,
  [reproducibility.md](../specs/reproducibility.md#run-fingerprint)), with no change to the run
  record format. Registered in the runtime library, as under
  [Where the service plugs in](#where-the-service-plugs-in), it would not be recorded (the section
  holds only services that providers serve), and recording it would be a run record format change
  with its own ADR.
- Main module: `ode-uq-spec` 0.0.0 defines an Antimony source's meaning as the flattened SBML of
  its main module: the module marked `*`, else `__main` when the file has top-level statements, else
  the last module defined (`specification.md`, Model specification). The service uses
  `getMainModuleName()`; that it applies the same rule in every case is unverified.
- `loadSBMLString()` removes function definitions by default [A4], so an SBML to Antimony
  translation may not keep them as function definitions (whether their uses are expanded in place is
  unverified); the report says so when the source has any.

### Threading

See Global state and thread safety. The service needs no worker thread of its own and starts none.

### Security

The runtime never evaluates expressions or model sources; this service is the first place where
runtime code would parse model sources, so it applies `ode-uq-spec`'s rules itself.

- **The rules.** `ode-uq-spec` 0.0.0 treats expression strings and model sources as data, not code;
  forbids XML document type declarations in model sources, which enable entity-expansion attacks;
  says Antimony sources must not `import` other files; and says to check for `import` before
  translating, because libAntimony opens imported files (`conventions.md`, Security). Its
  normative text adds that an Antimony source must be self-contained, with no `import`, and that an
  SBML source must not require a Level 3 package, which excludes `comp` (`specification.md`, Model
  specification).
- **How libAntimony imports.** An Antimony file imports another with `import "file"`; SBML files can
  be imported the same way; a relative name is resolved against the directory of the working file;
  other directories can be configured; and `.antimony` files can redirect imports [A7][A4]. The C API
  documents no function or flag that turns import processing off [A4].
- **The import check.** Before calling libAntimony, the service removes comments (`//` and `#` to
  the end of the line, `/* ... */`), double-quoted strings and text between triple backticks, and
  refuses the source if the word `import` remains, in any letter case. This is the check of
  `ode-uq-spec`'s reference validator (`tools/validate.py`). It refuses a little more than
  necessary (an identifier spelled `Import`, for instance), which is the safe direction.
- **Defense in depth.** `clearDirectories()` before each call; no file name is ever passed; the
  runtime never changes the process's working directory; CellML support is left out of the build (the
  loaders skip CellML when built without it [A4], and the Emscripten build turns it off [A8]).
- **SBML input.** Text containing `<!DOCTYPE`, in any letter case, is refused before any XML parser
  sees it. After parsing with libSBML, a document that declares a required package is refused, which
  also refuses the `comp` package's external model definitions, whose `source` may be a URL, URN or
  file location [A13].
- **Limits.** A size limit on the source and the sink limits on the output bound memory and time; a
  translation runs under the lock, so a pathological source delays other callers, never the whole
  process.

## WebAssembly outlook

- **The runtime compiled to WebAssembly** ([webassembly.md](webassembly.md), Model A).
  `libantimonyjs` builds expat, a static libSBML (with expat, without libxml2) and libAntimony
  (without CellML) with Emscripten's CMake and make wrappers, and links them into a single-file
  JavaScript and WebAssembly module with memory growth, exporting C functions such as `_loadString`,
  `_getSBMLString` and `_freeAll` [A8]. MakeSBML, a client-side web application that converts
  between Antimony and SBML [A9], loads that `libantimony.js` module [A17]. The service can therefore
  be compiled into a WebAssembly runtime with the same flags. The import check still applies: the
  in-memory file system holds whatever the embedder wrote into it.
- **WebAssembly modules in a native runtime** (Model B). The service stays native; WebAssembly
  modules would reach it through host imports that mirror the table above, in a later version.
  Running libAntimony itself as a sandboxed WebAssembly module (option C4) would remove file access
  altogether.

## Packaging and build

- **Upstream packages.** libAntimony 3.3.0 (2026-10-28) is on PyPI as wheels only, for `win_amd64`,
  `manylinux_2_28_x86_64`, `macosx_15_0_x86_64` and `macosx_14_0_arm64`, 2.7 to 5.6 MB each, with no
  source distribution [A3]. The GitHub release carries zipped Python wheels and Julia archives; no C
  library archive is visible among its listed assets [A2]. A C or C++ consumer builds from source.
- **Upstream build.** CMake, with libSBML and SBMLNetwork required; Python, Qt (for its editor),
  Julia and GoogleTest optional; options such as `ANT_DEPENDENCIES_INSTALL_PREFIX`, `WITH_PYTHON`,
  `WITH_QTANTIMONY`, `WITH_GTEST` and `WITH_JULIA`; Bison only to regenerate the parser; Windows
  builds in an MSVC x64 environment [A1]. Whether an install provides a CMake package configuration
  is unverified; this project would use a find module.
- **In this project.** `PRAENORA_WITH_ANTIMONY` (OFF) in
  [`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake); when ON, a static libAntimony and
  libSBML found through `CMAKE_PREFIX_PATH`, compiled into `praenora_runtime` as an OBJECT library
  with the third-party libraries linked PRIVATE ([ADR-0017](../adr/ADR-0017-dependency-management.md)).
  No FetchContent pin in the first version: the dependency chain (libSBML, SBMLNetwork, expat) would
  need pins of its own. A default build never uses the network.
- **Linux x86-64.** Source build; the upstream wheels target manylinux 2.28. No Linux arm64 wheel is
  published [A3].
- **macOS arm64.** Source build for the project's deployment target of 11.0 (unverified that the
  sources support it); the upstream arm64 wheel targets macOS 14 [A3].
- **Windows x86-64.** Source build with MSVC [A1]. Keep the shared MSVC runtime, and free only
  through `freeAll()` (see the conversion sequence).
- **Licensing facts.** The repository page states BSD-3-Clause [A1], while the PyPI metadata of
  3.3.0 states MIT [A3]; the discrepancy should be settled upstream before redistribution. libSBML is
  LGPL 2.1 [A10]. The licenses of SBMLNetwork and expat were not checked (unverified). This project
  states no license of its own (OQ-1).

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| An `import`, or a reference to another document, slips past the checks | libAntimony or libSBML opens a file chosen by the source | The validator's conservative check, `clearDirectories()`, no file names, SBML package and document type checks; spike tests in every comment and string form; later, option C4. |
| Another copy of libAntimony in the process shares global state | Races between the service and a module | Static, hidden linking of the runtime's copy; module authors told to use the service rather than their own copy. |
| Two libSBML copies in one process (the runtime's and the provider's) | Unexpected interaction | Test both options together. |
| Function definitions removed on SBML load | An SBML to Antimony translation differs in form, or in meaning, from its source | Warning in the report; spike checks what the default does to models with function definitions. |
| The main module chosen by libAntimony differs from the `ode-uq-spec` rule | Translations of multi-module files differ from other consumers | Spike checks the three cases of the rule. |
| License discrepancy (BSD-3-Clause or MIT) and LGPL-2.1 libSBML | Distribution blocked | Ask upstream; legal review together with the simulation service (OQ-1, OQ-13). |
| Long translations under one lock | Callers wait; deadlines pass while waiting | Size limit; cancellable wait; translations are expected to be short (unverified). |

## Spike plan

Each step has an exit criterion; the spike stops at the first that fails and records why.

1. **Link.** Build libAntimony 3.3.0 and libSBML 5.21 (expat) statically on macOS arm64 and link
   them into a scratch OBJECT library of the runtime with hidden visibility. *Exit:* warning-free
   build, `ctest --preset dev` unchanged, and the runtime exports exactly the symbols it exports
   today.
2. **Round trip.** Implement the draft table and translate a small self-contained Antimony model to
   SBML and back. *Exit:* the two SBML documents agree on species, parameters, reactions and rules
   (compared with libSBML).
3. **Import check.** Fixtures with `import` in code, in each comment form, in strings, in triple
   backticks and in other letter cases. *Exit:* every code-level `import` is refused with
   `details.rule` before libAntimony is called (shown by a fixture whose import names a file that
   does not exist: the error must be the service's, never libAntimony's file error), and every
   mention inside a comment or string is accepted.
4. **SBML checks.** Fixtures with a document type declaration, an entity, a required package and a
   `comp` external model definition. *Exit:* each is refused before translation.
5. **Concurrency.** Eight threads call `convert` at once under a ThreadSanitizer preset
   (`macos-arm64-tsan`, `linux-x64-tsan`). *Exit:* no report, and outputs identical to sequential
   calls.
6. **Determinism.** The same conversion twice in one process and in two processes, and on Linux and
   Windows. *Exit:* byte-identical text, or the differences are documented.
7. **`ode-uq-spec` cases.** A file with a module marked `*`, one with top-level statements, and one
   with neither. *Exit:* `getMainModuleName()` agrees with the `ode-uq-spec` rule in all three, or
   the service implements the rule itself.
8. **Both services.** Build with `PRAENORA_WITH_ANTIMONY` and `PRAENORA_WITH_ROADRUNNER` together.
   *Exit:* both services' tests pass in one process, with the runtime's libSBML and the provider's.
9. **WebAssembly.** Build the service with Emscripten using the flags of `libantimonyjs`. *Exit:*
   steps 2 and 3 pass under Node.

## Sources

Every page was read on 2026-11-11. The `ode-uq-spec` 0.0.0 rules cited by name above were read in
that specification's own repository (`docs/conventions.md`, `docs/specification.md` and the
reference validator `tools/validate.py`); no public URL is cited for it.

| Id | Source | Supports |
|---|---|---|
| A1 | [sys-bio/antimony, repository page](https://github.com/sys-bio/antimony) | A modular, human-readable model definition language with a C/C++ library and Python and Julia bindings; BSD-3-Clause; libSBML and SBMLNetwork required, expat recommended; CMake options; optional Python, Qt, Julia and GoogleTest; Windows, macOS and Linux builds. |
| A2 | [sys-bio/antimony, releases](https://github.com/sys-bio/antimony/releases) | v3.3.0 is the latest release (Julia bindings; zip and bzip support in libSBML); its visible assets are zipped Python wheels, Julia archives and a macOS debug archive, with no C library archive. |
| A3 | [antimony on PyPI](https://pypi.org/project/antimony/#files) | 3.3.0 of 2026-10-28; wheels for four platforms, 2.7 to 5.6 MB, no source distribution; license metadata "MIT". |
| A4 | [antimony `src/antimony_api.h.in`](https://raw.githubusercontent.com/sys-bio/antimony/HEAD/src/antimony_api.h.in) | The C API: loaders and their return values, translation functions, handle-less errors and warnings, `clearPreviousLoads`, `addDirectory`, `clearDirectories`, `.antimony` files, `freeAll` and its warning, `malloc`-owned results, `getVersionStr`, file names relative to the working directory, no function that disables imports. |
| A5 | [antimony `src/registry.h`](https://raw.githubusercontent.com/sys-bio/antimony/HEAD/src/registry.h) | `extern Registry g_registry;` with file, directory and import state; no mutex or thread declarations. |
| A6 | [antimony `src/` directory](https://github.com/sys-bio/antimony/tree/HEAD/src) | The API header is the template `antimony_api.h.in`; `registry.h` and `registry.cpp` hold the registry. |
| A7 | [Antimony Reference](https://tellurium.readthedocs.io/en/latest/antimony.html) | `import "file"` syntax; SBML files can be imported; relative names resolved against the working file's directory; other directories configurable; `.antimony` files can redirect imports; bare declarations belong to `__main`. |
| A8 | [sys-bio/libantimonyjs](https://github.com/sys-bio/libantimonyjs) | Emscripten build of expat, libSBML and libAntimony into a single-file module with memory growth; CellML off; exported C functions. |
| A9 | [MakeSBML: A tool for converting between Antimony and SBML](https://arxiv.org/abs/2309.03344) | A client-based web application converting between Antimony and SBML. |
| A10 | [sbmlteam/libsbml](https://github.com/sbmlteam/libsbml) | LGPL 2.1; release 5.21.0; Xerces, Expat or libxml2; Linux, Windows and macOS. |
| A11 | [libSBML C++ API: `SBMLExtensionRegistry`](https://sbml.org/libsbml/class_s_b_m_l_extension_registry.html) | The package extension registry is a singleton; nothing about threads; documents libSBML 5.21.2. |
| A12 | [Known libSBML problems and pitfalls](https://sbml.org/software/libsbml/libsbml-docs/known-pitfalls) | XML parsers behave slightly differently; explicit freeing of memory with static MSVC runtimes; nothing about thread safety. |
| A13 | [libSBML `ExternalModelDefinition` class reference](https://sbml.org/software/libsbml/5.18.0/docs/formatted/cpp-api/class_external_model_definition.html) | The `comp` package's `source` may be a URL, URN or relative or absolute file location. |
| A14 | [dlopen(3), Linux manual page](https://man7.org/linux/man-pages/man3/dlopen.3.html) | Opening the same shared object again returns the same handle; `RTLD_LOCAL` only limits symbol resolution. |
| A15 | [GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) | Section 6: conditions for distributing a work linked with the library. |
| A16 | [roadrunner `CMakeLists.txt` (develop)](https://raw.githubusercontent.com/sys-bio/roadrunner/develop/CMakeLists.txt) | libRoadRunner requires a static libSBML of at least 5.20.5. |
| A17 | [sys-bio/makesbml](https://github.com/sys-bio/makesbml) | The MakeSBML site loads a JavaScript wrapper of a WebAssembly build of libAntimony (`libantimony.js`) from the `libantimonyjs` repository. |
