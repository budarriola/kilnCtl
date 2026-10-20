/* BW-01: implementation of the hal_kv_wstat_* counter declared in hal_kv.h. Included by exactly one
 * hal_kv backend TU (fake_kv.c or hal_kv_esp.c) so every binary that links a backend has it with no
 * build-list change. */
#ifndef HAL_KV_WSTAT_IMPL_H
#define HAL_KV_WSTAT_IMPL_H
#include "hal_kv.h"

/* ---- BW-01: kiln_nvs per-key write counter (declared in hal_kv.h) ---- */
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

#endif
