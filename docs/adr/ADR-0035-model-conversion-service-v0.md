# ADR-0035: Model conversion service v0 and the libAntimony provider

- Status: Proposed
- Date: 2026-11-17
- Supersedes: none (amends [ADR-0021](ADR-0021-runtime-services-through-host-api.md))
- Superseded by: none

## Context

[ADR-0021](ADR-0021-runtime-services-through-host-api.md) planned a model conversion service,
`praenora.model-conversion@0`, that translates Antimony to SBML and back with libAntimony, and the
feasibility document [libantimony.md](../feasibility/libantimony.md) recommended compiling it into
the runtime library (its option C1). [ADR-0026](ADR-0026-runtime-service-providers.md) has since
decided where heavy engines live: in service providers, separately built libraries that the runtime
loads on demand, one engine per process, reached by modules only through `get_service`, with the
engine's identity in the run fingerprint. The simulation service
([ADR-0027](ADR-0027-simulation-service-v0.md)) and its provider `roadrunner` follow that model.

Modules need a conversion service because models come in more than one format: `ode-uq-spec`
allows Antimony model sources, while the simulation service's `roadrunner` provider reads SBML. A
module that bundled a translator itself would have to apply the safety rules on its own, and two
modules that load the same shared libAntimony would share its process-global state without a lock
that covers both.

libAntimony 3.3.0 keeps its state in process-global variables (the loaded models, the last error,
the warnings, the directories searched for imports), opens the files that an Antimony `import`
names, translates an empty source into an empty module `__main`, and links libSBML and SBMLNetwork,
which libRoadRunner's dependencies also provide.

## Decision

- **The service.** `praenora.model-conversion@0`, version 0.0, is a public C99 table declared in
  [`include/praenora/sdk/model_conversion.h`](../../include/praenora/sdk/model_conversion.h),
  installed with the module SDK (0.2.0, unchanged), and specified in
  [model-conversion-service.md](../specs/model-conversion-service.md). It follows the conventions of
  the module ABI and of the simulation service: fixed-width integers, pointers, length-delimited
  UTF-8 and module-owned sinks; no exception crosses it.
- **One call, no objects.** `describe` writes the service's identity: the engine object of the
  provider's manifest and the pairs of formats it converts. `convert` translates one self-contained
  source: the options name `from`, `to` and optionally the `module` to translate; the source travels
  as raw text, not inside JSON, so a large SBML document is not escaped; the translated text goes to
  one sink and a canonical report `{"from", "to", "module", "warnings"}` to another.
- **Rules before the translation.** A source is checked before the engine translates it: size
  limits (source 16 MiB, options 1 MiB) before anything is parsed, valid UTF-8 without NUL bytes,
  the rule `empty` in every format (white space only, after an optional byte order mark: never an
  empty model), then the rules of its format, each named in `details.rule`. Engine failures are
  `DATA_INVALID` with `details.engine_error`, an unknown module `NOT_FOUND`. The order of these
  checks is fixed, so the same input always gets the same error, and every text the provider writes
  is valid UTF-8.
- **Threading.** `describe` and `convert` may be called concurrently from any thread. A provider
  whose engine keeps process-global state serializes its translations behind one process-wide lock;
  waiting for it is cancellable (polled at least every 50 ms), a translation in progress is not.
