#include "ramp_assist_cfg.h"
#include "cfgfs_file_validators.h"

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "legacy_nvs_erase.h"
#include "pref_cfg_fs.h"
#include "relay_authority.h" /* relay_authority_reset_in_flight() -- no save during a factory reset */
#include "cfg_save_lock.h"

static const char *TAG = "ramp_assist_cfg";

// Same partition/namespace unit_pref.c uses for exactly this shape of value
// (a single small scalar, not a struct needing its own version/migration
// story) -- see that module's header comment for the fuller rationale this
// one deliberately does not repeat.
#define KILN_NVS_PARTITION   "kiln_nvs"
#define NVS_NAMESPACE        "kiln_cfg"
#define NVS_KEY_RAMP_ASSIST  "ramp_assist"
// rev counter for the cfg-filesystem dual-write below (docs/FILESYSTEM_USER_DATA.md
// section 5 step 3) -- see unit_pref.c's identical NVS_KEY_UNIT_PREF_REV for
// why this is a separate key rather than a field on the stored value.
#define NVS_KEY_RAMP_ASSIST_REV "ramp_a_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_RAMP_ASSIST);
NVS_KEY_LEN_CHECK(NVS_KEY_RAMP_ASSIST_REV);

// docs/FILESYSTEM_USER_DATA.md section 5 step 3 (prefs move): the file
// this preference dual-writes to on the `cfg` LittleFS partition, once
// mounted -- see pref_cfg_fs.h for the read-through/dual-write/tie-break
// policy this module hands its NVS candidate to.
/* RAMP_ASSIST_FILE_PATH now lives in ramp_assist_cfg.h (kiln-scope reset names it). */

// DEFAULT (2026-11-19, ELEMENT_AGING_PLAN C2): stretch ON, stretch_max_pct 100. A board that has
// never heard of this key, or whose file is unreadable, runs with stretch ON.
//
// File item (after the 4-byte rev prefix pref_cfg_fs adds):
//   legacy, 1 byte : enabled 0/1 (pre-C2 firmware). Read as "never migrated".
//   current, 4 bytes: [0] enabled 0/1, [1] flags (bit0 = on-by-default migration done), [2..3] pct LE.
// A legacy record, or a 4-byte record without the migrated flag, is turned ON once at boot and rewritten
// with the flag set, so a later user OFF (which always writes the flag) sticks.
#define RA_REC_LEN 4
#define RA_FLAG_MIGRATED 0x01u
static bool s_ramp_assist_enabled = true;
static uint16_t s_stretch_max_pct = RAMP_ASSIST_STRETCH_MAX_PCT_DEFAULT;
static uint32_t s_ramp_assist_rev = 0;

// Copied from unit_pref.c/zones_http.c/touch_cal_store.c's identical
// nvs_partition_init() -- same partition, same rationale, same erase-only-
// the-broken-partition scope.
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

// Accepts the legacy 1-byte 0/1 item and the 4-byte record (enabled 0/1, only known flag bits,
// pct <= RAMP_ASSIST_STRETCH_MAX_PCT_LIMIT). Anything else is refused.
bool ramp_assist_cfg_file_validate(const void *bytes, size_t len)
{
    const uint8_t *b = (const uint8_t *)bytes;
    if (len == 1) {
        return b[0] == 0 || b[0] == 1;
    }
    if (len == RA_REC_LEN) {
        uint16_t pct = (uint16_t)(b[2] | (b[3] << 8));
        return b[0] <= 1 && (b[1] & ~RA_FLAG_MIGRATED) == 0 && pct <= RAMP_ASSIST_STRETCH_MAX_PCT_LIMIT;
    }
    return false;
}

/* Persist the record (caller holds s_save_lock). rev advances only on success. */
static cfg_save_lock_t s_save_lock = CFG_SAVE_LOCK_INIT;
static esp_err_t ra_persist_locked(bool enabled, uint16_t pct)
{
    uint8_t rec[RA_REC_LEN] = {enabled ? 1 : 0, RA_FLAG_MIGRATED, (uint8_t)(pct & 0xFF), (uint8_t)(pct >> 8)};
    uint32_t new_rev = s_ramp_assist_rev + 1;
    esp_err_t err = pref_cfg_fs_commit(RAMP_ASSIST_FILE_PATH, rec, sizeof(rec), new_rev, "ramp assist setting");
    if (err == ESP_OK) {
        s_ramp_assist_rev = new_rev;
    }
    return err;
}

