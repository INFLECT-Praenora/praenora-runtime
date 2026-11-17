/* Praenora provider interface v0: the binary contract between the runtime and a service provider.
 *
 * Internal to the runtime: built only in this repository, never installed, and free to change
 * with any runtime release. A service provider is a separately built shared library, shipped with
 * the runtime and described by a provider manifest (<name>.provider.json), that offers runtime
 * services: function tables a module reaches through praenora_host_api.get_service. The runtime
 * loads a provider only when something needs one of its services, starts it once per process and
 * never unloads or stops it (docs/specs/runtime-services.md).
 *
 * The conventions are those of <praenora/sdk/abi.h>: only fixed-width integers, size_t, pointers,
 * length-delimited UTF-8 and host-owned sinks cross the boundary; no exception or longjmp leaves a
 * function of the provider.
 *
 * C99 and C++. Depends only on <praenora/sdk/abi.h>. */
#ifndef PRAENORA_RUNTIME_PROVIDER_PROVIDER_ABI_H
#define PRAENORA_RUNTIME_PROVIDER_PROVIDER_ABI_H

#include <praenora/sdk/abi.h>

/* The provider interface version implemented by this header. The build reads both lines. */
#define PRAENORA_PROVIDER_ABI_MAJOR 0u
#define PRAENORA_PROVIDER_ABI_MINOR 0u

/* The name of the provider library's single exported symbol, for dlsym and GetProcAddress. */
#define PRAENORA_PROVIDER_ENTRY_POINT "praenora_provider_get_api"

#ifdef __cplusplus
extern "C" {
#endif

/* What the runtime offers a provider. Valid from start() for the rest of the process. */
typedef struct praenora_provider_host {
    uint32_t struct_size; /* sizeof as compiled into the runtime */
    uint32_t reserved;    /* 0 */
    void* context;
    /* clang-format off */
    /* Writes a log message at a PRAENORA_LOG_* level to the provider's own logger. Thread-safe. */
    void (PRAENORA_CALL *log)(void* context, int32_t level, const char* msg, size_t size);
    /* Nonzero if messages at this level are recorded, so a provider can skip formatting. */
    int32_t (PRAENORA_CALL *log_enabled)(void* context, int32_t level);
    /* clang-format on */
} praenora_provider_host;

/* The v0 function table. Every function is required. */
typedef struct praenora_provider_api_v0 {
    praenora_api_header header; /* abi_major PRAENORA_PROVIDER_ABI_MAJOR; struct_size; reserved 0 */
    /* clang-format off */
    /* {"name", "version", "services": [{"name","major","minor"}], "engine": {...}}.
       Thread-safe, free of side effects. */
    praenora_status (PRAENORA_CALL *describe)(praenora_sink* out_json);
    /* Called exactly once per process, before any get_service, under the runtime's lock. `host`
       stays valid for the rest of the process. */
    praenora_status (PRAENORA_CALL *start)(const praenora_provider_host* host,
                                           praenora_sink* error_json);
    /* After a successful start: the table of a service the provider offers (a struct starting
       with praenora_api_header), or NULL. Tables live until the process ends. Thread-safe. */
    const void* (PRAENORA_CALL *get_service)(const char* name, size_t name_size, uint32_t major);
    /* clang-format on */
} praenora_provider_api_v0;

/* The type of the entry point. */
/* clang-format off */
typedef const praenora_provider_api_v0* (PRAENORA_CALL *praenora_provider_get_api_fn)(void);
/* clang-format on */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Exported by the provider library (PRAENORA_MODULE_EXPORT, C linkage). Declared only for provider
 * code (praenora_add_provider() defines PRAENORA_PROVIDER_IMPLEMENTATION); the runtime never links
 * against it, it looks the symbol up at run time. */
#ifdef PRAENORA_PROVIDER_IMPLEMENTATION
PRAENORA_EXTERN_C PRAENORA_MODULE_EXPORT const praenora_provider_api_v0* PRAENORA_CALL
praenora_provider_get_api(void);
#endif

#endif /* PRAENORA_RUNTIME_PROVIDER_PROVIDER_ABI_H */
