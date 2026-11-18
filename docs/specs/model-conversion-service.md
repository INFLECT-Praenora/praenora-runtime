# Model conversion service `praenora.model-conversion@0`

- **Version:** service `praenora.model-conversion` 0.0 (`PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR` 0,
  `PRAENORA_MODEL_CONVERSION_SERVICE_MINOR` 0), declared in
  [`model_conversion.h`](../../include/praenora/sdk/model_conversion.h) of the module SDK 0.2.0
- **Status:** Normative. Proposed ([ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md)).
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The model conversion service lets a module translate a model source from one format to another
with an engine that the runtime owns: for example an Antimony model to SBML, which the simulation
service ([simulation-service.md](simulation-service.md)) can then simulate, or SBML back to
Antimony. It is a runtime service offered by service providers
([runtime-services.md](runtime-services.md)): a module declares that it requires it, the runtime
finds and loads a provider that offers it, and the module calls the provider's engine through the
C table this document specifies. The module links no engine and may be built with any compiler.

One call, `convert`, translates one self-contained model source: the module passes the source's
text and names its format and the format to write; the provider writes the translated text and a
report. The service has no objects: nothing is opened, and nothing needs closing.

In this repository the provider `antimony` offers the service for Antimony and SBML, with
libAntimony ([The antimony provider](#the-antimony-provider)), when the build has it, and the
fixture provider `test-conversion` offers it for two toy formats
([The test provider](#the-test-provider-test-conversion)). Every provider of the service follows
this document, and the service checks ([Service checks](#service-checks)) test that it does.

## Getting the service

A module declares the service in its manifest
([module-developer-guide.md](../module-developer-guide.md#requiring-services)):

```cmake
praenora_add_module(converting_module NAME converting-module VERSION 0.1.0
    CAPABILITIES example.estimate@0.1.0 SOURCES src/module.cpp
    REQUIRES_SERVICES praenora.model-conversion@0.0)
```

and gets the table from its host API, usually once, in `create`:

```c
const praenora_model_conversion_v0* conversion =
    (const praenora_model_conversion_v0*)host->get_service(
        host->context, PRAENORA_MODEL_CONVERSION_SERVICE, 25,
        PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR);
```

The service's name, `praenora.model-conversion`, is 25 bytes long. A module MUST use the table only
when it is not NULL, `header.abi_major` is 0, `header.reserved` is 0 and `header.struct_size` is at
least `PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE`. `header.abi_minor` is the minor the provider
implements: a later minor only appends functions to the table (and fields to the documents, see
[Evolution](#evolution)), and a module uses an appended function only when `struct_size` covers it.
The table lives until the process ends.

## The table

The header is authoritative; this is its declaration, without comments:

```c
#define PRAENORA_MODEL_CONVERSION_SERVICE "praenora.model-conversion"
#define PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR 0u
#define PRAENORA_MODEL_CONVERSION_SERVICE_MINOR 0u
#define PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE ((size_t)16 * 1024 * 1024)
#define PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE ((size_t)1024 * 1024)

typedef struct praenora_model_conversion_v0 {
    praenora_api_header header;
    void* context;
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);
    praenora_status (PRAENORA_CALL *convert)(void* context, const praenora_host_api* host,
        praenora_bytes options_json, praenora_bytes source, praenora_sink* out_text,
        praenora_sink* report_json, praenora_sink* error_json);
} praenora_model_conversion_v0;
```

The header also defines `PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE`, the size of the v0.0 table up to
the end of `convert`, frozen at that layout as `PRAENORA_HOST_API_V0_BASE_SIZE` is. It is C99,
depends only on `abi.h`, `status.h`, `<stddef.h>` and `<stdint.h>`, and is installed with the SDK.
Every function of the table is required; `context` is the provider's, passed to both functions.

## Conventions

The conventions of the module ABI apply ([module-abi.md](module-abi.md#conventions)): only
fixed-width integers, `size_t`, pointers, length-delimited UTF-8 and sinks cross the table, and no
exception or `longjmp` leaves a function of the provider.

- **Borrowed inputs.** `praenora_bytes` are borrowed for the call; the provider copies what it
  keeps. Their data MAY be NULL only when their size is 0; otherwise the call returns
  `INVALID_ARGUMENT`.
- **Sinks** belong to the module. The provider writes to them only while the call runs. Every sink
  argument is required: a NULL sink, or one whose `write` is NULL, makes the call return
  `INVALID_ARGUMENT` (and when that sink is `error_json`, nothing is written). When a sink refuses a
  write, the call stops and returns the status `write` returned, after writing an error document to
  `error_json` when the refusing sink is another one. An error document that `error_json` refuses
  leaves the call's status as it is.
- **Documents** the provider writes are canonical JSON: compact, object keys sorted
  ([reproducibility.md](reproducibility.md#canonical-json)). The options it reads are a strict JSON
  object.
- **Text** that the provider writes in a document, its own messages and the engine's included, is
  valid UTF-8: a sequence that is not, such as the part of a multi-byte character that an engine's
  error message quotes, is replaced by U+FFFD. So every document is valid JSON.
- **Statuses** are `PRAENORA_STATUS_OK` and the general codes 1 to 12 of
  [`status.h`](../../include/praenora/sdk/status.h), except that a call ended by a refusing sink
  returns the status that sink's `write` returned, whatever its value.
- **Error documents.** A call that returns a status other than OK writes one document of the
  `pmp-error` shape to `error_json`: `{"code": <the status>, "message": <text>, "details": {...}}`.
  `details` holds `"service": "praenora.model-conversion"` and, where they apply:
  - `pointer`: the JSON pointer of the problem in the options (`""` for the whole document), or in
    a source of a JSON format;
  - `format` and `conversions`: with `UNSUPPORTED` at `/from` or `/to`, the format that the options
    name and the provider does not read or write, and `describe`'s `conversions`;
  - `reason`: `"utf8"` or `"nul"` for a source that is not valid UTF-8 or holds a NUL byte, with
    `offset`, the byte where it is not; with `UNSUPPORTED` for a source, what the provider does not
    translate, as its section states (the `antimony` provider: `"layout"`);
  - `rule`: the name of the format rule that refused the source;
  - `engine_error`: `{"type", "message"}` of a failure the engine raised;
  - `line` and `column` (from 1): where the engine locates a failure, or where a rule finds what it
    refuses, when the provider's section says so;
  - `module`: a module the source does not have;
  - `size` and `limit`: the size of an input over its limit, and the limit;
  - members that a provider's section names (the `antimony` provider's `package` and `modules`).

## Threading

- `describe` and `convert` MAY be called from any thread at any time, concurrently; a provider MUST
  be free of data races.
- A provider MAY serialize its translations behind one process-wide lock; a provider whose engine
  keeps process-global state MUST. A call waits for the lock, polling for cancellation while it
  waits ([Host, logging and cancellation](#host-logging-and-cancellation)); a translation in
  progress is not interrupted.
- The service hands out no object, so a module has nothing to close before its `destroy` returns.

## Host, logging and cancellation

- `host` is the calling instance's host API ([module-abi.md](module-abi.md#host-api)), valid for the
  call; it MAY be NULL, which means no logging and no cancellation. The provider checks that
  `host->struct_size` covers a callback before it uses it, and never keeps `host` after the call.
- Records that a call can attribute to itself go through `host->log`, so they carry the step's
  attributes: a call logs at debug level the conversion it made and the sizes of its source and
  text. What the engine emits through its own process-wide logging, which cannot tell the calling
  instance apart, goes to the provider's own logger
  ([runtime-services.md](runtime-services.md#loading)), or is silenced; it never goes to `host`.
- A call polls `host->cancel_requested()` once before the translation starts, before it takes the
  provider's lock, and, while it waits for the lock, at least every 50 ms. When it reports nonzero,
  the call stops and returns `CANCELLED`. A translation in progress is not interrupted; one longer
  than the call's deadline ends as the runtime's timeout rules say
  ([time-cancellation-timeouts.md](time-cancellation-timeouts.md)).
- A conversion may wait for others, which the provider serializes: modules SHOULD convert in
  `create` or `initialize` (lifecycle deadline) rather than in `invoke`.

## describe

`describe(context, out_json)` writes the service's identity. For `test-conversion`:

```json
{"conversions":[{"from":"test-decay","to":"test-decay-text"},{"from":"test-decay-text","to":"test-decay"}],"engine":{"name":"test-conversion-engine","version":"1.0.0"},"service":"praenora.model-conversion","version":{"major":0,"minor":0}}
```

- `service` is `"praenora.model-conversion"`; `version` is `{"major": 0, "minor":
  <header.abi_minor>}`.
- `engine` is the `engine` object of the provider's manifest, which the run fingerprint records
  ([runtime-services.md](runtime-services.md#fingerprint)).
- `conversions` lists the pairs of formats `convert` translates, each `{"from", "to"}`. It is not
  empty, holds no pair twice, and is sorted by `from`, then by `to`, comparing UTF-8 bytes.
- It is thread-safe and free of side effects, and writes the same bytes every time.

## convert

`convert(context, host, options_json, source, out_text, report_json, error_json)` translates the
model source `source` as the options say. On success it writes the translated text to `out_text`,
then the report to `report_json`, and returns OK. A call that fails writes neither, except that a
call ended by a `report_json` that refuses its write has written the text.

### Options

```json
{"from": "test-decay-text", "to": "test-decay", "module": "main"}
```

| Field | Required | Meaning |
|---|---|---|
| `from` | yes | The format of the source: a `from` of `describe`'s conversions. |
| `to` | yes | The format to write: a `to` that `describe`'s conversions pair with `from`. |
| `module` | no | A non-empty string: the name of the module of the source to translate. Left out: the source's main module. |

Bytes that are not a JSON object (empty bytes included) are `INVALID_ARGUMENT` at `""`; an unknown
field, a missing `from` or `to`, a field that is not a string and an empty `module` are
`INVALID_ARGUMENT` at their pointer. A `from` the provider does not read is `UNSUPPORTED` at
`/from`, and a `to` it cannot write from that `from` (the same format included, unless `describe`
lists that pair) is `UNSUPPORTED` at `/to`; both errors name that format in `details.format` and
list `describe`'s `conversions` in `details.conversions`. Format names are compared exactly. When
the options have several problems, a provider MUST report the first in this order: not a JSON
object; an unknown field, the first in the order of their names (UTF-8 bytes); `from`; `to`;
`module`; then `UNSUPPORTED` at `/from`; then `UNSUPPORTED` at `/to`.

### Source

- The source is the model's text in the format `from`, UTF-8 and not NUL-terminated. It is the model
  itself, never a path or a URI: a provider MUST NOT read files or the network on its behalf, also
  not files or documents the source names.
- A source that is not valid UTF-8 (overlong forms, surrogates and code points above U+10FFFF
  included) is `INVALID_ARGUMENT` with `details.reason` `"utf8"`, and one that holds a NUL byte is
  `INVALID_ARGUMENT` with `details.reason` `"nul"`; `details.offset` is the first byte that is not
  allowed.
- A UTF-8 byte order mark at the start and CR LF line ends are accepted in every format whose engine
  accepts them; what a provider does with them is stated for each of its formats.

### Rules

A provider applies rules to a source before its engine translates it: a source a rule refuses is
`DATA_INVALID`, with `details.rule` naming the rule, and never reaches the translation. Rules that
read the source's text alone run before the call polls for cancellation and waits for the
provider's lock. Rules that need the engine's parser (an XML parser, for example) run after the
lock is taken, when the provider has one, before the translation; a source that parser cannot read
is an engine failure
([Engine failures](#engine-failures)). A provider applies its rules in a fixed order, which it
states for each of its formats.

- **Empty sources**, rule `empty`, in every format: a source that is empty, or that holds only
  white space (space, tab, line feed, vertical tab, form feed and carriage return) after an optional
  UTF-8 byte order mark, is refused. An engine could otherwise translate it into an empty model.
- **The rules of a format** keep the engine from reading what lies outside the source (an Antimony
  `import`, for example) or from reading a source whose meaning is not certain. A provider states
  its rules and their names for each of its formats in its section of this document.

### Modules

`module` names the module to translate; without it, the provider translates the source's main
module, as the engine defines it. A module the source does not have is `NOT_FOUND`, with
`details.module`. A format whose sources hold one module says what it is called.

### Engine failures

A source the engine cannot read or translate is `DATA_INVALID`, with `details.engine_error`
`{"type", "message"}` (the kind of failure and the engine's message) and, where the engine locates
the problem, `details.line` and `details.column`.

### Report

The report is canonical JSON:

```json
{"from":"test-decay-text","module":"main","to":"test-decay","warnings":["hold 0 ignored in output"]}
```

`from` and `to` are the options'; `module` is the module translated; `warnings` lists the engine's
warnings about this translation, one entry for each non-empty line, in order, and is `[]` when there
are none.

### Order of work

A call works in this order, so that the same input always gets the same error:

1. the arguments: the sinks and the bytes (`INVALID_ARGUMENT`);
2. the size limits, before anything is parsed: the options, then the source
   ([Size limits](#size-limits));
3. the options;
4. the source's text: UTF-8, no NUL byte;
5. the rules that read the text alone: `empty`, then those of the format `from`, in a fixed
   order;
6. cancellation: `cancel_requested()`;
7. the provider's lock, when it has one, polling `cancel_requested()` while it waits;
8. the rules that need the engine's parser, in a fixed order;
9. the translation: reading the source, the module, writing the text, the warnings; the outputs
   are copied, then the lock is released;
10. `out_text`, then `report_json`.

## Size limits

| What | Limit | Macro | Exceeded |
|---|---|---|---|
| The options | 1 MiB (1,048,576 bytes) | `PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE` | `RESOURCE_EXHAUSTED`, before anything is parsed |
| The source | 16 MiB (16,777,216 bytes) | `PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE` | `RESOURCE_EXHAUSTED`, before anything is parsed |

Both errors have `details.size` and `details.limit`; options over their limit are reported before a
source over its limit. A provider MAY also return `RESOURCE_EXHAUSTED` when it runs out of memory,
or when its engine returns nothing for lack of memory.

## Errors

| Condition | Status |
|---|---|
| A NULL sink or sink `write`; bytes whose data is NULL with a size | `INVALID_ARGUMENT` |
| Options that are not a JSON object; an unknown field; a missing or wrong-typed field; an empty `module` | `INVALID_ARGUMENT`, `details.pointer` |
| A source that is not valid UTF-8, or that holds a NUL byte | `INVALID_ARGUMENT`, `details.reason` |
| A format the provider does not read, or a pair it does not convert | `UNSUPPORTED`, `details.pointer`, `details.format` |
| A source the provider does not translate, for a reason its section states (the `antimony` provider: a layout) | `UNSUPPORTED`, `details.reason` |
| Options or a source over its size limit; out of memory | `RESOURCE_EXHAUSTED` |
| A source a rule refuses | `DATA_INVALID`, `details.rule` |
| A source the engine cannot read or translate | `DATA_INVALID`, `details.engine_error` |
| A module the source does not have | `NOT_FOUND`, `details.module` |
| `cancel_requested()` reported nonzero | `CANCELLED` |
| A sink refused a write | the status `write` returned |
| Any other failure (an unexpected exception inside the provider) | `INTERNAL` |

## Reproducibility

- A translation is a function of the source's bytes, the options and the engine identity only: for
  the same engine identity, the same source and options give the same text and report, byte for
  byte, in one process and across processes, on one platform with one build
  ([reproducibility.md](reproducibility.md)), and the same input gets the same error.
- A run's fingerprint records each service it resolved, with its provider and the provider's engine
  object, which `describe` reports as `engine`
  ([runtime-services.md](runtime-services.md#fingerprint)).
- A module SHOULD record what it converted (the report, or a digest of the text) where its results
  are reported, in its outputs or diagnostics.

## Evolution

A later minor of the service MAY append functions to the table, add optional option fields and add
fields to the report and to `describe`'s document that a module can ignore; a provider MAY offer
more conversions at any time. It never changes the meaning of what exists. A provider of a later
minor still refuses what it does not know, so a module that sends a field a provider does not know
gets `INVALID_ARGUMENT`, never a silent default. A breaking change is a new major, with a new table.

## The antimony provider

The provider `antimony` (`src/providers/antimony/`) offers `praenora.model-conversion@0.0` for
Antimony and SBML, with libAntimony 3.3 as its engine. It is built only with the build option
`PRAENORA_WITH_ANTIMONY` (OFF by default;
[platforms.md](../platforms.md#the-antimony-provider)), links libAntimony, SBMLNetwork, libSBML and
expat statically and exports only the provider entry point. Its version is the runtime's. In the
build tree it sits where the build tree's runtime looks for the providers of its installation, and
an installation holds it in `<prefix>/lib/praenora/providers/antimony/`. This section states what
the rules above leave to a provider ([ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md)).

**Identity.** `describe` writes, for the build checked on macOS arm64:

```json
{"conversions":[{"from":"antimony","to":"sbml"},{"from":"sbml","to":"antimony"}],"engine":{"function_definitions":"kept","libsbml":"5.21.2","name":"libantimony","sbmlnetwork":"0.5.12","version":"3.3.0"},"service":"praenora.model-conversion","version":{"major":0,"minor":0}}
```

The engine object is the manifest's: the versions that libAntimony (`getVersionStr()`, without its
leading `v`), libSBML (`getLibSBMLDottedVersion()`) and SBMLNetwork (`getLibraryVersion()`) report
in a probe that the build links and runs when it is configured, and `"function_definitions":
"kept"`, the setting below. A provider built against other versions has another engine object, and
so another run fingerprint.

**Start.** Once per process, under its lock, the provider sets libAntimony to keep function
definitions when it reads SBML (`setRemoveFunctionDefinitions(false)`; by default libAntimony
replaces each call by the function's body and drops the definition) and to write no time stamp into
SBML (`setWriteSBMLTimestamp(false)`), and clears the directories libAntimony searches for imported
files and what it has loaded (`clearDirectories()`, `clearPreviousLoads()`). It never changes this
configuration again. It then checks that the libraries it links report the versions of its engine
object; a difference fails the start (`INTERNAL`), with an error document that names both. It logs
`antimony started: libAntimony 3.3.0, libSBML 5.21.2, SBMLNetwork 0.5.12` to its own logger.

**Conversions.**

- `antimony` to `sbml`: the module as libAntimony writes it (`getSBMLString`): SBML Level 3
  Version 2, flattened, so that the elements of the module's submodules are elements of its model,
  their ids prefixed with the submodule's name and `_` (`A_J0` for reaction `J0` of submodule `A`).
  It begins with the comment `<!-- Created by libAntimony version v3.3.0 with libSBML version
  5.21.2. -->`. A source that asks for a layout is refused (Layouts, below).
- `sbml` to `antimony`: the module as libAntimony writes it (`getAntimonyString`), with function
  definitions kept. It begins with the line `// Created by libAntimony v3.3.0`, followed by
  libAntimony's warnings as comments when there are any.

**Format `antimony`.** The source is Antimony text. libAntimony reads it without its byte order
mark and with each CR LF replaced by a LF (libAntimony itself refuses a byte order mark, and keeps
the CR of a CR LF inside a block that spans lines, such as a note), so a source with them converts
exactly as the same source without them. Its rules: `xml` first; then the check of imports, which
reads the source from its start and refuses with the first of `import` or `unterminated` that it
meets (an `import` before an unclosed `/*` is `import`; a string that a line end leaves open before
an `import` is `unterminated`):

- `xml`: a source whose first character other than white space, after an optional byte order mark,
  is `<` is an XML document. libAntimony would read it as SBML first, outside the rules of format
  `sbml` (it would read the documents that the source's comp external model definitions name);
  such a source is converted from `sbml`.
- `import`: libAntimony opens the file that an `import` names. A source with the word `import`, in
  any case of letters, in code is refused. The check finds comments, strings and blocks as the
  lexer of libAntimony 3.3 does, so that it never takes for a comment, a string or a block what
  libAntimony reads as code:
  - `//` and `#` start a line comment, which ends before the next CR or LF (libAntimony ends a line
    comment at a CR alone too);
  - `/*` starts a block comment, which ends after the next `*/`;
  - `"` starts a string, in which a backslash takes the next character, whatever it is, into the
    string; the string ends at the next `"` without a backslash before it;
  - three backticks start a block, which ends after the next three backticks; four backticks are a
    token of code, two an empty string and one a character.

  Outside these, `import` is code when it is a word: a run of letters, digits and `_` that starts
  with a letter or `_` and is `import`; or, in a run of letters, digits, `_` and `.` that starts
  with a digit (a number, which libAntimony may end before the letters that follow it, as in
  `1import`), `import` followed by a character other than a letter, a digit and `_`, or by the end
  of the source. `importance`, `k_import` and `import2` are words of their own.
- `unterminated`: a block comment, a string or a block that the source does not close. A string is
  closed on its own line only: a CR or a LF without a backslash before it ends the string
  unterminated (libAntimony then reads its content again as code). What libAntimony does with these
  is not assumed.

**Format `sbml`.** The source is an SBML document (checked with Level 2 Version 4 and Level 3
Versions 1 and 2). libSBML and libAntimony read the document from its first `<`, without the byte
order mark and the white space before it, after a first line `<?xml version="1.0"
encoding="UTF-8"?>` when the document does not start with `<?xml version=` (libSBML would add the
same line itself). CR LF line ends are the XML parser's, which reads them as LF. So a document with
a byte order mark, CR LF line ends or white space before it converts exactly as the same document
without them. Its rules that read the text alone, in this order:

- `not_document`: a source whose first character other than white space, after an optional byte
  order mark, is not `<`: the source is the document's text, never a path or a URI to it;
- `document_type`: a source that holds a document type declaration (`<!DOCTYPE`, in any case of
  letters, anywhere), which can declare entities that name files; refused before any XML parser
  reads the text.

The rules that need libSBML's parser run under the provider's lock: libSBML reads the document
(`readSBMLFromString`), and then, in this order:

- `comp`: a document whose `sbml` element declares the package comp (a namespace whose URI holds
  `/comp/`, as each version of comp's does), required or not. Its external model definitions name
  other documents by URI or path, which libSBML and libAntimony would read. The rule comes before
  libSBML's errors because libSBML reports a comp declaration whose `required` is `false` as an
  error itself. A comp namespace that only an element below `sbml` declares is not a package of the
  document, and libSBML does not interpret it;
- an error or a fatal error that libSBML reports: an engine failure of type `libsbml`, with the
  first one's message and where it is in the source, `details.line` and `details.column` (from 1;
  the byte order mark, the white space and the line that the provider adds or leaves out are
  counted as the source has them);
- `package`: a Level 3 document that requires a package (`required="true"`). A package that the
  document does not require is ignored, as SBML allows; one that libSBML does not know and that the
  document requires is an error of libSBML's, above. libAntimony itself writes the package
  `distrib`, required, into the SBML of a model that uses a distribution (`normal(0, 1)`, for
  example): that SBML is refused by this rule, so such a model converts from Antimony to SBML but
  not back.

The refusals `comp` and `package` name the package's namespace in `details.package`. An entity
reference without a declaration is an error of libSBML's, and XInclude elements and style-sheet
processing instructions are not interpreted, so no file is read for an SBML source: the provider
tests check this with files that exist.

**Order of work.** Before the call polls for cancellation: `empty`, then the rules of the format
that read the text alone (`xml`, then the check of imports, which refuses with `import` or
`unterminated`, whichever it meets first; or `not_document`, then `document_type`). Then the lock.
Under it: for `sbml`, `comp`, libSBML's first error and `package`; then the translation, and for
`sbml` written from `antimony`, the refusal of a layout (Layouts, below).

**Modules.** libAntimony gives every source a module `__main`, for statements outside every module,
besides the modules the source defines; an SBML document defines one, named after its model's id,
which is its main module. In Antimony, the main module is the module marked with `*` (the last of
them, with a warning, when several are); else `__main`, when statements stand outside every
module; else the last module defined. `module` names any of these, and a submodule then translates
alone; an instance of a module (`A` in `A: decay_unit()`) is not a module. An unknown name is
`NOT_FOUND`, with `details.module` and `details.modules`, the names of the source's modules. The
report's `module` is the module translated.

**Engine failures.** A source that libAntimony cannot read is `DATA_INVALID`, with
`details.engine_error` `{"type": "libantimony", "message": <libAntimony's error>}` and without
`details.line` and `details.column`. libAntimony's message names a line (`Error in model string,
line <n>:`), but libAntimony counts no line end inside a block comment or a triple-backtick block
and counts a run of continued lines as one, so after such a block its line is not the source's;
the message is reported as libAntimony wrote it. libAntimony's message can quote the first byte of
a character it cannot read, which is not UTF-8 alone; that byte becomes U+FFFD. A module that
libAntimony cannot write is the same failure; libAntimony out of memory is `RESOURCE_EXHAUSTED`.
libSBML's failures (format `sbml`) do have `details.line` and `details.column`, located in the
source.

**Warnings.** The report's `warnings` are libAntimony's warnings about the translation
(`getWarnings()`), such as `Warning: changing main module for this file to be c instead of 'a'.`
for a source that marks two modules as the main one. libSBML's own warnings (about units, for
example) are not reported.

**The lock.** libAntimony keeps what it loads, its last error, its warnings and its settings in
process-global state, and libSBML's writer keeps a process-global setting, so one process-wide lock
serializes the provider's every use of libAntimony and libSBML. A call polls `cancel_requested()`
once before it takes the lock and every 20 ms while it waits for it (the same code as
`test-conversion`'s; each provider has a lock of its own). Under the lock, a call clears what
libAntimony loaded and the directories it searches (`clearPreviousLoads()`, `clearDirectories()`);
for `sbml`, applies the rules that need libSBML; loads the text with `loadAntimonyString` or
`loadSBMLString` (never with a function that takes a file name, nor with `loadString`, which tries
every format); finds the module (`getMainModuleName()`, `getNumModules()`, `getNthModuleName()`);
writes it; copies the text, the warnings and any error into the provider's memory; and frees what
libAntimony handed out (`freeAll()`; a pointer that libAntimony returned is never passed to
`free()`) and clears what it loaded. A translation in progress is not interrupted. Its time grows
faster than the source: a chain of 500 reactions took about 0.3 s, of 700 about 0.55 s and of 1000
about 0.8 s on the machine where this was checked. How long one translation may hold the lock is an
open question ([open-questions.md](../open-questions.md), OQ-30).

**Layouts.** An Antimony source can ask for a drawing of its network: `model.layout = on`, a
`model.layout.*` or `model.autolayout.*` setting, an element's position, size, color, font, shape or
lines (`S1.x = 28`, `S1.position = {40, 30}`, `S1.color = "red"`), or the default style of the
species, reactions or compartments (`species.color = "blue"`). libAntimony then has SBMLNetwork lay
the network out. SBMLNetwork 0.5.12's automatic layout places the elements at random positions that
it draws with the C library's `rand()` after seeding it with `srand(time(NULL))`, so the SBML would
change from one second to the next. The provider therefore refuses such a translation: when the SBML
that libAntimony wrote declares the package `layout` or `render` (read with libSBML, under the
lock), the call fails with `UNSUPPORTED` and `details.reason` `"layout"`, and returns no text. The
request cannot be seen before libAntimony acts on it: libAntimony lays the network out when it loads
the source and again when it writes the SBML.

SBML that holds a layout converts to Antimony: libAntimony reads its positions and styles and writes
them as Antimony statements, and the text is the same bytes in different seconds (checked across a
second of the wall clock). Loading it runs SBMLNetwork's layout once, whose random positions do not
reach the text. That Antimony, converted back to SBML, is refused as above.

**Process state.**
- libAntimony sets the process's locale to `C` (`setlocale(LC_ALL, "C")`) while it reads and writes
  a model, and restores it afterwards; libSBML switches the locale too, in both providers' copies.
  `setlocale` is process-wide: other code in the process (the host application, modules, other
  providers) may see `C` while a conversion runs, and switches that overlap on different threads can
  leave the process in `C`. The runtime does not depend on the locale.
- SBMLNetwork's automatic layout calls `srand(time(NULL))` and `rand()`: a refused layout request,
  and SBML with a layout converted to Antimony, reseed the C library's `rand()` for the whole
  process. Code that draws numbers with `rand()` after seeding it with `srand()` (from a run's seed,
  for example) gets another sequence when such a conversion runs in the same process; modules should
  draw random numbers from generators of their own.
- libAntimony and libSBML write nothing to standard output or standard error.

**Reproducibility.** For one engine identity, a translation's text is the same bytes in one process
and across processes (checked on macOS arm64 with libAntimony 3.3.0, libSBML 5.21.2 and SBMLNetwork
0.5.12). That holds because layouts computed from the wall clock are refused (Layouts, above); SBML
that holds a layout gives the same Antimony in different seconds. Antimony translated to SBML, back
to Antimony and to SBML again gives the first SBML for the provider's test models; the order of
declarations in a source can make the first round differ from later ones, since libAntimony writes
Antimony in an order of its own. A model that uses a distribution does not come back at all: its
SBML requires `distrib` (the rule `package`).

## The test provider `test-conversion`

The fixture provider `test-conversion` (`tests/fixtures/providers/test-conversion/`, built for the
tests only and never installed) offers `praenora.model-conversion@0.0` without an engine, so that
the service checks and modules using the service run in every build. Its engine object is
`{"name":"test-conversion-engine","version":"1.0.0"}`. It converts between two toy formats of one
model, both ways:

- **Format `test-decay`.** The model format of the simulation service's test provider
  `test-simulation`
  ([simulation-service.md](simulation-service.md#the-test-provider-test-simulation)), with its
  rules: a JSON object `{"states": {...}, "parameters": {...}}`. A model whose `events` are
  not empty is `UNSUPPORTED` at `/events`, since `test-decay-text` has no events.
- **Format `test-decay-text`.** Lines of words separated by spaces and tabs; `#` starts a comment
  that runs to the end of its line. Each line is empty, or one of

  ```text
  state <id> <number or parameter id>
  parameter <id> <number>
  hold <milliseconds>
  ```

  with the ids and rules of `test-decay`; numbers are JSON numbers, and the milliseconds of a
  `hold` an integer from 0 to 10000 without leading zeros. For example:

  ```text
  # x halves in about 1.39 time units
  state x 10
  parameter k_x 0.5
  ```

- **Rule `import`.** A `test-decay-text` line whose first word is `import` refuses the source;
  `details.line` is that line.
- **Translations.** `test-decay-text` to `test-decay` writes the model as canonical JSON, every
  number a double: `{"parameters":{"k_x":0.5},"states":{"x":10.0}}` for the example, which
  `test-simulation` opens. `test-decay` to `test-decay-text` writes the `state` lines, then the
  `parameter` lines, each sorted by id (UTF-8 bytes), numbers as canonical JSON writes them, each
  line ending in a line feed: `state x 10.0` and `parameter k_x 0.5` for the example. Comments and
  `hold` lines never reach the output.
- **hold.** Each `hold` line adds the warning `hold <milliseconds> ignored in output` to the report
  and holds the provider's lock that long, so that the checks can test serialization and
  cancellation while a call waits.
- **Byte order mark and line ends.** A leading UTF-8 byte order mark is skipped in both formats; a
  CR before a line feed belongs to the line end in `test-decay-text`, and is white space in
  `test-decay`.
- **Module.** Each source has one module, `main`.
- **Engine failures.** Anything else malformed, a number too large for a double included, is
  `DATA_INVALID`, with `details.engine_error` `{"type": "test-conversion", "message"}` and, for a
  problem on a line, `details.line`.
- One process-wide lock serializes its translations. A call polls `cancel_requested()` before it
  takes the lock and every 20 ms while it waits, with the code that the `antimony` provider uses
  for its own lock. It logs a debug message through the calling instance's host for each
  conversion, and `test-conversion started` to its own logger when it starts.

## Service checks

The service checks are a GoogleTest suite, `ModelConversionService` (CTest label `contract`;
[`tests/contract/model_conversion_service_checks.hpp`](../../tests/contract/model_conversion_service_checks.hpp)),
that load a provider through the runtime's provider loader and test this document against its
table. A **parameter set** names the provider's manifest and engine object, `describe`'s
conversions, translations the provider must produce exactly (source, options, text and warnings),
the sources its rules refuse, a source its engine cannot read, optionally a source with several
modules, and optionally a source whose translation holds the provider's lock. The checks cover:

- the table's header and functions, and `describe` (the engine equals the manifest's, the
  conversions are sorted, the same bytes every time and from any thread);
- every translation, exactly, with its canonical report; repeated calls that give the same bytes,
  also for refusals and engine failures;
- the rules (with `details.rule`, never an engine error), engine failures (with
  `details.engine_error`), and empty sources in every format the provider reads;
- the options and their pointers, unsupported formats and pairs, unknown modules, sources that are
  not UTF-8 or hold a NUL byte, and the size limits, checked before anything is parsed and not
  before the limit is passed;
- NULL and refusing sinks, a NULL host, host APIs whose `struct_size` ends before a callback, and
  cancellation before the work;
- with a source that holds the lock: a call cancelled while it waits for the lock, and a second
  conversion that waits for the first and finishes after it;
- eight threads converting at once, giving what one thread alone gives (run under ThreadSanitizer
  in the `*-tsan` builds).

A provider is checked by instantiating the suite with its parameter set in a source file of its own
(`tests/contract/model_conversion_service_test.cpp` does so for `test-conversion`, with `hold`
lines that hold its lock, and `tests/contract/model_conversion_service_antimony_test.cpp` for
`antimony`, whose translation of a generated chain of 700 reactions holds its lock). The fixture
module `converting-module` uses the service from a module, through the runtime: its test vectors
convert both ways and simulate a converted model through the simulation service in the contract
checks, and integration tests run it in a workflow.

## References

- [ADR-0035](../adr/ADR-0035-model-conversion-service-v0.md): the model conversion service and its
  providers.
- [ADR-0026](../adr/ADR-0026-runtime-service-providers.md) and
  [runtime-services.md](runtime-services.md): service providers, resolution and the fingerprint.
- [ADR-0021](../adr/ADR-0021-runtime-services-through-host-api.md): runtime services through the
  host API.
- [simulation-service.md](simulation-service.md): the simulation service, whose conventions this
  service shares.
- [module-abi.md](module-abi.md): conventions, the host API and sinks.
- [threading-and-ownership.md](threading-and-ownership.md): threads and ownership across the ABI.
- [reproducibility.md](reproducibility.md): canonical JSON and the fingerprint.
- [feasibility/libantimony.md](../feasibility/libantimony.md): upstream facts about libAntimony,
  the engine of the provider `antimony`, and future work.
