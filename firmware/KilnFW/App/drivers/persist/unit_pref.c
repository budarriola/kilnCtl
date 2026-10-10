#include "unit_pref.h"

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"
#include "cfg_save_lock.h"

static const char *TAG = "unit_pref";

// Same partition zones_http.c/touch_cal_store.c use for board-wide config,
// same namespace profiles_builtin.c's hidden-mask and zones_http.c's zones
// blob use for small persisted preferences -- this is exactly that kind of
// value (a single small scalar, not a struct needing its own version/migration
// story), so it gets a key in the existing namespace rather than a new one.
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "kiln_cfg"
#define NVS_KEY_UNIT_PREF  "unit_pref"
// rev counter for the cfg-filesystem dual-write below (docs/FILESYSTEM_USER_DATA.md
// section 5 step 3) -- a SEPARATE key, same pattern zones_config_cfg_fs.c's
// "zones_rev" key uses rather than a field on the value itself, so the rev
// survives independently of whatever shape unit_pref_t ever takes.
#define NVS_KEY_UNIT_PREF_REV "u_pref_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_UNIT_PREF);
NVS_KEY_LEN_CHECK(NVS_KEY_UNIT_PREF_REV);

// docs/FILESYSTEM_USER_DATA.md section 5 step 3 (prefs move): the file
// this preference dual-writes to on the `cfg` LittleFS partition, once
// mounted -- see pref_cfg_fs.h for the read-through/dual-write/tie-break
// policy this module hands its NVS candidate to.
/* UNIT_PREF_FILE_PATH now lives in unit_pref.h (kiln-scope reset names it). */

static unit_pref_t s_unit_pref = UNIT_PREF_CELSIUS;
static uint32_t s_unit_pref_rev = 0;

// Brings up one NVS partition, erasing ONLY that partition if its contents
// are unusable -- copied verbatim from zones_http.c/touch_cal_store.c's
// nvs_partition_init() (same partition, same rationale: NO_FREE_PAGES /
// NEW_VERSION_FOUND have no other cure, and the erase must stay scoped to the
// partition that is actually broken).
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

// Same in-range check unit_pref_start() has always applied to a stored NVS
// byte -- reused verbatim as the pref_cfg_fs_validate_fn_t for the file, per
// this task's requirement to validate a moved item exactly as its NVS path
// validates today (no relaxing, no new sentinel invented).
static bool unit_pref_validate(const void *bytes, size_t len)
{
    if (len != 1) {
        return false;
    }
    uint8_t raw = *(const uint8_t *)bytes;
    return raw == (uint8_t)UNIT_PREF_CELSIUS || raw == (uint8_t)UNIT_PREF_FAHRENHEIT;
}

