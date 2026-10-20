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


/* ---- BW-01: kiln_nvs per-key write counter (declared in hal_kv.h) ---- */
#include "hal_kv.h"
#include <string.h>

static struct {
    char name[HAL_KV_WSTAT_NAME_MAX];
    volatile uint32_t n;
} s_wstat[HAL_KV_WSTAT_SLOTS];
static volatile uint32_t s_wstat_other;

void hal_kv_wstat_note(const char *partition, const char *key)
{
    if (partition == NULL || key == NULL || strcmp(partition, "kiln_nvs") != 0) {
        return;
    }
    for (unsigned i = 0; i < HAL_KV_WSTAT_SLOTS; i++) {
        if (s_wstat[i].name[0] == '\0') {
            size_t kl = strlen(key);
            if (kl > HAL_KV_WSTAT_NAME_MAX - 1) {
                kl = HAL_KV_WSTAT_NAME_MAX - 1;
            }
            memcpy(s_wstat[i].name, key, kl);
            s_wstat[i].name[kl] = '\0';
        }
        if (strncmp(s_wstat[i].name, key, HAL_KV_WSTAT_NAME_MAX - 1) == 0) {
            s_wstat[i].n++;
            return;
        }
    }
    s_wstat_other++;
}

bool hal_kv_wstat_get(unsigned i, char name[HAL_KV_WSTAT_NAME_MAX], uint32_t *count)
{
    if (i < HAL_KV_WSTAT_SLOTS) {
        if (s_wstat[i].name[0] == '\0') {
            return false;
        }
        memcpy(name, s_wstat[i].name, HAL_KV_WSTAT_NAME_MAX);
        *count = s_wstat[i].n;
        return true;
    }
    if (i == HAL_KV_WSTAT_SLOTS && s_wstat_other != 0u) {
        memcpy(name, "other", 6);
        *count = s_wstat_other;
        return true;
    }
    return false;
}

void hal_kv_wstat_reset(void)
{
    memset((void *)s_wstat, 0, sizeof(s_wstat));
    s_wstat_other = 0;
}
