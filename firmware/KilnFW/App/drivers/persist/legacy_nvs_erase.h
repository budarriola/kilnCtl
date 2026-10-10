#pragma once
/* persfx3 MED-3: one-shot retirement of a frozen legacy NVS copy.
 *
 * The five control stores (aux_outputs, ramp_assist, ki_baseline, iter_tune, ct_verify) are cfg-file-only since
 * the 2026-10-06 dual-write close; their NVS keys stopped being written and are frozen at that moment. Once the
 * file is the adopted source for a boot, the frozen copy can only do harm (an operator-disabled aux rule that
 * a later file loss would resurrect), so it is erased here. Header-only so it adds no source to any build list. */

#include <stddef.h>
#include "esp_err.h"
#include "esp_log.h"
#include "hal_kv.h"

/* Erases `keys` from partition/namespace and commits. Absent keys are fine. Returns ESP_OK only when every key
 * is gone afterwards (read-back by existence probe), so a caller can log a failure and leave the retry to the
 * next boot -- the file is authoritative either way. */
static inline esp_err_t legacy_nvs_erase_keys(const char *tag, const char *partition, const char *ns,
                                              const char *const *keys, size_t nkeys)
{
    hal_kv_handle_t h;
    hal_status_t st = hal_kv_open(&h, ns, HAL_KV_MODE_READ_WRITE, partition);
    if (st == HAL_NOT_FOUND) {
        return ESP_OK; /* namespace never created: nothing to erase */
    }
    if (st != HAL_OK) {
        ESP_LOGW(tag, "legacy NVS %s: open failed (%s) -- copy kept, retried next boot", ns, hal_status_to_name(st));
        return ESP_FAIL;
    }
    bool dirty = false;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < nkeys; i++) {
        if (hal_kv_key_exists(&h, keys[i]) != HAL_OK) {
            continue;
        }
        st = hal_kv_erase_key(&h, keys[i]);
        if (st != HAL_OK && st != HAL_NOT_FOUND) {
            ESP_LOGW(tag, "legacy NVS %s/%s: erase failed (%s)", ns, keys[i], hal_status_to_name(st));
            result = ESP_FAIL;
        } else {
            dirty = true;
        }
    }
    if (dirty && hal_kv_commit(&h) != HAL_OK) {
        result = ESP_FAIL;
    }
    for (size_t i = 0; i < nkeys && result == ESP_OK; i++) {
        if (hal_kv_key_exists(&h, keys[i]) == HAL_OK) {
            ESP_LOGW(tag, "legacy NVS %s/%s: still present after erase", ns, keys[i]);
            result = ESP_FAIL;
        }
    }
    hal_kv_close(&h);
    if (dirty && result == ESP_OK) {
        ESP_LOGI(tag, "legacy NVS %s: frozen copy erased (cfg file is authoritative)", ns);
    }
    return result;
}
