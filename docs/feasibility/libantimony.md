# libAntimony model conversion service: upstream facts and future work

- **Status:** Informative. The model conversion service `praenora.model-conversion@0` is specified
  in [model-conversion-service.md](../specs/model-conversion-service.md),
  [`model_conversion.h`](../../include/praenora/sdk/model_conversion.h) and
  [ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md), and built: the service provider
  `antimony` ([ADR-0026](../adr/ADR-0026-runtime-service-providers.md),
  [runtime-services.md](../specs/runtime-services.md)), which translates Antimony to SBML and back
  with libAntimony 3.3, built only with the build option `PRAENORA_WITH_ANTIMONY=ON`
  ([`PraenoraOptions.cmake`](../../cmake/PraenoraOptions.cmake)). Hierarchical SBML output, CellML
  and a WebAssembly build are future work ([Future work](#future-work)). Where this document and the
  specifications differ, the specifications hold.
- **Decision records:** [ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md) (Proposed),
  which amends [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md) as
  [ADR-0026](../adr/ADR-0026-runtime-service-providers.md) requires; the open questions OQ-29 and
  OQ-30 in [open-questions.md](../open-questions.md). The companion simulation service is in
  [libroadrunner.md](libroadrunner.md).
- **Sources:** facts from upstream documentation were checked on 2026-11-11 against libAntimony
  3.3.0 and are cited as [A1] to [A17]. Facts about libAntimony 3.3.0's source, its build and the
  libraries it was built against were checked on 2026-11-17, when the provider was built, and are
  cited as [A18] to [A20]; a fact of [A1] to [A17] that the build checked again is cited with them,
  and its row under [Sources](#sources) says so. A claim that could not be checked is marked
  "(unverified)".

## What this document holds

It began as the feasibility design of a conversion service backed by libAntimony and compiled into
the runtime. The service is now specified and built, as a service provider, so this document keeps
what the specifications do not hold: the options that were compared, facts about libAntimony 3.3.0
and the libraries it links (each with where it was checked), what the build corrected in the first
version of this document, the alignment with `ode-uq-spec`, platforms and licensing, future work,
and the risks and checks that remain.

What the specifications own, and where to find it:

- The service table, its options, report, errors, size limits and order of work:
  [`model_conversion.h`](../../include/praenora/sdk/model_conversion.h) and
  [model-conversion-service.md](../specs/model-conversion-service.md).
- What the provider `antimony` converts, its rules for each format, modules, engine failures,
  warnings, its lock and the sequence of calls under it, and the process state it changes:
  [model-conversion-service.md](../specs/model-conversion-service.md#the-antimony-provider).
- Why a provider, why one of its own, and the calls made while building it:
  [ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md).
- Providers, their discovery, loading and lifetime, and the engine in the run fingerprint:
  [runtime-services.md](../specs/runtime-services.md) and
  [ADR-0026](../adr/ADR-0026-runtime-service-providers.md).
- Building libAntimony and the provider: [platforms.md](../platforms.md#the-antimony-provider) and
  [developer-guide.md](../developer-guide.md#the-antimony-provider).
- The test provider `test-conversion` and the service checks:
  [model-conversion-service.md](../specs/model-conversion-service.md#the-test-provider-test-conversion).

## Options compared

| Option | Summary | Outcome |
|---|---|---|
| C1. Compiled into the runtime | `PRAENORA_WITH_ANTIMONY=ON` compiles the service into `praenora_runtime`, which registers it when a `Runtime` is constructed. | Recommended by this document's first version; rejected ([ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md), following [ADR-0026](../adr/ADR-0026-runtime-service-providers.md)): every runtime built with the option would carry and load libAntimony and libSBML, also when no run converts, and the run fingerprint would not record the engine. |
| C2. Each module bundles libAntimony | Possible: nothing stops a module from linking it. | Not supported: every such module must apply the rules itself, and two modules that load the same shared libAntimony share one copy of its global state, because the dynamic loader returns the same handle for the same library [A14], and no lock covers both. |
| C3. Out-of-process conversion | A helper process that owns libAntimony. | Not needed yet. It would isolate libAntimony's global state, could run without file system access and could stop a translation (OQ-5, OQ-30), at the cost of a process and of copying every source and text. |
| C4. libAntimony compiled to WebAssembly, run by an embedded engine | A converter inside the engine of [webassembly.md](webassembly.md), Model B, with no file system imports. | Future work ([WebAssembly](#webassembly)): the sandbox would leave no file to open. |
| C5. No conversion | Modules accept SBML only. | Rejected: `ode-uq-spec` allows Antimony model sources, so a module that reads them would bundle a translator anyway (C2). |
| C6. A service provider of its own | A separately built library, shipped with the runtime, that the runtime loads only when a run needs its service: one engine per process. | Chosen and built ([ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md)): the provider `antimony`. |
| C7. Inside the provider `roadrunner` | One provider offers both services, with one libSBML. | Rejected ([ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md)): conversion would need libRoadRunner and LLVM, one engine object would describe two engines, and a change to either would change the fingerprint of runs that use only the other. A process that loads both providers holds two copies of libSBML, which is accepted. |

## libAntimony 3.3.0: checked facts

Building and testing the provider on macOS arm64, with libAntimony 3.3.0 built from its source at
the tag `v3.3.0` [A18][A19] against the libraries that the release binaries of libRoadRunner 2.11.0
hold [A20], and reading that source [A18], settled the following on 2026-11-17. A fact that one of
the provider's tests pins names the test.

### Build and link

- **Built from source.** No archive of the C library for C or C++ consumers is among the release's
  assets, which are Python wheels, Julia archives and a macOS debug archive [A2], and PyPI has
  Python wheels only [A3], so the provider's libAntimony was built from the tag `v3.3.0` (commit
  `f13a50bce91ff2d16ec4b6c5179a8d43edff9f9b`), Release, with `ANT_DEPENDENCIES_INSTALL_PREFIX`
  (where its build finds libSBML, SBMLNetwork and expat), with `WITH_QTANTIMONY`, `WITH_PYTHON`,
  `WITH_CELLML`, `WITH_GTEST` and `WITH_JULIA` off, and with position-independent code, so that its
  static library links into a `MODULE` library [A1][A19];
  [platforms.md](../platforms.md#the-antimony-provider) has the commands. libAntimony's own build
  printed ten compiler warnings (`-Wswitch`) and six linker warnings in the build checked; the
  provider's build has none.
- **The installation** holds `include/antimony_api.h` with the headers it includes, the static
  library `libantimony-static.a` and a shared library, and no CMake package configuration [A19].
  The provider's build finds libAntimony, and SBMLNetwork, which has no package either, by header
  and library on `CMAKE_PREFIX_PATH`, and libSBML by its package `sbml-static`
  ([`PraenoraAntimony.cmake`](../../cmake/PraenoraAntimony.cmake)).
- **Dependencies from libRoadRunner.** libAntimony requires libSBML and SBMLNetwork, and its README
  points to `libroadrunner-deps` for them and recommends expat [A1]; libRoadRunner itself links a
  static libSBML of at least 5.20.5 [A16]. The release binaries of libRoadRunner 2.11.0 hold all of
  them: libSBML 5.21.2 as a static library with the packages comp, fbc, distrib, layout and render
  among others, SBMLNetwork 0.5.12 as a static library, expat 2.9.0, zlib and bzip2, with CMake
  packages for all but SBMLNetwork [A20]. That libSBML links neither zlib nor bzip2 (`nm -u`), so
  the provider's build makes them available without requiring them.
- **A probe at configuration.** The provider's build links libAntimony, SBMLNetwork and libSBML
  into a small program and runs it when the build is configured. The program prints the versions
  the three libraries report: libAntimony's `getVersionStr()` (`v3.3.0`, with a leading `v`),
  libSBML's `getLibSBMLDottedVersion()` (5.21.2) and SBMLNetwork's `getLibraryVersion()` (0.5.12, a
  function in the global namespace), and whether libSBML has the package comp. That it links and
  runs shows that the three go together, and the versions become the provider's engine object. A
  libAntimony other than 3.3.x is refused, so that a new lexer is read before it is used
  ([Risks](#risks)).
- **Behind an export list.** The provider library links all of them statically and PRIVATE and
  exports only `praenora_provider_get_api` (the test `providers.antimony.exports`); SBMLNetwork's
  global `getLibraryVersion` and libAntimony's lexer stay inside it. It is a `.so` bundle of about
  12 MB in a Debug build, about 8 MB stripped, that needs only libSystem and libc++.
- **macOS 14.** The release binaries of libRoadRunner 2.11.0 are built for macOS 14.0 [A20], so
  libAntimony is built against them for 14.0, and a build with the provider needs a deployment
  target of at least 14.0 instead of the project default of 11.0.
- **Windows.** libAntimony's header declares its functions for import from a DLL
  (`__declspec(dllimport)`) on Windows unless its own build defines `LIB_EXPORTS` [A19], so linking
  the static library there may need a definition that the provider's build does not give
  (unverified).
- **Linux.** SBMLNetwork needs `libdl`, which the provider's build adds (not built on Linux).

### The C API

The functions the provider calls, and the upstream facts their use rests on. Where no other source
is cited, a fact is from the header [A4], which the 3.3.0 build generates from its template [A6] and
installs [A19]:

- **Loading.** `loadAntimonyString` and `loadSBMLString` return the index of the stored set, or -1
  with the error saved and nothing stored. `loadString`, `loadFile` and the file loaders read any
  format the library knows, CellML included when it is built with it, and take file names absolute
  or relative to the directory the program runs from; the provider never calls them.
- **Antimony text is read as SBML first.** `loadAntimonyString` hands its text to the registry's
  `OpenString`, which first parses it with libSBML's `readSBMLFromString`; a text that parses as
  SBML is loaded as SBML, reading the documents that its comp external model definitions name, and
  then refused as not Antimony [A18]; their `source` may be a URL, a URN or a file location [A13].
  So Antimony text that is an SBML document can read files without an `import`, although its load
  fails; the rule `xml` refuses it before libAntimony sees it.
- **Modules.** Every load has the module `__main`, for statements outside every module, besides the
  modules the source defines; `getMainModuleName()`, `getNumModules()` and `getNthModuleName()` name
  them (`AntimonyProvider.MainModuleFollowsTheRule`). An empty Antimony source loads as an empty
  `__main`, as the build showed; the rule `empty` refuses it.
- **Writing.** `getSBMLString(module)` writes the module flattened: the elements of its submodules
  renamed and placed in one model. `getAntimonyString(module)` writes Antimony, and returns NULL on
  failure, with an error set. `getCompSBMLString(module)` writes SBML with the package comp,
  keeping the module's hierarchy; the provider does not offer it ([Future work](#future-work)).
- **Settings.** `setRemoveFunctionDefinitions(bool)` says whether loading SBML removes function
  definitions (`true` by default), and `setWriteSBMLTimestamp(bool)` whether written SBML holds a
  time stamp [A19] (`false` by default in 3.3.0 [A18]). `clearPreviousLoads()` clears the loaded
  files and leaves these settings alone [A18], so the provider sets them once, when it starts.
- **Results of the process.** `getLastError()` and `getWarnings()` take no handle: they return the
  last error and the warnings of the process.
- **Directories and `.antimony` files.** `addDirectory` and `clearDirectories` set the directories
  searched for imported files. A `.antimony` file, of rules about where imports are found, is read
  only while an `import` is resolved, and directories are added only by opening a file and by
  `addDirectory` [A18].
- **Memory.** Returned strings are allocated with `malloc` and owned by the caller; `freeAll()`
  frees every pointer the library handed out, and calling it after freeing one of them yourself
  crashes the program; the library returns NULL when it runs out of memory for a returned pointer.
  The provider copies each result at once and calls `freeAll()` under its lock, never `free()`,
  which also avoids freeing across C runtimes on Windows [A12].

### Global state, the locale and time

- **Global state.** libAntimony's registry is the global `g_registry`, which holds the loaded files,
  the directories and the imports of a parse, with no mutex [A5][A18]. libSBML keeps process-wide
  state too: its package extension registry is a singleton, documented with nothing about threads
  [A11], and its writer keeps a process-wide setting. One process-wide lock in the provider covers
  every call into either.
- **The locale.** `loadAntimonyString`, `getAntimonyString` and other functions set the process's
  locale to `C` (`setlocale(LC_ALL, "C")`) and restore it before they return [A18]; libSBML
  switches it too [A20]. `setlocale` is process-wide, and the provider's lock does not cover other
  code: the host application, a module or another provider that formats or parses numbers through
  the C locale while a conversion runs may see `C`, and switches that overlap on different threads
  can leave the process in `C`. The runtime does not depend on the locale.
- **The C library's random numbers.** SBMLNetwork's automatic layout
  (`libsbmlnetwork_autolayout.cpp.o` in its static library [A20]) calls `srand(time(NULL))` and
  then `rand()`, and its Fruchterman-Reingold placement calls `rand()` too. libAntimony runs it for
  a module that asks for a layout, when it checks the module after loading (which builds its SBML)
  and again when it writes the SBML [A18]. Such a conversion reseeds `rand()` for the whole process,
  and its layout changes from one second to the next (Layouts, under Determinism).
- **Output.** On the paths the provider uses, libAntimony and libSBML write nothing to standard
  output or standard error: libAntimony's writes there are commented out [A18], and nothing reached
  standard error in the conversions checked.
- **Time.** The header names no function that interrupts a translation. Its time grows faster than
  the source: a generated chain of 500 reactions translated to SBML in about 0.3 s, of 700 reactions
  (42 KB) in about 0.55 s and of 1000 (60 KB) in about 0.8 s on the machine where this was measured,
  and 700 took about 1.3 s in a ThreadSanitizer build of the tests. How long one translation may
  hold the provider's lock is OQ-30.

### Reading Antimony

- **Imports open files.** An Antimony source imports another file with `import "file"`, SBML
  files included, resolved against the working file's directory or configured directories, and
  `.antimony` files can redirect imports [A7]. An `import` of a file that does not exist fails with
  libAntimony's error `Could not open` [A18]; the provider's tests show that no source they convert
  reaches that error (`AntimonyProvider.ImportOfAMissingFileIsTheServicesRefusal`,
  `AntimonyProvider.CommentedAndQuotedImportsReachLibAntimonyHarmlessly`).
- **The lexer** of libAntimony 3.3 (`antimony.ypp`) [A18]: `//` and `#` start a line comment that
  ends before the next CR or LF; `/*` starts a block comment that ends after the next `*/`; `"`
  starts a string, in which a backslash takes the next character, CR and LF included, and which a
  line end without a backslash interrupts, after which the string's content is read again as code;
  one backtick is a character, two an empty string, three open a block that ends after the next
  three, and four are one token. A number followed by letters is two tokens, so `1import "f"`
  imports. Keywords are compared with their letter case (`Import` is no keyword), and words are
  ASCII letters, digits and `_`. The provider's check of imports reads sources as this lexer does,
  and refuses more than it would, never less
  ([model-conversion-service.md](../specs/model-conversion-service.md#the-antimony-provider)); its
  tests convert 196 sources that hold an import of a missing file inside comments, strings and
  blocks, and none opens it
  (`AntimonyProvider.CommentedAndQuotedImportsReachLibAntimonyHarmlessly`).
- **Line numbers.** libAntimony's error names a line (`Error in model string, line <n>:`), but its
  count skips the line ends inside block comments and triple-backtick blocks and counts a run of
  continued lines as one [A18], so after such a block it is not the source's line. The provider's
  libAntimony errors therefore have no `details.line`
  (`AntimonyProvider.EngineFailureHasNoLineAfterMultiLineBlocks`).
- **Byte order mark and CR LF.** libAntimony refuses a UTF-8 byte order mark, and keeps the CR of a
  CR LF inside a block that spans lines; the provider hands it the text without the mark and with
  LF line ends (`AntimonyProvider.BomAndCrLfSourcesConvertLikeTheirPlainForms`).
- **The main module** is the module marked `*` (the last of them, with a warning, when several
  are), else `__main` when statements stand outside every module, else the last module defined:
  the rule of `ode-uq-spec` ([Alignment with ode-uq-spec](#alignment-with-ode-uq-spec)), in its
  three cases (`AntimonyProvider.MainModuleFollowsTheRule`).
- **Flattening.** The elements of submodules become elements of the module's model, their ids
  prefixed with the submodule's name and `_` (`A_J0` for reaction `J0` of submodule `A`).

### Reading and writing SBML

- **Function definitions.** By default `loadSBMLString` removes function definitions [A4][A19],
  replacing each call by the function's body. With `setRemoveFunctionDefinitions(false)` they are
  kept, and SBML translated to Antimony keeps them as functions; the provider sets it and says so in
  its engine object, `"function_definitions": "kept"`
  (`AntimonyProvider.FunctionDefinitionsAreKept`).
- **What `getSBMLString` writes.** SBML Level 3 Version 2, flattened, beginning with the comment
  `<!-- Created by libAntimony version v3.3.0 with libSBML version 5.21.2. -->`, with no time
  stamp, libAntimony 3.3.0's default, which the provider sets again when it starts
  (`setWriteSBMLTimestamp(false)`; libSBML keeps the setting for the whole process) [A18]. A source
  that asks for a layout (`model.layout = on`, the autolayout settings, an element's position or
  style, a default style) gets SBMLNetwork's layout and render, in the packages layout and render,
  which the document does not require; the provider refuses that SBML (below).
- **Distributions.** libAntimony writes the package distrib, with `required="true"`, into the SBML
  of a model that uses a distribution [A18]; the provider's rule against required packages then
  refuses that SBML on the way back (`AntimonyProvider.DistributionsDoNotConvertBack`).
- **libSBML 5.21.2** [A20] reports a comp declaration whose `required` is `false` as an error of its
  own, which is why the provider's rule `comp` comes before libSBML's errors. It counts columns from
  0, and reads a document that does not start with an XML declaration as if it began with one. An
  entity reference without a declaration is an error, and it interprets neither XInclude elements
  nor style-sheet processing instructions: none of them made it read a file that exists
  (`AntimonyProvider.SbmlNeverOpensReferencedFiles`). Documents of Level 2 Version 4 and Level 3
  Versions 1 and 2 translate to Antimony.

### Determinism

- The same conversion gives the same bytes on repeated calls
  (`Antimony/ModelConversionService.RepeatedCallsAreIdentical/Antimony`) and in two fresh processes
  (`providers.antimony.processes`), on one machine, except for a layout.
- **Layouts.** The SBML of an Antimony source that asks for a layout differs from one second to the
  next: SBMLNetwork seeds its random positions from the wall clock (the same four-reaction source
  gave 30086 and 30091 bytes, with other positions, in two processes 5 s apart). libAntimony runs
  the layout already while it loads the source, and its C API shows nothing of the request before
  it writes, so the provider reads the SBML it wrote with libSBML and refuses it when it declares
  layout or render (`UNSUPPORTED`, `details.reason` `"layout"`;
  `AntimonyProvider.LayoutRequestsAreRefused`, which straddles a second of the wall clock). SBML
  that holds a layout converts to the same Antimony in different seconds: libAntimony writes the
  positions it read, and the layout it runs while loading does not reach the text
  (`AntimonyProvider.LaidOutSbmlConvertsTheSameAcrossSeconds`).
- Antimony to SBML, back to Antimony and to SBML again gives the first SBML once the source declares
  its elements in the order libAntimony writes them; from another order, the first round can differ
  from later ones (`AntimonyProvider.RoundTripIsAFixpoint`).

## What the build corrected

The first version of this document (sources read on 2026-11-11) was a design. Building the provider
showed otherwise in these points:

| The first version said | The build showed |
|---|---|
| Compile the service into `praenora_runtime` (option C1), registered in its `ServiceRegistry`; a service registered there would not be in the run fingerprint. | The service comes from the provider `antimony` (option C6, [ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md)), as [ADR-0026](../adr/ADR-0026-runtime-service-providers.md) requires of heavy engines; its engine object enters the fingerprint through the provider mechanism. |
| A libSBML in the runtime and the one in the provider `roadrunner` would neither collide nor share state (unverified with both built). | Each provider links a copy of its own, from the one libSBML package; both load in one process, and a model converted by one simulates in the other (`Api.AntimonyModelSimulatesThroughRoadrunner`). |
| libAntimony needs libSBML and SBMLNetwork; whether its installation has a CMake package was unverified, and a find module would be used. | The release binaries of libRoadRunner 2.11.0 hold libSBML, SBMLNetwork and expat [A20]. libAntimony and SBMLNetwork install no CMake package, so the build finds them by header and library, and a probe at configuration proves that the three libraries link together and gives their versions for the engine object. |
| No C library archive is visible among the release's assets. | None was found among the assets; the provider's libAntimony is built from source. |
| Build for the project's deployment target of 11.0 (unverified). | The dependencies are built for macOS 14, so libAntimony and the provider are too. |
| That `getMainModuleName()` applies the `ode-uq-spec` rule in every case was unverified. | It does, in the rule's three cases. |
| `loadSBMLString()` removes function definitions by default, and the report would say so when a source has any. | The provider keeps them (`setRemoveFunctionDefinitions(false)`), and its engine object says so. |
| Byte-identical output was expected (unverified). | Verified on one machine, on repeated calls and in two processes, except for layouts: SBMLNetwork seeds an automatic layout from the wall clock and reseeds the C library's `rand()`, so the provider refuses Antimony that asks for a layout; SBML that holds one converts to the same Antimony in different seconds. |
| The import check removes comments (`//`, `#`, `/* */`), double-quoted strings and triple-backtick blocks, then refuses the word `import`. | It must read as libAntimony 3.3's lexer does: a backslash escapes in a string, a line end leaves a string unterminated (refused, rule `unterminated`), a CR ends a line comment, four backticks are code, and `import` right after a number is an import. A scan that ended strings at their next quote would let `x is "a\" // "; import "f"` through, and one that took digits for letters `M.sboTerm = 1import "f"`; libAntimony opens `f` for both. |
| An Antimony source reaches other files through `import`, and `.antimony` files can redirect imports. | It can without an `import` too: `loadAntimonyString` reads its text as SBML first, and SBML with comp external model definitions reads the documents they name, so Antimony text that is an XML document is refused (rule `xml`). |
| A document that declares a required package is refused, which refuses comp's external model definitions. | Every declaration of comp is refused, required or not, before libSBML's errors, since libSBML reports comp with `required="false"` as an error; the rule `package` applies to Level 3, where packages exist. |
| libAntimony's message goes into `details.engine_error`. | It does, as libAntimony wrote it, but the line it names is not the source's after a block comment or a triple-backtick block that spans lines, so libAntimony's errors carry no `details.line`. libSBML's errors do, once its columns from 0 and the declaration line it adds are mapped back to the source. |
| Not considered. | libAntimony switches the process's locale to `C` while it works. |
| Not considered. | A model that uses a distribution converts to SBML but not back: libAntimony writes distrib as required. |
| Not considered. | An empty Antimony source translates into an empty module `__main`: every format refuses an empty source (rule `empty`). |
| Translations are expected to be short (unverified), and the size limit bounds how long the lock is held. | Translation time grows faster than the source: the size limit bounds the source, not the time (OQ-30). |

The draft service table, the conversion sequence and the error mapping of the first version are
replaced by [model-conversion-service.md](../specs/model-conversion-service.md), which differs from
them: `describe` lists pairs of formats (`conversions`) rather than formats, a pair the provider
does not convert is `UNSUPPORTED` rather than `INVALID_ARGUMENT`, and the report holds `from`,
`to`, `module` and `warnings`, while the engine is in `describe` and the run fingerprint.

## Alignment with ode-uq-spec

`ode-uq-spec` 0.0.0 treats model sources as data, not code, and states rules for them
(`conventions.md`, Security, and `specification.md`, Model specification): XML document type
declarations are forbidden, since they enable entity-expansion attacks; an Antimony source is
self-contained, without `import`, and is checked for `import` before it is translated, because
libAntimony opens imported files; an SBML source must not require a Level 3 package, which excludes
comp; and an Antimony source means the flattened SBML of its main module, the module marked `*`,
else `__main` when the file has top-level statements, else the last module defined. The provider
applies each: the rules `import`, `unterminated` and `xml` for Antimony, `document_type`, `comp`
and `package` for SBML, and flattened SBML of the main module, which libAntimony chooses by the
same rule
([model-conversion-service.md](../specs/model-conversion-service.md#the-antimony-provider)).

The first version described the check of imports as the one of `ode-uq-spec`'s reference validator
(`tools/validate.py`). The provider's check refuses more. The validator reads strings with
backslash escapes and ends them at a line end, so it refuses the first of the two forms under
[What the build corrected](#what-the-build-corrected); it looks for `import` between word
boundaries, and a digit and a letter have none between them, so `M.sboTerm = 1import "f"` passes
it, and libAntimony then opens `f`.

## Platforms

| Platform | Project presets | Upstream libAntimony 3.3.0 binaries [A3] | Notes |
|---|---|---|---|
| macOS arm64 | `macos-arm64-*` | `macosx_14_0_arm64` Python wheel | Built and tested ([platforms.md](../platforms.md#the-antimony-provider)): libAntimony from source against the release binaries of libRoadRunner 2.11.0, for macOS 14.0. |
| Linux x86-64 | `linux-x64-*` | `manylinux_2_28_x86_64` Python wheel | Not built. The provider's export list is a version script there, and SBMLNetwork needs `libdl`. |
| Windows x86-64 | `windows-x64-*` | `win_amd64` Python wheel | Not built. libAntimony builds with MSVC x64 [A1]; its header declares its functions for import from a DLL ([Build and link](#build-and-link)). Keep the shared MSVC runtime; the provider frees libAntimony's memory only through `freeAll()`. |
| macOS x86-64 | none | `macosx_15_0_x86_64` Python wheel | Not a project platform. |
| Linux arm64, Windows arm64 | none | none listed | Building from source should be possible (unverified). |

The wheels hold the Python bindings, not a C library to link against. How the engine is built and
distributed is OQ-29.

### Licensing facts

These are facts about upstream projects, gathered for the license decision (OQ-1) and for
distribution (OQ-13, OQ-29). This project states no license of its own.

| Component | License as stated upstream |
|---|---|
| libAntimony | BSD-3-Clause on its repository page [A1]; MIT in the PyPI metadata of 3.3.0 [A3]. The discrepancy should be settled upstream before redistribution. |
| libSBML | LGPL 2.1 [A10] |
| SBMLNetwork, expat | Not checked (unverified) |

The provider links libSBML statically. A distributed binary that contains a statically linked
LGPL-2.1 library must, among other conditions, give users the means to relink it with a modified
library, or use a shared-library mechanism instead (section 6) [A15]. The provider `roadrunner`
links the same libSBML release, so one legal review can cover both (OQ-27, OQ-29).

## Future work

Nothing in this section is specified or built.

### WebAssembly

- **The runtime compiled to WebAssembly** ([webassembly.md](webassembly.md), Model A).
  `libantimonyjs` builds expat, a static libSBML (with expat, without libxml2) and libAntimony
  (without CellML) with Emscripten's CMake and make wrappers, and links them into a single-file
  JavaScript and WebAssembly module with memory growth, exporting C functions such as `_loadString`,
  `_getSBMLString` and `_freeAll` [A8]. MakeSBML, a client-side web application that converts
  between Antimony and SBML [A9], loads that `libantimony.js` module [A17]. A WebAssembly runtime
  would need the provider's code compiled into it, and the design of Model A, older than service
  providers, registers modules statically and loads no provider; whether `libantimonyjs` builds
  libAntimony 3.3, which requires SBMLNetwork [A1], was not checked (unverified). The rules still
  apply there: an in-memory file system holds whatever the embedder wrote into it.
- **WebAssembly modules in a native runtime** (Model B). The provider stays native; WebAssembly
  modules would reach the service through host imports that mirror the table of
  `model_conversion.h`. Running libAntimony itself as a sandboxed WebAssembly module (option C4)
  would remove file access altogether, and leave the rules as a second defense.

### Hierarchical SBML

`getCompSBMLString` writes a module with its submodules kept as comp submodels [A19]. The provider
writes flattened SBML only, and refuses comp in SBML it reads, since comp's external model
definitions name other documents. Hierarchical output would be a format of its own among the
conversions that `describe` lists, which a provider may add at any time
([model-conversion-service.md](../specs/model-conversion-service.md#evolution)); reading such SBML
back would need a rule for external model definitions.

### CellML

libAntimony reads and writes CellML when it is built with libCellML [A4][A19]; the provider's
libAntimony is built without it (`WITH_CELLML=OFF`), so its loaders skip CellML. A format
`cellml` would need libCellML in the build and rules for CellML's own imports, which name other
documents (unverified how libAntimony resolves them).

### Other

- The simulation service reading Antimony itself: today a module converts the source first and
  simulates the SBML.
- A conversion command in the `praenora` tool or the Python package.
- Providers that use the services of other providers.
- Pinned downloads of libAntimony (FetchContent): its dependency chain (libSBML, SBMLNetwork, expat)
  would need pins of its own ([ADR-0017](../adr/ADR-0017-dependency-management.md)).
- An engine out of process (option C3; OQ-5, OQ-30).

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| An `import`, or a reference to another document, slips past the rules | libAntimony or libSBML opens a file chosen by the source | The check of imports reads as libAntimony 3.3's lexer and refuses unterminated forms; the rules `xml`, `document_type` and `comp`; no file name is passed and `clearDirectories()` runs on every call; the tests sweep imports of a missing file through every comment, string and block form; the build accepts libAntimony 3.3.x only, so a new lexer is read before it is used; later, option C4. |
| Another copy of libAntimony in the process | Races between the provider and a module that links libAntimony | The provider's copy is static and hidden behind its export list; module authors use the service. |
| libAntimony's switch of the process's locale | Code on another thread that formats or parses through the C locale sees `C` while a conversion runs | Stated in the specification; the runtime does not depend on the locale; the provider cannot prevent it. |
| Long translations under one lock | Calls wait; deadlines pass while they wait | A cancellable wait; conversions in `create` or `initialize`; OQ-30. |
| Linux and Windows builds are not done | The provider exists on macOS arm64 only | Build and test there (OQ-29). |
| License discrepancy (BSD-3-Clause or MIT), a statically linked LGPL-2.1 libSBML, SBMLNetwork and expat unchecked | Distribution blocked or obligations unmet | Ask upstream; a license review with the simulation service's (OQ-1, OQ-13, OQ-27, OQ-29). |
| Models with distributions | They convert to SBML only | Stated in the specification and tested; exempting distrib from the rule `package` would be a decision of its own. |

## Remaining checks

The checks that the first version planned were carried out by the provider's build and tests on
macOS arm64: linking (into a provider rather than the runtime), round trips, the check of imports,
the SBML rules, eight threads converting at once under ThreadSanitizer, determinism in one process
and across two, the three cases of the main module, and both providers in one process. What
remains:

1. **Other platforms.** Build the provider on Linux x86-64 and Windows x86-64 and run its tests.
   *Exit:* the same tests pass, and the Windows import declaration and the Linux export list are
   confirmed or corrected here (OQ-29).
2. **Determinism elsewhere.** Run `providers.antimony.processes` on a second machine and on the
   other platforms. *Exit:* identical bytes, or the differences recorded.
3. **License review.** *Exit:* a written answer on libAntimony's license, on linking libSBML, and on
   SBMLNetwork and expat, recorded in an ADR before the provider is distributed (OQ-29).
4. **A new libAntimony.** Before the build accepts a version other than 3.3.x, read its lexer and
   its registry again against the check of imports and the rule `xml`. *Exit:* the provider's tests
   pass, and the facts above are dated again.
5. **Translation time.** Measure translation time against source size with real models. *Exit:* an
   answer to OQ-30.
6. **`ode-uq-spec`'s validator.** Report to that specification that its check of imports lets
   `import` right after a number through (`1import "f"`), which libAntimony 3.3 reads as an import.
   *Exit:* the report made, or the validator refuses that form.
7. **WebAssembly.** Build libAntimony and the provider's code with Emscripten, with the flags of
   `libantimonyjs`. *Exit:* the provider's conversion tests pass under Node.

## Sources

Pages A1 to A17 were read on 2026-11-11; A18 to A20 were checked on 2026-11-17, when the provider
was built, and the rows of A1, A4, A5, A6 and A7 say what was checked again then. The
`ode-uq-spec` 0.0.0 rules cited by name above were read in that specification's own repository
(`docs/conventions.md`, `docs/specification.md` and the reference validator `tools/validate.py`,
whose check of imports was read again on 2026-11-17); no public URL is cited for it.

| Id | Source | Supports |
|---|---|---|
| A1 | [sys-bio/antimony, repository page](https://github.com/sys-bio/antimony) | A modular, human-readable model definition language with a C/C++ library and Python and Julia bindings; BSD-3-Clause; libSBML and SBMLNetwork required, expat recommended; CMake options; optional Python, Qt, Julia and GoogleTest; Windows, macOS and Linux builds. Checked again on 2026-11-17 by the build [A19]: libSBML and SBMLNetwork are required, and the options `ANT_DEPENDENCIES_INSTALL_PREFIX`, `WITH_QTANTIMONY`, `WITH_PYTHON`, `WITH_GTEST` and `WITH_JULIA` exist. |
| A2 | [sys-bio/antimony, releases](https://github.com/sys-bio/antimony/releases) | v3.3.0 is the latest release (Julia bindings; zip and bzip support in libSBML); its visible assets are zipped Python wheels, Julia archives and a macOS debug archive, with no C library archive. |
| A3 | [antimony on PyPI](https://pypi.org/project/antimony/#files) | 3.3.0 of 2026-10-28; wheels for four platforms, 2.7 to 5.6 MB, no source distribution; license metadata "MIT". |
| A4 | [antimony `src/antimony_api.h.in`](https://raw.githubusercontent.com/sys-bio/antimony/HEAD/src/antimony_api.h.in) | The C API: loaders and their return values, translation functions, handle-less errors and warnings, `clearPreviousLoads`, `addDirectory`, `clearDirectories`, `.antimony` files, `freeAll` and its warning, `malloc`-owned results, `getVersionStr`, file names relative to the working directory, no function that disables imports. Checked again on 2026-11-17 in the header that the 3.3.0 build installed [A19]. |
| A5 | [antimony `src/registry.h`](https://raw.githubusercontent.com/sys-bio/antimony/HEAD/src/registry.h) | `extern Registry g_registry;` with file, directory and import state; no mutex or thread declarations. Checked again on 2026-11-17 in the 3.3.0 source [A18]. |
| A6 | [antimony `src/` directory](https://github.com/sys-bio/antimony/tree/HEAD/src) | The API header is the template `antimony_api.h.in`; `registry.h` and `registry.cpp` hold the registry. Checked again on 2026-11-17: the 3.3.0 build generates and installs `antimony_api.h` from it [A19]. |
| A7 | [Antimony Reference](https://tellurium.readthedocs.io/en/latest/antimony.html) | `import "file"` syntax; SBML files can be imported; relative names resolved against the working file's directory; other directories configurable; `.antimony` files can redirect imports; bare declarations belong to `__main`. Checked again on 2026-11-17: an import of a missing file fails with `Could not open` in the 3.3.0 source [A18], and statements outside every module make `__main` the main module (`AntimonyProvider.MainModuleFollowsTheRule`). |
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
| A18 | [libAntimony source at the tag v3.3.0](https://github.com/sys-bio/antimony/tree/v3.3.0) (commit `f13a50bce91ff2d16ec4b6c5179a8d43edff9f9b`) | `antimony.ypp` (the lexer: comments, strings and their escapes, backticks, numbers before words, the count of lines), `registry.cpp` (`OpenString` reading SBML first, `CheckAndAddSBMLIfGood` and `LoadSubmodelsFrom`, `OpenFile` and its `Could not open` error, `ClearAll`, `.antimony` files and search directories, no time stamp by default), `antimony_api.cpp` (the loaders and writers, `loadAntimonyString` refusing a text that is SBML, the time stamp setting passed to libSBML before each write, `setlocale`), `module-sbml.cpp` (distrib written as required; `CreateSBMLModel` calling SBMLNetwork's `autorender` and `autolayout` for a module that asks for a layout, and `LoadLayout` reading a layout from SBML), `module.cpp` (its writes to standard output commented out; `Finalize` building the module's SBML, and so its layout, while a source loads; the layout statements that ask for one). |
| A19 | libAntimony 3.3.0 built from [A18] (Release; the options in [platforms.md](../platforms.md#the-antimony-provider)) and its installation | The installed `antimony_api.h` (the loaders, `setRemoveFunctionDefinitions`, `setWriteSBMLTimestamp`, `getCompSBMLString`, `getVersionStr`, CellML only with libCellML, `LIB_EXTERN` declaring `dllimport` on Windows), `libantimony-static.a` and a shared library, no CMake package; the library as the provider links and tests it. |
| A20 | `roadrunner-2.11.0-macos-14-arm64-Release.zip` of the [release v2.11.0 of sys-bio/roadrunner](https://github.com/sys-bio/roadrunner/releases/tag/v2.11.0), unpacked (the same archive as in [libroadrunner.md](libroadrunner.md#sources)) | libSBML 5.21.2 (static, with comp, distrib, layout and render), SBMLNetwork 0.5.12 (static, without a CMake package), expat 2.9.0, zlib and bzip2, built for macOS 14.0; libSBML as the provider uses it (comp with `required="false"` an error, columns from 0, the declaration line it adds, `setlocale` in `libsbml-static.a`); SBMLNetwork's `libsbmlnetwork_autolayout.cpp.o` referencing `srand`, `time` and `rand`. |
