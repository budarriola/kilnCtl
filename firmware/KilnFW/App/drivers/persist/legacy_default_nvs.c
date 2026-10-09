// See legacy_default_nvs.h.
#include "legacy_default_nvs.h"

#include <stdio.h>

#include "esp_log.h"

#include "hal_esp_common.h"
#include "hal_kv.h"

static const char *TAG = "legacy_nvs";

/* Must match the migrating modules' own definitions (zones_http_internal.h
 * NVS_NAMESPACE/NVS_KEY_ZONES, run_state.c NVS_KEY_RUN, relay_cycles.c
 * NVS_KEY_CYCLES, profiles_http.c NVS_KEY_USED/profN). The host tests load
 * through those real migrations, so drift here fails them. */
#define LEGACY_NAMESPACE "kiln_cfg"
#define PROFILES_MAX 8 /* the legacy default-partition bitmap is a u8: ids 0..7 only (profiles_http.c migrate_from_default_partition) */

static esp_err_t erase_keys(const char *const *keys, size_t n)
{
    hal_kv_handle_t h;
    hal_status_t st = hal_kv_open(&h, LEGACY_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL);
    if (st == HAL_NOT_FOUND) {
        return ESP_OK; /* namespace never existed on the default partition */
    }
    if (st != HAL_OK) {
        ESP_LOGE(TAG, "cannot open default-partition namespace: %s", hal_status_to_name(st));
        return hal_status_to_esp_err(st);
    }
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < n; i++) {
        hal_status_t e = hal_kv_erase_key(&h, keys[i]);
        if (e != HAL_OK && e != HAL_NOT_FOUND) {
            ESP_LOGE(TAG, "erase of legacy key '%s' failed: %s", keys[i], hal_status_to_name(e));
            if (first == ESP_OK) {
                first = hal_status_to_esp_err(e);
            }
        }
    }
    hal_status_t c = hal_kv_commit(&h);
    hal_kv_close(&h);
    if (c != HAL_OK) {
        ESP_LOGE(TAG, "commit after legacy erase failed: %s", hal_status_to_name(c));
        if (first == ESP_OK) {
            first = hal_status_to_esp_err(c);
        }
    }
    return first;
}

esp_err_t legacy_default_nvs_erase_kiln(void)
{
    static const char *const keys[] = { "zones_cfg", "run_state", "relay_cyc" };
    return erase_keys(keys, sizeof(keys) / sizeof(keys[0]));
}

esp_err_t legacy_default_nvs_erase_relay_cycles(void)
{
    static const char *const keys[] = { "relay_cyc" };
    return erase_keys(keys, 1);
}

esp_err_t legacy_default_nvs_erase_profiles(void)
{
    char names[PROFILES_MAX + 1][12];
    const char *keys[PROFILES_MAX + 1];
    snprintf(names[0], sizeof(names[0]), "prof_used");
    keys[0] = names[0];
    for (int i = 0; i < PROFILES_MAX; i++) {
        snprintf(names[i + 1], sizeof(names[i + 1]), "prof%d", i);
        keys[i + 1] = names[i + 1];
    }
    return erase_keys(keys, PROFILES_MAX + 1);
}