- **Reproducibility.** A translation is a function of the source, the options and the engine
  identity; the run fingerprint records the provider and its engine object through the existing
  provider mechanism ([runtime-services.md](../specs/runtime-services.md#fingerprint)), which this
  decision does not change.
- **The libAntimony provider is a provider.** libAntimony is offered by a separate provider,
  `antimony`, built only with the build option `PRAENORA_WITH_ANTIMONY` (OFF by default), never
  fetched, linking libAntimony, libSBML, SBMLNetwork and expat statically and PRIVATE behind an
  export list that holds only the provider entry point. This amends ADR-0021's plan and the
  feasibility document's option C1, as ADR-0026 requires. It converts Antimony to SBML (flattened,
  Level 3 Version 2) and SBML to Antimony (function definitions kept, which its engine object
  states), and serializes every use of libAntimony, and its own use of libSBML, behind one
  process-wide lock.
- **The provider's rules.** An Antimony source is refused when it is an XML document (`xml`:
  libAntimony would read it as SBML first), when it imports another file (`import`, found by
  reading comments, strings and blocks as libAntimony 3.3's lexer does) or when it leaves a
  comment, a string or a block open (`unterminated`). An SBML source is refused when it is not a
  document's text (`not_document`), holds a document type declaration (`document_type`), declares
  the package comp (`comp`) or, at Level 3, requires a package (`package`); libSBML's errors are
  engine failures, located in the source. So no source makes the engine read a file or the
  network.
- **The engine is built from source.** No archive of libAntimony's C library for C or C++
  consumers was found among its release's assets. The provider is built against libAntimony 3.3
  built from source against the libSBML (with comp), SBMLNetwork and expat of libRoadRunner's
  dependencies: the release binaries of libRoadRunner 2.11.0, or a build of libroadrunner-deps. A
  probe at configuration links the three libraries and reports their versions, which make the
  provider's engine object; the build accepts libAntimony 3.3.x only. How the engine is built and
  distributed on each platform is open (OQ-29).
- **Checks and a reference provider.** A parameterized suite of service checks tests any provider
  of the service against the specification; the fixture provider `test-conversion` implements every
  rule for two toy formats, so the service, the checks and modules that use it run in every build.

### Builder notes (Proposed)

- **Header.** `model_conversion.h` defines the service's name, major and minor,
  `PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE` (the v0.0 size, frozen like
  `PRAENORA_HOST_API_V0_BASE_SIZE`) and the two size limits as `size_t` macros, and its comment
  summarizes the rules. It is compiled as C99 into the unit tests (`ModelConversionAbi.*`) and
  included by the install test's consumer module.
- **White space.** The rule `empty` counts space, tab, line feed, vertical tab, form feed and
  carriage return as white space (the six ASCII white-space characters), after one optional UTF-8
  byte order mark.
- **Options.** Empty option bytes are not a JSON object (`INVALID_ARGUMENT` at `""`): `from` and
  `to` are required, unlike the simulation service's session options, where empty bytes count as
  `{}`. Problems of the document's shape come first (not an object, unknown fields in the order of
  their names, `from`, `to`, `module`), then `UNSUPPORTED` at `/from`, then at `/to`; the
  specification makes this order normative, so every provider reports the same error for the same
  options. Both `UNSUPPORTED` errors carry `details.format` and `details.conversions`.
- **Order of the rules.** Rules that read the source's text alone run before the cancellation poll
  and the provider's lock. Rules that need the engine's parser (an XML parser, for example) run
  under the lock, before the translation, in a fixed order; a source that parser cannot read is an
  engine failure. A source a rule refuses never reaches the translation.
- **Details.** A source that is not UTF-8 or holds a NUL byte also gets `details.offset`, the first
  byte not allowed (one scan reports the first problem in byte order). `details.pointer` may point
  into a source of a JSON format, as for `test-conversion`'s `UNSUPPORTED` at `/events`. Size errors
  carry `details.size` and `details.limit`.
- **Valid UTF-8.** Error documents and reports are written with nlohmann/json's replacing error
  handler, so text that is not valid UTF-8 becomes U+FFFD.
- **Sinks.** `out_text` is written before `report_json`; a call ended by a `report_json` that
  refuses its write has written the text. An error document that `error_json` refuses leaves the
  call's status as it is.
- **The test provider.** `test-conversion` writes numbers as canonical JSON writes a double
  (nlohmann/json: the shortest digits that read back as the same double, `.0` after an integral
  value), in both formats; numbers in `test-decay-text` are JSON numbers. The rule `import` refuses
  a line whose first word, after its comment is removed and its leading spaces and tabs skipped, is
  `import`. A `hold` takes milliseconds from 0 to 10000 without leading zeros, and a source's hold
  lines add up. An empty `events` array is accepted; a model with events is `UNSUPPORTED` once it is
  otherwise valid. An unknown module is `NOT_FOUND` after the source is read, as an engine would
  find it, so a malformed source is `DATA_INVALID` whatever its `module`. Any failure of
  nlohmann/json to read a `test-decay` source is an engine failure, a number too large for a double
  included; only a syntax error has a byte offset, and so a `details.line`. Waiting for its lock
  polls every 20 ms.
- **Service checks.** A parameter set (`ModelConversionCase`) names its manifest, engine object,
  conversions, exact translations (`ConversionExpectation`), refusals (`ConversionRefusal`, with an
  optional pointer), an engine failure with an optional line, an optional source with several
  modules and an optional source that holds the lock. A check sees the lock from outside: it counts
  the polls of a conversion that nothing makes wait, then converts with a host that reports a
  cancellation from the next poll on, which only a call that has to wait for the lock reaches. A
  successful conversion with a full host must poll at least once and log at least once.
