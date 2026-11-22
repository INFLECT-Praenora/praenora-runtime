/* The model conversion service praenora.model-conversion@0: translating a model source from one
 * format to another through the engine of a service provider
 * (docs/specs/model-conversion-service.md, which is normative).
 *
 * A module declares the service in its manifest (REQUIRES_SERVICES praenora.model-conversion@0.0
 * in praenora_add_module()) and gets this table from its host API:
 *     get_service(context, PRAENORA_MODEL_CONVERSION_SERVICE, 25,
 *                 PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR).
 * It accepts the table when header.abi_major is PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR,
 * header.reserved is 0 and header.struct_size is at least PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE;
 * a later minor version only appends functions, which a module uses only when struct_size covers
 * them.
 *
 * convert() translates one self-contained model source: options_json is {"from": <format>, "to":
 * <format>} and optionally "module", the name of the module of the source to translate (by
 * default its main module); the source is the model's text. describe() lists the pairs of formats
 * the provider converts, for example "antimony" to "sbml" and "sbml" to "antimony".
 *
 * Conventions are those of abi.h: only fixed-width integers, size_t, pointers, length-delimited
 * UTF-8 (praenora_bytes) and sinks cross the table; no exception or longjmp leaves a function.
 *   - Sinks are module-owned: the provider writes to them during the call only. Every sink is
 *     required (INVALID_ARGUMENT when it or its write function is NULL; nothing is written when
 *     error_json is the one missing). A refused write ends the call with the status write()
 *     returned, after an error document to error_json when the refusing sink is another one.
 *   - Inputs are borrowed for the call; their data may be NULL only when their size is 0.
 *   - Order of work: argument checks; the size limits below (RESOURCE_EXHAUSTED before anything
 *     is parsed); the options; the source text (valid UTF-8 without NUL bytes, else
 *     INVALID_ARGUMENT); the rules of its format that read the text alone (a source that is
 *     empty or holds only white space, after an optional UTF-8 byte order mark, is refused in
 *     every format: DATA_INVALID, details.rule "empty"); cancellation; the provider's lock, when
 *     it serializes its translations behind one; the rules that need the engine's parser; the
 *     translation; then out_text and, last, report_json {"from", "to", "module", "warnings":
 *     [...]}. A source a rule refuses never reaches the translation.
 *   - Threading: describe() and convert() may be called from any thread, concurrently. Waiting
 *     for the provider's lock is cancellable; a translation in progress is not.
 *   - host: the calling instance's host API, valid for the call (NULL means: no logging and no
 *     cancellation). The provider checks host->struct_size before each callback, polls
 *     cancel_requested() before the translation starts and while it waits for its lock, returning
 *     CANCELLED, logs what it can attribute to the call through host, and never keeps host.
 *   - Statuses are the general codes of status.h, or the status a refusing sink returned. A failed
 *     call writes an error document of the pmp-error shape {"code", "message", "details"} to
 *     error_json, with details.service "praenora.model-conversion" and, where they apply,
 *     details.pointer (in the options, or in a source of a JSON format), details.format (a
 *     format the provider does not read or write), details.reason (the source's text, or what the
 *     provider does not translate), details.rule (a format rule), details.engine_error (a failure
 *     the engine raised) or details.module (a module the source does not have).
 *
 * C99 and C++. Besides abi.h and status.h, this header depends only on <stddef.h> and
 * <stdint.h>. */
#ifndef PRAENORA_SDK_MODEL_CONVERSION_H
#define PRAENORA_SDK_MODEL_CONVERSION_H

#include <stddef.h>
#include <stdint.h>

#include "abi.h"
#include "status.h"

/* The service's name (25 bytes) and version. A module asks for the major; the table's header
 * reports the minor the provider implements. */
#define PRAENORA_MODEL_CONVERSION_SERVICE "praenora.model-conversion"
#define PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR 0u
#define PRAENORA_MODEL_CONVERSION_SERVICE_MINOR 0u

/* Size limits, checked before anything is parsed. Larger inputs are refused with
 * RESOURCE_EXHAUSTED. */
/* The source of convert(), in bytes (16 MiB). */
#define PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE ((size_t)16 * 1024 * 1024)
/* The options document of convert(), in bytes (1 MiB). */
#define PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE ((size_t)1024 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* The v0 table. Every function is required. */
typedef struct praenora_model_conversion_v0 {
    praenora_api_header header; /* abi_major: the service major (0); abi_minor; struct_size */
    void* context;              /* the provider's; pass it to every function */
    /* clang-format off */
    /* {"service", "version" {major, minor}, "engine" {...}, "conversions": [{"from", "to"}]}.
       Thread-safe, free of side effects, the same bytes every time. */
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);
    /* Translates one self-contained model source. options_json: {"from", "to", "module"?}. On
       success writes the translated text to out_text, then the report {"from", "to", "module",
       "warnings"} to report_json. */
    praenora_status (PRAENORA_CALL *convert)(void* context, const praenora_host_api* host,
        praenora_bytes options_json, praenora_bytes source, praenora_sink* out_text,
        praenora_sink* report_json, praenora_sink* error_json);
    /* clang-format on */
} praenora_model_conversion_v0;

/* Size of the v0.0 table, up to the end of convert: a table of major 0 is at least this large.
 * Frozen at the v0.0 layout, as PRAENORA_HOST_API_V0_BASE_SIZE is. */
#define PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE                                                     \
    (offsetof(praenora_model_conversion_v0, convert) +                                             \
     sizeof(((praenora_model_conversion_v0*)0)->convert))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_SDK_MODEL_CONVERSION_H */
