/* Praenora module ABI v0: the binary contract between the runtime (the host) and modules.
 *
 * A module is a separately built shared library that the host loads at run time. It exports one
 * symbol, praenora_module_get_api, which returns a pointer to a static, versioned function table.
 *
 * Types. Only fixed-width integers, size_t, and object and function pointers cross the boundary;
 * ABI structs contain no enum, bool or C++ types. Strings are UTF-8 and length-delimited (never
 * NUL-terminated). No C++ types, exceptions, longjmp or allocator ownership cross the boundary.
 *
 * Versions. PRAENORA_ABI_MAJOR selects the table generation: the host accepts only tables whose
 * header carries its own major. Minor versions only append: new module functions go at the end
 * of praenora_module_api_v0, new host callbacks at the end of praenora_host_api, and each side
 * reads the other's struct_size to learn which fields exist. A later generation N adds a table
 * type praenora_module_api_vN that starts with the same praenora_api_header.
 *
 * Ownership. Inputs (praenora_bytes, the host API) are borrowed from the host: input bytes for the
 * duration of the call, the host API from create() until destroy() returns. Outputs go through
 * host-owned sinks, which copy the bytes before write() returns. create() allocates an instance
 * and destroy() frees it, both with the module's own allocator. The table and the describe()
 * output are static data of the library.
 *
 * Threading. praenora_module_get_api and describe() are thread-safe and side-effect free. The
 * host makes all calls for one instance from one thread, never concurrently. Host callbacks are
 * thread-safe and may be called from any module thread while the instance exists; a module stops
 * its own threads before destroy() returns.
 *
 * C99 and C++. Besides status.h, this header depends only on <stddef.h> and <stdint.h>. */
#ifndef PRAENORA_SDK_ABI_H
#define PRAENORA_SDK_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "status.h"

/* The ABI version implemented by these headers. The build reads both lines. */
#define PRAENORA_ABI_MAJOR 0u
#define PRAENORA_ABI_MINOR 0u

/* The name of the single exported symbol, for dlsym and GetProcAddress. */
#define PRAENORA_MODULE_ENTRY_POINT "praenora_module_get_api"

/* C linkage for the entry point when compiled as C++. */
#ifdef __cplusplus
#define PRAENORA_EXTERN_C extern "C"
#else
#define PRAENORA_EXTERN_C
#endif

/* Calling convention of every ABI function and callback. Empty on all supported targets; it
 * exists so that a target needing an explicit convention can define one in this header. */
#define PRAENORA_CALL

/* Exports the entry point from a module library built with hidden symbol visibility. */
#if defined(_WIN32) || defined(__CYGWIN__)
#define PRAENORA_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define PRAENORA_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define PRAENORA_MODULE_EXPORT
#endif

/* Levels of praenora_host_api.log and log_enabled. */
#define PRAENORA_LOG_TRACE 0
#define PRAENORA_LOG_DEBUG 1
#define PRAENORA_LOG_INFO 2
#define PRAENORA_LOG_WARN 3
#define PRAENORA_LOG_ERROR 4
#define PRAENORA_LOG_CRITICAL 5

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed UTF-8 bytes; NOT NUL-terminated. Valid only for the duration of the call. */
typedef struct praenora_bytes {
    const char* data;
    size_t size;
} praenora_bytes;

/* A host-owned output channel. A module calls write() zero or more times; the bytes are appended,
 * and the host copies them before write() returns, so the module keeps ownership of its buffers.
 * write() returns a non-OK status when the host refuses the data (for example a size limit); the
 * module should then stop and return that status. */
typedef struct praenora_sink {
    void* context;
    /* Function pointers keep the spelling `ret (PRAENORA_CALL *name)(...)`, which clang-format
     * would change to `ret(PRAENORA_CALL* name)(...)`; it is off for their declarations. */
    /* clang-format off */
    praenora_status (PRAENORA_CALL *write)(void* context, const char* data, size_t size);
    /* clang-format on */
} praenora_sink;

/* Host services for one instance, valid from create() until destroy() returns. */
typedef struct praenora_host_api {
    uint32_t struct_size; /* sizeof this struct as compiled into the host */
    uint32_t abi_major, abi_minor;
    void* context; /* opaque, per instance; pass it to every callback */
    /* clang-format off */
    /* Writes a log message at a PRAENORA_LOG_* level. */
    void (PRAENORA_CALL *log)(void* context, int32_t level, const char* msg, size_t size);
    /* Nonzero if messages at this level are recorded, so a module can skip formatting. */
    int32_t (PRAENORA_CALL *log_enabled)(void* context, int32_t level);
    /* Nonzero once the current call's deadline has passed or a stop was requested during the
     * call: the module should return PRAENORA_STATUS_CANCELLED as soon as possible. Every call
     * starts with it zero. */
    int32_t (PRAENORA_CALL *cancel_requested)(void* context);
    /* A runtime service's function table (a struct that starts with praenora_api_header) for the
     * given name and major version, or NULL if unavailable. */
    const void* (PRAENORA_CALL *get_service)(void* context, const char* name, size_t name_size,
                                             uint32_t service_abi_major);
    /* clang-format on */
} praenora_host_api;

