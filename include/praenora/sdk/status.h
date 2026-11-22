/* Status codes shared by the module ABI and the runtime.
 *
 * One int32 code space (the error taxonomy): 0 is success, 1-99 are general codes that modules
 * may return, 100-999 belong to the runtime, and codes from PRAENORA_STATUS_MODULE_BASE (10000)
 * up are module-defined and reported verbatim. This header defines the general codes and the
 * module base; the C++ praenora::ErrorCode covers the whole space.
 *
 * C99 and C++; depends only on <stdint.h>. */
#ifndef PRAENORA_SDK_STATUS_H
#define PRAENORA_SDK_STATUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The result of every ABI call that can fail. */
typedef int32_t praenora_status;

#define PRAENORA_STATUS_OK 0
#define PRAENORA_STATUS_UNKNOWN 1
#define PRAENORA_STATUS_INVALID_ARGUMENT 2
#define PRAENORA_STATUS_INVALID_STATE 3
#define PRAENORA_STATUS_NOT_FOUND 4
#define PRAENORA_STATUS_UNSUPPORTED 5
#define PRAENORA_STATUS_RESOURCE_EXHAUSTED 6
#define PRAENORA_STATUS_CANCELLED 7
#define PRAENORA_STATUS_TIMEOUT 8
#define PRAENORA_STATUS_IO_ERROR 9
#define PRAENORA_STATUS_INTERNAL 10
#define PRAENORA_STATUS_FAILED 11
#define PRAENORA_STATUS_DATA_INVALID 12

/* First module-defined code. */
#define PRAENORA_STATUS_MODULE_BASE 10000

/* The stable name of a general code (0-12), for example "CANCELLED"; "UNKNOWN" for any other
 * value. The returned string is static. */
static inline const char* praenora_status_name(int32_t status) {
    switch (status) {
    case PRAENORA_STATUS_OK:
        return "OK";
    case PRAENORA_STATUS_UNKNOWN:
        return "UNKNOWN";
    case PRAENORA_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case PRAENORA_STATUS_INVALID_STATE:
        return "INVALID_STATE";
    case PRAENORA_STATUS_NOT_FOUND:
        return "NOT_FOUND";
    case PRAENORA_STATUS_UNSUPPORTED:
        return "UNSUPPORTED";
    case PRAENORA_STATUS_RESOURCE_EXHAUSTED:
        return "RESOURCE_EXHAUSTED";
    case PRAENORA_STATUS_CANCELLED:
        return "CANCELLED";
    case PRAENORA_STATUS_TIMEOUT:
        return "TIMEOUT";
    case PRAENORA_STATUS_IO_ERROR:
        return "IO_ERROR";
    case PRAENORA_STATUS_INTERNAL:
        return "INTERNAL";
    case PRAENORA_STATUS_FAILED:
        return "FAILED";
    case PRAENORA_STATUS_DATA_INVALID:
        return "DATA_INVALID";
    default:
        return "UNKNOWN";
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRAENORA_SDK_STATUS_H */