- **The fixture module.** `converting-module` declares the test catalog's capability
  `test.echo@0.1.0`, as `simulating-module` does: its message type takes any JSON object. It
  computes the SHA-256 of the converted text itself (the SDK has no digest), looks the simulation
  service up in `create` only when its parameter `simulate` is given, passes `module` to the
  service whenever the parameter is given (an empty one reaches the service, which refuses it), and
  quotes the service's message in its own.
- **Provider files.** `praenora_add_provider_file_tests()` takes the print tool's target as an
  argument, and the tool `praenora_conversion_print` converts a source and its result back,
  printing both texts and reports verbatim. `test-conversion` has the tests
  `providers.test-conversion.exports` and `providers.test-conversion.processes`. The driver of the
  latter, `run_twice.cmake`, reused, takes the tool's name (`TOOL`, by default
  `praenora_simulation_print`), which names the tool in its reports and says what its output must
  show: a simulation and a batch, or a conversion and the conversion back.
- **The antimony provider's build.** `cmake/PraenoraAntimony.cmake` finds libAntimony and
  SBMLNetwork, which have no CMake package, by their headers and static libraries, and makes them
  the imported targets `praenora::libantimony` and `praenora::sbmlnetwork`. It loads libSBML's
  package first, so that expat, zlib and bzip2 are searched beside its package files; zlib and
  bzip2 are made available, not required, since the libSBML of libRoadRunner's dependencies links
  neither. A probe built and run at configuration (`try_run`) links the three libraries and prints
  their versions, which make the engine object. What this finder and roadrunner's share, the
  message for a missing dependency, a status line printed once per configuration (libSBML, which
  both find, is reported once) and the macOS repair of link interfaces, is in
  `cmake/PraenoraEngineDependencies.cmake`; roadrunner's configuration prints what it printed
  before. A libAntimony other than 3.3.x stops the configuration, and one other than 3.3.0, the
  version checked, gets a status line that says so.
- **The check of imports mirrors libAntimony's lexer.** libAntimony 3.3 reads a backslash in a
  string as taking the next character, ends a line comment at a CR alone, reads a string that a
  line end interrupts again as code, reads four backticks as one token, and reads a number and a
  word that follows it without a space as two tokens. A scan that ended strings at their first
  quote would let `x is "a\" // "; import "f"` through, and one that took digits for word
  characters `M.sboTerm = 1import "f"`; libAntimony opens `f` for both. So the check reads strings
  with libAntimony's escapes and ends them at a line end as unterminated, ends line comments at CR
  or LF, takes four backticks for code, and refuses `import` in a run that starts with a digit.
  Its unit tests run in every build; the provider tests show through libAntimony that what it lets
  pass never makes libAntimony open a file.
- **Rule `xml`.** libAntimony's `loadAntimonyString` first reads its text as SBML, and loads an SBML
  document with comp external model definitions by reading the documents they name (a test read
  one so, outside the provider). An Antimony source whose first character other than white space
  is `<` is therefore refused by the rule `xml`, before the rules of imports.
- **The order of the SBML rules under the lock.** `comp` comes before libSBML's errors, then
  `package`: libSBML 5.21.2 reports a comp declaration whose `required` is `false` as an error
  itself, and every document that declares comp is refused by its rule. A comp namespace declared
  below the `sbml` element is not interpreted, and converts.
- **What the engine reads.** libAntimony reads an Antimony source without its byte order mark and
  with CR LF replaced by LF: it reads neither a byte order mark, nor a CR LF inside a block that
  spans lines as a line end. libSBML and libAntimony read an SBML source from its first `<`, after
  a line `<?xml version="1.0" encoding="UTF-8"?>` when it does not start with `<?xml version=`,
  which libSBML would add itself; libSBML's lines and its columns, which count from 0, are given
  as the source's, from 1.
- **Details.** The refusals `comp` and `package` name the namespace in `details.package`; an
  unknown module also lists the source's modules in `details.modules`. A libAntimony failure has
  no `details.line`: the line libAntimony's message names (`Error in model string, line <n>:`)
  skips the line ends inside block comments and triple-backtick blocks and counts a run of
  continued lines as one, so it is not the source's line after such a block; the message stays in
  `engine_error.message` as libAntimony wrote it. libSBML's failures keep `details.line` and
  `details.column`, which locate. The report's warnings are libAntimony's (`getWarnings()`), not
  libSBML's.
