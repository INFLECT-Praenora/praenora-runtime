/* The Python service praenora.python@0: objects of the Python environment that ships with the
 * runtime, created and called with JSON documents (docs/specs/python-service.md, which is
 * normative).
 *
 * A module declares the service in its manifest (REQUIRES_SERVICES praenora.python@0.0 in
 * praenora_add_module()) and gets this table from its host API:
 *     get_service(context, PRAENORA_PYTHON_SERVICE, 15, PRAENORA_PYTHON_SERVICE_MAJOR).
 * It accepts the table when header.abi_major is PRAENORA_PYTHON_SERVICE_MAJOR, header.reserved is
 * 0 and header.struct_size is at least PRAENORA_PYTHON_V0_BASE_SIZE; a later minor version only
 * appends functions, which a module uses only when struct_size covers them. A Python module's
 * generated library uses this service; a C or C++ module may use it to reach Python packages.
 *
 * create_object() imports a callable, "package.module:attribute", from the Python sources below a
 * directory (or from the environment), calls it with one JSON document and keeps what it returns
 * as an object; call() calls a method of that object with one JSON document and writes what it
 * returns as JSON; release_object() drops the object. One interpreter serves the process: every
 * object of every module shares it, and it is never shut down.
 *
 * Conventions are those of abi.h: only fixed-width integers, size_t, pointers, length-delimited
 * UTF-8 (praenora_bytes) and sinks cross the table; no exception or longjmp leaves a function.
 *   - Sinks are module-owned: the provider writes to them during the call only. Every sink is
 *     required (INVALID_ARGUMENT when it or its write function is NULL; nothing is written when
 *     error_json is the one missing). A refused write ends the call with the status write()
 *     returned, after an error document to error_json when the refusing sink is another one; an
 *     error document that error_json refuses leaves the call's status as it is.
 *   - Inputs are borrowed for the call; their data may be NULL only when their size is 0.
 *   - Documents are JSON in UTF-8, each at most PRAENORA_PYTHON_MAX_DOCUMENT_SIZE bytes: a larger
 *     argument or result is refused with RESOURCE_EXHAUSTED, before anything is parsed or written.
 *     Empty argument bytes count as null. A result is written as compact JSON (null for None);
 *     NaN, an infinity or a value JSON cannot hold is refused with INVALID_ARGUMENT naming the
 *     method. Text that leaves Python and cannot be written as UTF-8 has U+FFFD in its place.
 *   - Threading: describe() may be called from any thread at any time. The calls of one object
 *     come from one thread at a time; different objects may be called from different threads at
 *     the same time, and the interpreter runs their Python code one thread at a time.
 *   - host: the module's host API, borrowed from create_object() until release_object() returns
 *     (NULL means: records go to the provider's log, and nothing cancels). The object's records
 *     (print(), logging, its own Host's log) go through it, and praenora.sdk's cancel_requested()
 *     polls its cancel_requested(): cancellation is cooperative.
 *   - The code below a path is fixed for the process: the first create_object() of a path records
 *     the digest of its files, and a later one that names another digest is refused.
 *   - Statuses are the general codes of status.h, a module-defined code that Python code raised
 *     with praenora.sdk.Error, or the status a refusing sink returned. A failed call writes an
 *     error document {"code", "message", "details"} to error_json: praenora.sdk.Error gives its
 *     code, message and details; praenora.sdk.Cancelled CANCELLED; MemoryError
 *     RESOURCE_EXHAUSTED; any other exception INTERNAL, with the message "<Type>: <message>" and
 *     details.traceback, a list of lines. A method that does not exist is UNSUPPORTED.
 *
 * C99 and C++. Besides abi.h and status.h, this header depends only on <stddef.h> and
 * <stdint.h>. */
#ifndef PRAENORA_SDK_PYTHON_H
#define PRAENORA_SDK_PYTHON_H

#include <stddef.h>
#include <stdint.h>

#include "abi.h"
#include "status.h"

/* The service's name (15 bytes) and version. A module asks for the major; the table's header
 * reports the minor the provider implements. */
#define PRAENORA_PYTHON_SERVICE "praenora.python"
#define PRAENORA_PYTHON_SERVICE_MAJOR 0u
#define PRAENORA_PYTHON_SERVICE_MINOR 0u

/* Each JSON document passed to or written by the service (arguments, results), in bytes
 * (16 MiB). Larger ones are refused with RESOURCE_EXHAUSTED. */
#define PRAENORA_PYTHON_MAX_DOCUMENT_SIZE ((size_t)16 * 1024 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* A Python object kept for a module: opaque, owned by the provider. */
typedef struct praenora_python_object praenora_python_object;

/* The v0 table. Every function is required. */
typedef struct praenora_python_v0 {
    praenora_api_header header; /* abi_major: the service major (0); abi_minor; struct_size */
    void* context;              /* the provider's; pass it to describe and create_object */
    /* clang-format off */
    /* {"python": {"implementation", "version", "abi"}, "environment": {"name", "version",
       "package", "lock_sha256"}}. Thread-safe, free of side effects, the same bytes every
       time. */
    praenora_status (PRAENORA_CALL *describe)(void* context, praenora_sink* out_json);
    /* Adds `path` (an absolute directory, UTF-8; empty for none) to the import path after the
       environment's own entries, checks `path_sha256` (64 lowercase hexadecimal digits; empty:
       no check) against the digest of the files below `path`, imports the callable `name`
       ("package.module:attribute"), calls it with the JSON document `args_json` and keeps what
       it returns as an object. The top-level package of `name` must come from below `path`
       when `path` is given. On success *out_object holds the object; release it with
       release_object. */
    praenora_status (PRAENORA_CALL *create_object)(void* context, const praenora_host_api* host,
        praenora_bytes path, praenora_bytes path_sha256, praenora_bytes name,
        praenora_bytes args_json, praenora_python_object** out_object, praenora_sink* error_json);
    /* Calls object.<method>(document) with `args_json` parsed and writes what it returns, as
       JSON, to result_json. */
    praenora_status (PRAENORA_CALL *call)(praenora_python_object* object, praenora_bytes method,
        praenora_bytes args_json, praenora_sink* result_json, praenora_sink* error_json);
    /* Drops the object; release_object(NULL) does nothing. */
    void (PRAENORA_CALL *release_object)(praenora_python_object* object);
    /* clang-format on */
} praenora_python_v0;

/* Size of the v0.0 table, up to the end of release_object: a table of major 0 is at least this
 * large. Frozen at the v0.0 layout, as PRAENORA_HOST_API_V0_BASE_SIZE is. */
#define PRAENORA_PYTHON_V0_BASE_SIZE                                                               \
    (offsetof(praenora_python_v0, release_object) +                                                \
     sizeof(((praenora_python_v0*)0)->release_object))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_SDK_PYTHON_H */
