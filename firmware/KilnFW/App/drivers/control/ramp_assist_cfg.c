#include "ramp_assist_cfg.h"

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "ramp_assist_cfg";

// Same partition/namespace unit_pref.c uses for exactly this shape of value
// (a single small scalar, not a struct needing its own version/migration
// story) -- see that module's header comment for the fuller rationale this
// one deliberately does not repeat.
#define KILN_NVS_PARTITION   "kiln_nvs"
#define NVS_NAMESPACE        "kiln_cfg"
#define NVS_KEY_RAMP_ASSIST  "ramp_assist"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_RAMP_ASSIST);

// SAFE DEFAULT: disabled. See ramp_assist_cfg.h's header comment -- a board
// that has never heard of this key, or whose stored value is unreadable/
// out-of-range, must always come up with today's raw, unassisted behaviour.
static bool s_ramp_assist_enabled = false;

// Copied from unit_pref.c/zones_http.c/touch_cal_store.c's identical
// nvs_partition_init() -- same partition, same rationale, same erase-only-
// the-broken-partition scope.
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

esp_err_t ramp_assist_cfg_start(void)
{
    s_ramp_assist_enabled = false; // safe default stands until proven otherwise below

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- ramp assist stays disabled this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_OK; // non-fatal, same convention as unit_pref_start()/watchdog_cfg_init()
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_NOT_FOUND) {
        // Namespace never written (fresh board, or another module wrote it
        // first but this key specifically was never set) -- disabled is the
        // expected steady state, not an error.
        return ESP_OK;
    }
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "hal_kv_open failed: %s -- ramp assist stays disabled this boot",
                 hal_status_to_name(err));
        return ESP_OK;
    }

    uint8_t raw = 0;
    err = hal_kv_get_u8(&h, NVS_KEY_RAMP_ASSIST, &raw);
    hal_kv_close(&h);
    if (err == HAL_NOT_FOUND) {
        return ESP_OK; // key never set -- disabled default stands
    }
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "ramp_assist_cfg read failed: %s -- ramp assist stays disabled this boot",
                 hal_status_to_name(err));
        return ESP_OK;
    }
    if (raw != 0 && raw != 1) {
        // Out-of-range stored byte (corruption, or a future firmware's wider
        // encoding read by this older build) -- refuse it and stay at the
        // safe default rather than trust a value this build cannot vouch
        // for. Same "corrupt must resolve to the SAFE state, never the
        // other way" rule watchdog_cfg.c's load_disabled_flag() documents
        // for its own panic-disable flag. LOAD-BEARING: the assignment below
        // is a plain truthy cast, not an `== 1` equality check -- without
        // this guard a corrupted byte like 0xAA would cast to true (enabled)
        // rather than fail closed, so this branch is what actually keeps a
        // bit-flipped/garbage byte from being trusted as "enabled".
        ESP_LOGW(TAG, "stored ramp_assist value %u is out of range -- defaulting to disabled",
                 (unsigned)raw);
        return ESP_OK;
    }

    s_ramp_assist_enabled = (raw != 0);
    ESP_LOGI(TAG, "ramp assist: %s", s_ramp_assist_enabled ? "ENABLED" : "disabled");
    return ESP_OK;
}

bool ramp_assist_cfg_enabled(void)
{
    return s_ramp_assist_enabled;
}

esp_err_t ramp_assist_cfg_set_enabled(bool enabled)
{
    // Live immediately, same "in-RAM truth first" reasoning unit_pref_set()/
    // watchdog_cfg_set_panic_disabled() use -- whether or not the NVS write
    // below succeeds, the very next reader (a status poll, or the future
    // ramp-stretch consumer) must see this take effect for the rest of the
    // boot.
    s_ramp_assist_enabled = enabled;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- ramp assist setting not persisted",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hal_kv_open failed: %s -- ramp assist setting not persisted",
                 hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_u8(&h, NVS_KEY_RAMP_ASSIST, enabled ? 1 : 0);
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);

    if (err != HAL_OK) {
        ESP_LOGE(TAG, "could not persist ramp assist setting: %s -- will not survive a reboot",
                 hal_status_to_name(err));
    } else {
        ESP_LOGW(TAG, "ramp assist saved: %s", enabled ? "ENABLED" : "disabled");
    }
    return hal_status_to_esp_err(err);
}