/* Size of the v0.0 host API, up to the end of get_service; a host API of ABI v0 is at least this
 * large. Frozen at the v0.0 layout: callbacks that later minor versions append do not change it,
 * so a module built against later headers still accepts a v0.0 host. */
#define PRAENORA_HOST_API_V0_BASE_SIZE                                                             \
    (offsetof(praenora_host_api, get_service) + sizeof(((praenora_host_api*)0)->get_service))

/* An instance: opaque to the host, defined by the module. */
typedef struct praenora_instance praenora_instance;

/* The generation-independent prefix of every function table. */
typedef struct praenora_api_header {
    uint32_t abi_major;   /* generation of the table that follows */
    uint32_t abi_minor;   /* minor version the module was compiled against */
    uint32_t struct_size; /* sizeof the full table as compiled into the module */
    uint32_t reserved;    /* must be 0 */
} praenora_api_header;

/* The v0 function table. Calls that can fail return a praenora_status: OK, a general code 1-99,
 * or a module-defined code from PRAENORA_STATUS_MODULE_BASE up. On failure a module should write
 * {"code": <int>, "message": "<text>", "details": {...}} to error_json; other text is accepted and
 * treated as the message. Every document is JSON and carries "pmp_version". */
typedef struct praenora_module_api_v0 {
    praenora_api_header header;

    /* Required. */

    /* clang-format off */
    /* Identity JSON: name, version, abi {major, minor}, sdk_version, pmp_version and
     * capabilities [{id, version}]. Callable any time after load. */
    praenora_status (PRAENORA_CALL *describe)(praenora_sink* out_json);
    /* Creates one instance from a create document (pmp_version, step, capability {id, version},
     * module {name, version}, parameters with defaults applied). On success *out_instance holds
     * the instance and destroy() is later called exactly once for it. */
    praenora_status (PRAENORA_CALL *create)(const praenora_host_api* host,
                                            praenora_bytes create_json,
                                            praenora_instance** out_instance,
                                            praenora_sink* error_json);
    /* Called once after a successful create(), with an initialize document (pmp_version,
     * run {fingerprint, seed}, time {unit, start, period}). */
    praenora_status (PRAENORA_CALL *initialize)(praenora_instance*, praenora_bytes initialize_json,
                                                praenora_sink* error_json);
    /* Called once per tick with an invocation document (pmp_version, step, tick, logical_time,
     * inputs {port: envelope}; absent optional inputs are omitted). Writes a result document
     * (outputs {port: {data}}, optional diagnostics object) to result_json. */
    praenora_status (PRAENORA_CALL *invoke)(praenora_instance*, praenora_bytes invocation_json,
                                            praenora_sink* result_json, praenora_sink* error_json);
    /* Called once at shutdown, only if initialize() succeeded. */
    praenora_status (PRAENORA_CALL *finalize)(praenora_instance*, praenora_sink* error_json);
    /* Frees the instance. Cannot fail. */
    void (PRAENORA_CALL *destroy)(praenora_instance*);

    /* Optional (may be NULL). */

    /* A module-defined JSON object describing the instance, requested between calls. */
    praenora_status (PRAENORA_CALL *diagnostics)(praenora_instance*, praenora_sink* out_json);
    /* Used by contract checks only, never concurrently with other calls into the library.
     * Writes {"passed": bool, "checks": [{"name", "passed", "message"}]}. */
    praenora_status (PRAENORA_CALL *self_test)(const praenora_host_api* host,
                                               praenora_sink* report_json);
    /* clang-format on */
} praenora_module_api_v0;

/* Size of the required part of the v0 table. The host accepts a table only if it is non-NULL,
 * header.abi_major is 0, header.reserved is 0, header.struct_size is at least this size, and
 * every required function pointer is non-NULL. */
#define PRAENORA_MODULE_API_V0_BASE_SIZE (offsetof(praenora_module_api_v0, diagnostics))

/* The entry point: returns the table for abi_major, or NULL if the module does not implement
 * that generation. The host calls it with 0. */
/* clang-format off */
typedef const praenora_api_header* (PRAENORA_CALL *praenora_module_get_api_fn)(uint32_t abi_major);
/* clang-format on */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Declared only for module code (praenora_add_module() defines PRAENORA_MODULE_IMPLEMENTATION);
 * the host never links against it, it looks the symbol up at run time. */
#ifdef PRAENORA_MODULE_IMPLEMENTATION
PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_api_header* PRAENORA_CALL
praenora_module_get_api(uint32_t abi_major);
#endif

#endif /* PRAENORA_SDK_ABI_H */
