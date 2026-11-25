/* The Python core's interface: the one library of the runtime that embeds CPython and links the
 * environment's libpython (docs/specs/python-service.md, "The Python core").
 *
 * Internal: built only in this repository, never installed as a header. The providers that run
 * Python load the core by its absolute path and call it through this table; it holds the
 * process's one interpreter, so its state is process-wide. The conventions are those of
 * provider_abi.h: only fixed-width integers, size_t, pointers, length-delimited UTF-8 and sinks
 * cross the table, and no exception or longjmp leaves a function of the core. Every sink is
 * required (INVALID_ARGUMENT when it or its write function is NULL; nothing is written when
 * error_json is the one missing); a failed call writes an error document {"code", "message",
 * "details"} to error_json, and when error_json refuses it the call's status stays as it is.
 *
 * C99 and C++. Depends only on <praenora/sdk/python.h> and provider_abi.h. */
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_PYTHON_CORE_H
#define PRAENORA_PROVIDERS_PYTHON_CORE_PYTHON_CORE_H

#include <praenora/sdk/python.h> /* praenora_python_object */

#include "provider_abi.h" /* praenora_provider_host */

/* The version of this interface: built with the providers, from the same sources. */
#define PRAENORA_PYTHON_CORE_MAJOR 0u
#define PRAENORA_PYTHON_CORE_MINOR 0u

/* The name of the core library's single exported symbol, for dlsym and GetProcAddress. */
#define PRAENORA_PYTHON_CORE_ENTRY_POINT "praenora_python_core_get_api"

#ifdef __cplusplus
extern "C" {
#endif

/* The v0 table. Every function is required. */
typedef struct praenora_python_core_v0 {
    praenora_api_header header; /* abi_major PRAENORA_PYTHON_CORE_MAJOR; struct_size; reserved 0 */
    /* clang-format off */
    /* Initializes the interpreter (first call of the process) or checks that the running one is
       this environment's; `environment_dir` absolute; `sdk_dir` the directory holding
       praenora/sdk; records outside calls go to `provider_host`'s log. `provider_host` stays
       valid for the rest of the process, as a provider's start() host does. Every call checks
       the process's environment variables and logs a warning, to `provider_host`, for each that
       is unset or has another value. Thread-safe. */
    praenora_status (PRAENORA_CALL *start)(const praenora_provider_host* provider_host,
        praenora_bytes environment_dir, praenora_bytes sdk_dir, praenora_sink* error_json);
    /* As praenora_python_v0.describe, after a successful start (INVALID_STATE before one). */
    praenora_status (PRAENORA_CALL *describe)(praenora_sink* out_json);
    /* As praenora_python_v0.create_object; `module_host` may be NULL (records then go to
       `provider_host`). */
    praenora_status (PRAENORA_CALL *create_object)(const praenora_provider_host* provider_host,
        const praenora_host_api* module_host, praenora_bytes path, praenora_bytes path_sha256,
        praenora_bytes name, praenora_bytes args_json, praenora_python_object** out_object,
        praenora_sink* error_json);
    /* As praenora_python_v0.call; a non-NULL `module_host` replaces the object's host for the
       call (records and cancel flag). */
    praenora_status (PRAENORA_CALL *call)(praenora_python_object* object,
        const praenora_host_api* module_host, praenora_bytes method, praenora_bytes args_json,
        praenora_sink* result_json, praenora_sink* error_json);
    /* As praenora_python_v0.release_object. */
    void (PRAENORA_CALL *release_object)(praenora_python_object* object);
    /* The build identity the providers compare: 64 lowercase hexadecimal digits, the
       PRAENORA_PYTHON_CORE_BUILD_ID the core was compiled with. Any time, before start too. */
    praenora_status (PRAENORA_CALL *build_id)(praenora_sink* out_text);
    /* clang-format on */
} praenora_python_core_v0;

/* The type of the entry point. */
/* clang-format off */
typedef const praenora_python_core_v0* (PRAENORA_CALL *praenora_python_core_get_api_fn)(void);
/* clang-format on */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Exported by the core library (C linkage). Declared for the core's own code
 * (PRAENORA_PYTHON_CORE_IMPLEMENTATION) and for the core's white-box tests, which link the core
 * (PRAENORA_PYTHON_CORE_LINKED); providers look the symbol up at run time instead. */
#if defined(PRAENORA_PYTHON_CORE_IMPLEMENTATION)
PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_python_core_v0* PRAENORA_CALL
praenora_python_core_get_api(void);
#elif defined(PRAENORA_PYTHON_CORE_LINKED)
PRAENORA_EXTERN_C const praenora_python_core_v0* PRAENORA_CALL praenora_python_core_get_api(void);
#endif

#endif /* PRAENORA_PROVIDERS_PYTHON_CORE_PYTHON_CORE_H */