- **Start.** `start()` sets libAntimony's configuration under the provider's lock and fails
  (`INTERNAL`) when the libraries it links report other versions than the engine object.
- **One wait, two locks.** The wait for the lock, a poll before it and one every 20 ms, is the
  header `src/providers/common/cancellable_lock.hpp`, which `antimony` and `test-conversion` both
  use; each library has a lock of its own. The antimony parameter set of the service checks holds
  its lock with a generated chain of 700 reactions, which libAntimony translates in about 0.55 s
  (about 1.3 s in a ThreadSanitizer build); the two checks that use it take about 0.8 s and 1.8 s
  in a Debug build, 1.7 s and 4.0 s under ThreadSanitizer.
- **Expected outputs.** The antimony provider's expected outputs
  (`tests/fixtures/models/antimony/`) were written once from its own conversions (the one of a
  submodule chosen by `module` with a scratch program that configures libAntimony as the provider
  does) and checked by hand; they are fixtures that no test rewrites, and the tests show that each
  equals the provider's output byte for byte. The rules model declares its elements in the order
  libAntimony writes them, so that Antimony to SBML, back and to SBML again gives the first SBML.
- **Process state.** libAntimony sets the process's locale to `C` while it reads and writes, and
  restores it, and libSBML switches it too; other code in the process (the host application,
  modules, other providers) may see `C`, and overlapping switches can leave the process in `C`.
  SBMLNetwork's automatic layout calls `srand(time(NULL))` and `rand()`, which reseeds the C
  library's `rand()` for the whole process. The specification says both, since a provider cannot
  prevent them.
