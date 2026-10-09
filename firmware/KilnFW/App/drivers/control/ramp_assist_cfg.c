#include "ramp_assist_cfg.h"

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"

static const char *TAG = "ramp_assist_cfg";

// Same partition/namespace unit_pref.c uses for exactly this shape of value
// (a single small scalar, not a struct needing its own version/migration
// story) -- see that module's header comment for the fuller rationale this
// one deliberately does not repeat.
#define KILN_NVS_PARTITION   "kiln_nvs"
#define NVS_NAMESPACE        "kiln_cfg"
#define NVS_KEY_RAMP_ASSIST  "ramp_assist"
// rev counter for the cfg-filesystem dual-write below (docs/FILESYSTEM_USER_DATA_PLAN.md
// section 5 step 3) -- see unit_pref.c's identical NVS_KEY_UNIT_PREF_REV for
// why this is a separate key rather than a field on the stored value.
#define NVS_KEY_RAMP_ASSIST_REV "ramp_a_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_RAMP_ASSIST);
NVS_KEY_LEN_CHECK(NVS_KEY_RAMP_ASSIST_REV);

// docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 3 (prefs move): the file
// this preference dual-writes to on the `cfg` LittleFS partition, once
// mounted -- see pref_cfg_fs.h for the read-through/dual-write/tie-break
// policy this module hands its NVS candidate to.
/* RAMP_ASSIST_FILE_PATH now lives in ramp_assist_cfg.h (kiln-scope reset names it). */

// SAFE DEFAULT: disabled. See ramp_assist_cfg.h's header comment -- a board
// that has never heard of this key, or whose stored value is unreadable/
// out-of-range, must always come up with today's raw, unassisted behaviour.
static bool s_ramp_assist_enabled = false;
static uint32_t s_ramp_assist_rev = 0;

// Copied from unit_pref.c/zones_http.c/touch_cal_store.c's identical
// nvs_partition_init() -- same partition, same rationale, same erase-only-
// the-broken-partition scope.
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

// Same 0/1 range check ramp_assist_cfg_start() has always applied to a
// stored NVS byte -- reused verbatim as the pref_cfg_fs_validate_fn_t for
// the file, so the moved item is validated exactly as its NVS path
// validates today.
static bool ramp_assist_validate(const void *bytes, size_t len)
{
    if (len != 1) {
        return false;
    }
    uint8_t raw = *(const uint8_t *)bytes;
    return raw == 0 || raw == 1;
}