esp_err_t unit_pref_start(void)
{
    s_unit_pref = UNIT_PREF_CELSIUS;
    s_unit_pref_rev = 0;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- defaulting to Celsius this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_FAIL; // non-fatal to the caller (logs only); the error lets it latch a startup fault
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    bool nvs_valid = false;
    uint8_t nvs_raw = (uint8_t)UNIT_PREF_CELSIUS;
    uint32_t nvs_rev = 0;
    bool load_err = false; /* a real NVS open/read error, not merely "never written" */
    if (err == HAL_NOT_FOUND) {
        // Namespace never written (fresh board, or zones/profiles wrote it
        // first but this key specifically was never set) -- Celsius is the
        // expected steady state, not an error.
    } else if (err != HAL_OK) {
        load_err = true;
        ESP_LOGW(TAG, "hal_kv_open failed: %s -- defaulting to Celsius this boot",
                 hal_status_to_name(err));
    } else {
        uint8_t raw = (uint8_t)UNIT_PREF_CELSIUS;
        hal_status_t rerr = hal_kv_get_u8(&h, NVS_KEY_UNIT_PREF, &raw);
        if (rerr == HAL_OK) {
            if (unit_pref_validate(&raw, 1)) {
                nvs_valid = true;
                nvs_raw = raw;
            } else {
                // Out-of-range stored byte (corruption, or a future firmware's
                // wider enum read by this older build) -- refuse it rather than
                // trust a value that isn't one of the two this build knows how
                // to render.
                ESP_LOGW(TAG, "stored unit_pref value %u is out of range -- defaulting to Celsius", (unsigned)raw);
            }
        } else if (rerr != HAL_NOT_FOUND) {
            load_err = true;
            ESP_LOGW(TAG, "unit_pref read failed: %s -- defaulting to Celsius this boot",
                     hal_status_to_name(rerr));
        }
        if (nvs_valid) {
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_UNIT_PREF_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    // Hand the NVS candidate to the file-vs-NVS read-through/tie-break
    // policy -- see pref_cfg_fs.h. On every board today (no `cfg`
    // partition mounted) this is a pass-through: file is absent,
    // pref_cfg_fs_resolve() returns exactly the NVS candidate unchanged.
    uint8_t resolved_raw = nvs_raw;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(UNIT_PREF_FILE_PATH, &nvs_raw, sizeof(nvs_raw), nvs_valid, nvs_rev,
                                           unit_pref_validate, &resolved_raw, &resolved_rev, &used_file);
    if (!have_value) {
        return load_err ? ESP_FAIL : ESP_OK; // neither side had anything trustworthy -- Celsius default stands
    }

    s_unit_pref = (unit_pref_t)resolved_raw;
    s_unit_pref_rev = resolved_rev;
    ESP_LOGI(TAG, "unit preference: %s (source=%s, rev=%lu)",
             s_unit_pref == UNIT_PREF_FAHRENHEIT ? "Fahrenheit" : "Celsius", used_file ? "file" : "NVS",
             (unsigned long)s_unit_pref_rev);
    return ESP_OK;
}

unit_pref_t unit_pref_get(void)
{
    return s_unit_pref;
}

/* Callers: httpd, LCD task, UART bridge task -- rev read + commit + bump is one section. */
static cfg_save_lock_t s_save_lock = CFG_SAVE_LOCK_INIT;

esp_err_t unit_pref_set(unit_pref_t pref)
{
    return unit_pref_set_ex(pref, NULL);
}

esp_err_t unit_pref_set_ex(unit_pref_t pref, bool *out_adopted)
{
    if (out_adopted) {
        *out_adopted = false;
    }
    if (pref != UNIT_PREF_CELSIUS && pref != UNIT_PREF_FAHRENHEIT) {
        return ESP_ERR_INVALID_ARG;
    }

    // Persist first, publish to RAM only on success (HTTP_INPUT_PARSING_AUDIT L12):
    // a failed save leaves the live value, and every reader, unchanged. The whole
    // commit + publish + rev bump is one section under the save lock (MED-3).
    cfg_save_lock_take(&s_save_lock);
    if (cfg_save_lock_reset_refused()) { /* factory reset in flight: nothing may persist, RAM stays as is */
        cfg_save_lock_give(&s_save_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t new_rev = s_unit_pref_rev + 1;
    uint8_t raw = (uint8_t)pref;

    // cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"):
    // no NVS write follows, and a failed write is returned, never masked by
    // one. The rev only advances once the write is verified (cfg_fs_write_atomic
    // reads the file back), so a retry reuses the same next rev.
    esp_err_t err = pref_cfg_fs_commit(UNIT_PREF_FILE_PATH, &raw, sizeof(raw), new_rev, "unit preference");
    if (err == ESP_OK) {
        s_unit_pref = pref;
        s_unit_pref_rev = new_rev;
    } else {
        // Review LOW: the write may have landed (rename done) while its read-back failed, so the
        // file could already hold the NEW value at new_rev -- which would win over RAM after a
        // reboot. Choice: re-read the file and adopt what it holds when it carries exactly new_rev,
        // so RAM and file agree. The failure is still reported (err) so the caller knows the save
        // was not verified; a file that still holds the old value leaves RAM untouched (L12).
        uint8_t f_raw = 0;
        uint32_t f_rev = 0;
        bool f_valid = false;
        pref_cfg_fs_load_raw(UNIT_PREF_FILE_PATH, sizeof(f_raw), unit_pref_validate, &f_raw, &f_rev, &f_valid);
        if (f_valid && f_rev == new_rev) {
            s_unit_pref = (unit_pref_t)f_raw;
            s_unit_pref_rev = f_rev;
            if (out_adopted) {
                *out_adopted = true;
            }
            ESP_LOGW(TAG, "unit preference save reported %s but the file holds the new value (rev %lu) -- RAM "
                          "adopts it so a reboot cannot flip the live value",
                     esp_err_to_name(err), (unsigned long)f_rev);
        }
    }
    cfg_save_lock_give(&s_save_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "unit preference saved: %s", pref == UNIT_PREF_FAHRENHEIT ? "Fahrenheit" : "Celsius");
    }
    return err;
}

const char *unit_pref_suffix(unit_pref_t pref)
{
    return pref == UNIT_PREF_FAHRENHEIT ? "F" : "C";
}

float unit_pref_convert(float value_c, unit_pref_t pref, unit_pref_kind_t kind)
{
    if (pref != UNIT_PREF_FAHRENHEIT) {
        return value_c; // Celsius is always the identity map.
    }
    // This is the whole reason unit_pref_kind_t is a required argument
    // rather than one function with an implicit "always absolute" behavior:
    // an ABSOLUTE reading needs the +32 offset (0C = 32F), a RATE/delta must
    // not get it (a rate has no fixed point on either scale to add) -- see
    // unit_pref.h's doc comment on unit_pref_kind_t for the full reasoning
    // and the current "nothing calls this with RATE yet" status.
    if (kind == UNIT_PREF_KIND_RATE) {
        return value_c * 9.0f / 5.0f;
    }
    return value_c * 9.0f / 5.0f + 32.0f;
}

void unit_pref_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
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
    pref_cfg_fs_load_raw(UNIT_PREF_FILE_PATH, sizeof(f_raw), unit_pref_validate, &f_raw, &f_rev, &f_valid);

    bool n_valid = false;
    uint8_t n_raw = (uint8_t)UNIT_PREF_CELSIUS;
    uint32_t n_rev = 0;
    if (nvs_partition_init(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            uint8_t raw = (uint8_t)UNIT_PREF_CELSIUS;
            if (hal_kv_get_u8(&h, NVS_KEY_UNIT_PREF, &raw) == HAL_OK && unit_pref_validate(&raw, 1)) {
                n_valid = true;
                n_raw = raw;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_UNIT_PREF_REV, &rev) == HAL_OK) {
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
