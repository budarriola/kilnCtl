/* hal_status.c -- name table for hal_status_t. Backend-independent: no
 * vendor headers, links into esp/pico/host builds alike. */
#include "hal_status.h"

const char *hal_status_to_name(hal_status_t status)
{
    switch (status) {
    case HAL_OK:             return "HAL_OK";
    case HAL_TIMEOUT:        return "HAL_TIMEOUT";
    case HAL_BUSY:           return "HAL_BUSY";
    case HAL_INVALID_ARG:    return "HAL_INVALID_ARG";
    case HAL_NO_MEM:         return "HAL_NO_MEM";
    case HAL_IO:             return "HAL_IO";
    case HAL_WEDGED:         return "HAL_WEDGED";
    case HAL_NOT_READY:      return "HAL_NOT_READY";
    case HAL_VERIFY_FAILED:  return "HAL_VERIFY_FAILED";
    case HAL_INVALID_SIZE:   return "HAL_INVALID_SIZE";
    case HAL_NOT_FOUND:      return "HAL_NOT_FOUND";
    case HAL_NOT_SUPPORTED:  return "HAL_NOT_SUPPORTED";
    default:                 return "HAL_UNKNOWN";
    }
}