esp_err_t ramp_assist_cfg_start(void)
{
    s_ramp_assist_enabled = false; // safe default stands until proven otherwise below
    s_ramp_assist_rev = 0;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- ramp assist stays disabled this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_FAIL; // non-fatal to the caller (logs only); the error lets it latch a startup fault
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    bool nvs_valid = false;
    uint8_t nvs_raw = 0;
    uint32_t nvs_rev = 0;
    bool load_err = false; /* a real NVS open/read error, not merely "never written" */
    if (err == HAL_NOT_FOUND) {
        // Namespace never written (fresh board, or another module wrote it
        // first but this key specifically was never set) -- disabled is the
        // expected steady state, not an error.
    } else if (err != HAL_OK) {
        load_err = true;
        ESP_LOGW(TAG, "hal_kv_open failed: %s -- ramp assist stays disabled this boot",
                 hal_status_to_name(err));
    } else {
        uint8_t raw = 0;
        hal_status_t rerr = hal_kv_get_u8(&h, NVS_KEY_RAMP_ASSIST, &raw);
        if (rerr == HAL_OK) {
            if (ramp_assist_validate(&raw, 1)) {
                nvs_valid = true;
                nvs_raw = raw;
            } else {
                // Out-of-range stored byte (corruption, or a future firmware's
                // wider encoding read by this older build) -- refuse it and
                // stay at the safe default rather than trust a value this
                // build cannot vouch for. LOAD-BEARING: this is a plain
                // truthy-cast-avoidance guard, not an `== 1` shortcut --
                // without it a corrupted byte like 0xAA would cast to true
                // (enabled) rather than fail closed.
                ESP_LOGW(TAG, "stored ramp_assist value %u is out of range -- defaulting to disabled",
                         (unsigned)raw);
            }
        } else if (rerr != HAL_NOT_FOUND) {
            load_err = true;
            ESP_LOGW(TAG, "ramp_assist_cfg read failed: %s -- ramp assist stays disabled this boot",
                     hal_status_to_name(rerr));
        }
        if (nvs_valid) {
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_RAMP_ASSIST_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    uint8_t resolved_raw = nvs_raw;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(RAMP_ASSIST_FILE_PATH, &nvs_raw, sizeof(nvs_raw), nvs_valid, nvs_rev,
                                           ramp_assist_validate, &resolved_raw, &resolved_rev, &used_file);
    if (!have_value) {
        return load_err ? ESP_FAIL : ESP_OK; // neither side had anything trustworthy -- disabled default stands
    }

    s_ramp_assist_enabled = (resolved_raw != 0);
    s_ramp_assist_rev = resolved_rev;
    ESP_LOGI(TAG, "ramp assist: %s (source=%s, rev=%lu)", s_ramp_assist_enabled ? "ENABLED" : "disabled",
             used_file ? "file" : "NVS", (unsigned long)s_ramp_assist_rev);
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
    uint32_t new_rev = s_ramp_assist_rev + 1;
    uint8_t raw = enabled ? 1 : 0;

    // FILE FIRST (best-effort, failure logged and swallowed -- NVS below
    // remains the persistence guarantee), THEN NVS (authoritative).
    esp_err_t file_err = pref_cfg_fs_save(RAMP_ASSIST_FILE_PATH, &raw, sizeof(raw), new_rev);
    if (file_err != ESP_OK && file_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "ramp assist file write failed: %s -- NVS remains the source of truth this boot",
                 esp_err_to_name(file_err));
    }

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
    err = hal_kv_set_u8(&h, NVS_KEY_RAMP_ASSIST, raw);
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_RAMP_ASSIST_REV, new_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);

    if (err != HAL_OK) {
        ESP_LOGE(TAG, "could not persist ramp assist setting: %s -- will not survive a reboot",
                 hal_status_to_name(err));
    } else {
        s_ramp_assist_rev = new_rev;
        ESP_LOGW(TAG, "ramp assist saved: %s", enabled ? "ENABLED" : "disabled");
    }
    return hal_status_to_esp_err(err);
}

void ramp_assist_cfg_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                           bool *diverged)
{
    if (file_valid) {
        *file_valid = false;
    }
    if (file_rev) {
        *file_rev = 0;
    }
    if (nvs_valid) {
        *nvs_valid = false;
    }
    if (nvs_rev) {
        *nvs_rev = 0;
    }
    if (diverged) {
        *diverged = false;
    }

    uint8_t f_raw = 0;
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(RAMP_ASSIST_FILE_PATH, sizeof(f_raw), ramp_assist_validate, &f_raw, &f_rev, &f_valid);

    bool n_valid = false;
    uint8_t n_raw = 0;
    uint32_t n_rev = 0;
    if (nvs_partition_init(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            uint8_t raw = 0;
            if (hal_kv_get_u8(&h, NVS_KEY_RAMP_ASSIST, &raw) == HAL_OK && ramp_assist_validate(&raw, 1)) {
                n_valid = true;
                n_raw = raw;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_RAMP_ASSIST_REV, &rev) == HAL_OK) {
                    n_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
    }

    bool content_equal = f_valid && n_valid && (f_raw == n_raw);
    if (file_valid) {
        *file_valid = f_valid;
    }
    if (file_rev) {
        *file_rev = f_rev;
    }
    if (nvs_valid) {
        *nvs_valid = n_valid;
    }
    if (nvs_rev) {
        *nvs_rev = n_rev;
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
}