- **Layouts are refused.** SBMLNetwork 0.5.12 places the elements of an automatic layout at random
  positions seeded from the wall clock, so the SBML of an Antimony source that asks for a layout
  (`model.layout = on`, the autolayout settings, an element's position or style, a default style)
  changes from one second to the next. libAntimony runs that layout when it loads such a source
  (its check of the module builds the SBML) and when it writes the SBML, and its C API tells
  nothing about the request before: a check of the text would have to know every layout statement
  and tell `S1.x` from a submodule's variable. So the provider reads the SBML libAntimony wrote with
  libSBML, under its lock, and refuses it (`UNSUPPORTED`, `details.reason` `"layout"`) when it
  declares the package layout or render; the refused request has already reseeded `rand()`. SBML
  that holds a layout converts to Antimony with its positions, the same bytes in different seconds
  (a test straddles a second of the wall clock); loading it runs the layout once, whose positions
  do not reach the text, and reseeds `rand()`.
- **Distributions do not convert back.** libAntimony writes the package `distrib` with
  `required="true"` into the SBML of a model that uses a distribution. The rule `package` refuses
  every Level 3 document that requires a package, as the roadrunner provider does for SBML it
  simulates, so that SBML is refused when it is converted back to Antimony; `distrib` is not
  exempted. The specification states it, and `AntimonyProvider.DistributionsDoNotConvertBack`
  pins it.
- **Tests that see the providers of a build.** With the antimony provider, the build tree's
  provider directory is not empty, so `cli.providers.list_empty` is registered only in a build
  with neither provider, as with roadrunner. The install test checks each installed provider, and
  its driver receives the providers as a list: escaped, as they were, the two names of a build with
  both providers arrived as one, a defect that only a second provider showed.
- **Open questions.** How libAntimony is built and distributed is OQ-29, and how long one
  translation may hold the provider's lock is OQ-30. Until OQ-30 is answered, the provider keeps
  the service's 16 MiB source limit and no limit of its own.
- Later: [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) answers OQ-30 for a step
  in a module host: the provider is loaded and started in the step's module host, with a
  libAntimony and a lock of its own, and a translation that outlives its call's deadline and the
  cancellation grace is ended by killing that module host, so no lock is left held and the
  conversions of other steps, each in a module host of its own, never wait for it. In the
  runtime's process the lock is held as above.

## Alternatives considered

- **A separate provider (chosen).** libAntimony and its libSBML live in their own library, loaded
  only by runs that need the service. A process that loads both `antimony` and `roadrunner` holds
  two copies of libSBML, each private to its provider behind hidden symbols; that is accepted.
- **Inside the `roadrunner` provider.** One provider would offer both services with one libSBML.
  But conversion would then need libRoadRunner and LLVM to be built and loaded, one engine object
  would describe two unrelated engines, and a change to either library would change the
  fingerprint of runs that use only the other.
- **Compiled into the runtime library** (the feasibility document's option C1, ADR-0021's plan).
  Every runtime built with the option would carry libAntimony and libSBML, loaded by every process
  whether or not it converts, which is what ADR-0026 moved engines out of the runtime to avoid.
- **The source inside the options document.** A single JSON document would be simpler to pass, but
  a large SBML document would have to be escaped into a JSON string and parsed back; the raw text
  costs one more argument.

## Consequences

- Modules convert Antimony and SBML without linking libAntimony, through the same provider
  mechanism, search paths and fingerprint as the simulation service; a module that simulates an
  Antimony model converts it first, then opens the SBML text with the simulation service.
- The rules are applied in one place, the provider, rather than in every module.
- The check of imports reads as libAntimony 3.3's lexer does, so it refuses more than a simpler
  scan would: a backslash escapes in a string, a line end leaves a string unterminated, and
  `import` right after a number is an import. Antimony text that starts with `<` is refused
  (`xml`), and every declaration of comp is refused before libSBML's errors. Some unusual but valid
  sources are therefore refused; what the check lets pass never made libAntimony open a file in the
  provider's tests. A libAntimony other than 3.3.x needs its lexer and registry read again before
  the build accepts it.
- One lock serializes every translation in a process: a long translation delays the others, and a
  translation in progress cannot be stopped. The size limit bounds the source, not the time:
  libAntimony's translation time grows faster than the source, and how long one translation may
  hold the lock is open (OQ-30). Modules convert in `create` or `initialize`.
- libAntimony sets the process's locale to `C` while it reads and writes, and restores it, and
  libSBML switches it too. Other code in the process (the host application, modules, other
  providers) may see `C` at that moment, and overlapping switches can leave the process in `C`; the
  runtime does not depend on the locale, and the provider cannot prevent it.
- Antimony that asks for a layout does not convert to SBML (its layout would depend on the wall
  clock), and such a request, or SBML with a layout converted to Antimony, reseeds the C library's
  `rand()`: modules draw random numbers from generators of their own.
- libAntimony's errors carry its message as it wrote it, but no `details.line`, since its count of
  lines is not the source's after a block that spans lines; libSBML's errors are located in the
  source.
- A model that uses a distribution converts from Antimony to SBML but not back.
- A process that uses both providers holds two copies of libSBML, each private to its provider.
  Both providers load into one process: a model converted by one is simulated by the other
  (`Api.AntimonyModelSimulatesThroughRoadrunner`).
- The run fingerprint changes with the versions of libAntimony, libSBML and SBMLNetwork, which the
  build reads from the libraries themselves, and with the setting for function definitions.
- Building the provider needs libAntimony built from source and, on macOS, a deployment target of
  at least 14.0, the dependencies' own; it is verified locally on macOS arm64 and by CI on every
  platform key against the dependency bundles, and distributing it needs a license review (OQ-29).
- Open: hierarchical (`comp`) SBML output, CellML, a WebAssembly build, the simulation service
  reading Antimony itself, and a conversion command in the tool or the Python package
  ([feasibility/libantimony.md](../feasibility/libantimony.md#future-work)).

## References

- [model-conversion-service.md](../specs/model-conversion-service.md): the service, normative.
- [ADR-0021](ADR-0021-runtime-services-through-host-api.md): runtime services through the host API.
- [ADR-0026](ADR-0026-runtime-service-providers.md) and
  [runtime-services.md](../specs/runtime-services.md): service providers.
- [ADR-0027](ADR-0027-simulation-service-v0.md) and
  [simulation-service.md](../specs/simulation-service.md): the simulation service, whose
  conventions this service shares.
- [feasibility/libantimony.md](../feasibility/libantimony.md): upstream facts about libAntimony
  3.3.0, what the build corrected in the first design, and future work.
- [platforms.md](../platforms.md#the-antimony-provider): building libAntimony and the provider.
- [open-questions.md](../open-questions.md): OQ-29 (building and distributing libAntimony) and
  OQ-30 (how long one translation may hold the lock).