esp_err_t ramp_assist_cfg_start(void)
{
    s_ramp_assist_enabled = true; // C2 default (ON) stands until a valid record says otherwise below
    s_stretch_max_pct = RAMP_ASSIST_STRETCH_MAX_PCT_DEFAULT;
    s_ramp_assist_rev = 0;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- ramp assist keeps its default (ON) this boot",
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
        ESP_LOGW(TAG, "hal_kv_open failed: %s -- ramp assist keeps its default (ON) this boot",
                 hal_status_to_name(err));
    } else {
        uint8_t raw = 0;
        hal_status_t rerr = hal_kv_get_u8(&h, NVS_KEY_RAMP_ASSIST, &raw);
        if (rerr == HAL_OK) {
            if (ramp_assist_cfg_file_validate(&raw, 1)) {
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
                ESP_LOGW(TAG, "stored ramp_assist value %u is out of range -- ignored, default (stretch ON) stands",
                         (unsigned)raw);
            }
        } else if (rerr != HAL_NOT_FOUND) {
            load_err = true;
            ESP_LOGW(TAG, "ramp_assist_cfg read failed: %s -- ramp assist keeps its default (ON) this boot",
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

    (void)nvs_raw;
    (void)nvs_rev;
    /* persfx3 MED-3: the frozen NVS copy is never a source. Only the cfg file is read, as a variable-length
     * item (legacy 1 byte or the 4-byte record), so a legacy file is migrated rather than rejected. */
    uint8_t item[RA_REC_LEN + 4];
    size_t item_len = 0;
    uint32_t file_rev = 0;
    esp_err_t lerr = pref_cfg_fs_load_var_checked(RAMP_ASSIST_FILE_PATH, item, sizeof(item), &item_len, &file_rev);
    if (lerr != ESP_OK) {
        if (lerr != ESP_ERR_NOT_FOUND) {
            load_err = true;
            ESP_LOGW(TAG, "ramp_assist file read failed: %s -- default (stretch ON) this boot", esp_err_to_name(lerr));
        }
        return load_err ? ESP_FAIL : ESP_OK;
    }
    if (!ramp_assist_cfg_file_validate(item, item_len)) {
        ESP_LOGW(TAG, "ramp_assist file (%u bytes) is not valid -- default (stretch ON) until the next save",
                 (unsigned)item_len);
        return ESP_OK;
    }
    static const char *const k_legacy[] = {NVS_KEY_RAMP_ASSIST, NVS_KEY_RAMP_ASSIST_REV};
    (void)legacy_nvs_erase_keys(TAG, KILN_NVS_PARTITION, NVS_NAMESPACE, k_legacy, 2);

    bool migrated = (item_len == RA_REC_LEN) && (item[1] & RA_FLAG_MIGRATED) != 0;
    s_ramp_assist_rev = file_rev;
    if (item_len == RA_REC_LEN) {
        s_stretch_max_pct = (uint16_t)(item[2] | (item[3] << 8));
    }
    if (!migrated) {
        /* One-shot: every pre-C2 board is switched ON once (owner 2026-11-19). The record is rewritten
         * with the migrated flag so a later user OFF persists. A failed write only means it repeats. */
        bool was_on = item[0] != 0;
        s_ramp_assist_enabled = true;
        cfg_save_lock_take(&s_save_lock);
        esp_err_t werr = relay_authority_reset_in_flight() ? ESP_ERR_INVALID_STATE
                                                            : ra_persist_locked(true, s_stretch_max_pct);
        cfg_save_lock_give(&s_save_lock);
        ESP_LOGW(TAG, "ramp assist one-shot migration: stretch %s -> ON (stretch_max_pct %u), persist %s",
                 was_on ? "was ON" : "was OFF", (unsigned)s_stretch_max_pct, esp_err_to_name(werr));
    } else {
        s_ramp_assist_enabled = (item[0] != 0);
    }
    ESP_LOGI(TAG, "ramp assist: %s, stretch_max_pct %u (rev=%lu)", s_ramp_assist_enabled ? "ENABLED" : "disabled",
             (unsigned)s_stretch_max_pct, (unsigned long)s_ramp_assist_rev);
    return ESP_OK;
}

bool ramp_assist_cfg_enabled(void)
{
    return s_ramp_assist_enabled;
}

uint16_t ramp_assist_cfg_stretch_max_pct(void)
{
    return s_stretch_max_pct;
}

/* Callers: httpd and backup_import's own async task (http_async_job_try_start),
 * so the rev read, RAM assign, commit and rev bump are one section
 * (docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md, MED-2). cfg_save_lock_t also
 * reserves the flash worker first -- see cfg_save_lock.h. */
static esp_err_t ra_set(bool enabled, uint16_t pct)
{
    if (pct > RAMP_ASSIST_STRETCH_MAX_PCT_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    // Live immediately, same "in-RAM truth first" reasoning unit_pref_set()/
    // watchdog_cfg_set_panic_disabled() use.
    cfg_save_lock_take(&s_save_lock);
    /* LOW-3 (review 10): refuse BEFORE touching RAM, so a refused save leaves live == stored. */
    if (relay_authority_reset_in_flight()) {
        cfg_save_lock_give(&s_save_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ramp_assist_enabled = enabled;
    s_stretch_max_pct = pct;
    // cfg file ONLY -- see unit_pref_set() and docs/CONFIG_FILESYSTEM.md. Always writes the migrated flag.
    esp_err_t err = ra_persist_locked(enabled, pct);
    cfg_save_lock_give(&s_save_lock);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "ramp assist saved: %s, stretch_max_pct %u", enabled ? "ENABLED" : "disabled", (unsigned)pct);
    }
    return err;
}

esp_err_t ramp_assist_cfg_set_enabled(bool enabled)
{
    return ra_set(enabled, s_stretch_max_pct);
}

esp_err_t ramp_assist_cfg_set_stretch_max_pct(uint16_t pct)
{
    return ra_set(s_ramp_assist_enabled, pct);
}

esp_err_t ramp_assist_cfg_set_all(bool enabled, uint16_t pct)
{
    return ra_set(enabled, pct);
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

    uint8_t f_item[RA_REC_LEN + 4];
    size_t f_len = 0;
    uint32_t f_rev = 0;
    bool f_valid = pref_cfg_fs_load_var(RAMP_ASSIST_FILE_PATH, f_item, sizeof(f_item), &f_len, &f_rev) &&
                   ramp_assist_cfg_file_validate(f_item, f_len);
    uint8_t f_raw = f_valid ? f_item[0] : 0;
    if (!f_valid) {
        f_rev = 0;
    }

    bool n_valid = false;
    uint8_t n_raw = 0;
    uint32_t n_rev = 0;
    if (nvs_partition_init(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            uint8_t raw = 0;
            if (hal_kv_get_u8(&h, NVS_KEY_RAMP_ASSIST, &raw) == HAL_OK && ramp_assist_cfg_file_validate(&raw, 1)) {
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
