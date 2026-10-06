#include "display_power_cfg.h"

#include <string.h>

#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"

static const char *TAG = "display_power_cfg";

// Same partition/namespace unit_pref.c/ramp_assist_cfg.c use for exactly
// this shape of value -- see those modules' header comments for the fuller
// rationale this one deliberately does not repeat.
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "kiln_cfg"
#define NVS_KEY_DISPLAY_POWER "display_power"
// rev counter for the cfg-filesystem dual-write below (docs/FILESYSTEM_USER_DATA_PLAN.md
// section 5 step 3) -- see unit_pref.c's identical NVS_KEY_UNIT_PREF_REV for
// why this is a separate key rather than a field on the stored blob.
#define NVS_KEY_DISPLAY_POWER_REV "disp_pow_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_DISPLAY_POWER);
NVS_KEY_LEN_CHECK(NVS_KEY_DISPLAY_POWER_REV);

// docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 3 (prefs move): the file
// this preference dual-writes to on the `cfg` LittleFS partition, once
// mounted -- see pref_cfg_fs.h for the read-through/dual-write/tie-break
// policy this module hands its NVS candidate to.
/* DISPLAY_POWER_FILE_PATH now lives in display_power_cfg.h (kiln-scope reset names it). */

// Versioned blob rather than four loose keys -- see display_power_cfg.h's
// PERSISTENCE note. version bumps only if a field is ever added/reinterpreted;
// a stored blob whose version this build does not recognize is treated
// exactly like a missing key (falls back to defaults), never partially
// trusted.
#define DISPLAY_POWER_CFG_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t brightness_percent;
    uint8_t timeout_setting; // display_timeout_setting_t, stored narrow
    uint8_t keep_on_while_firing; // 0/1
    uint8_t display_on_error;     // 0/1
} display_power_cfg_blob_t;

// SAFE DEFAULTS -- see display_power_cfg.h's header comment for why each one
// is the safe direction (matches today's shipped no-blank/full-brightness
// behaviour until the owner opts in).
static uint8_t s_brightness_percent = 100;
static display_timeout_setting_t s_timeout_setting = DISPLAY_TIMEOUT_NEVER;
static bool s_keep_on_while_firing = true;
static bool s_display_on_error = true;
static uint32_t s_display_power_rev = 0;

// Copied verbatim from unit_pref.c/ramp_assist_cfg.c's identical
// nvs_partition_init() -- same partition, same rationale, same erase-only-
// the-broken-partition scope. HAL Phase 3 item 3 (hal_kv migration):
// hal_kv_init_partition() already implements this erase-and-retry idiom.
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

static void apply_defaults(void)
{
    s_brightness_percent = 100;
    s_timeout_setting = DISPLAY_TIMEOUT_NEVER;
    s_keep_on_while_firing = true;
    s_display_on_error = true;
}

// Same size/version/field-range checks display_power_cfg_start() has always
// applied to a stored NVS blob -- reused verbatim as the
// pref_cfg_fs_validate_fn_t for the file, so the moved item is validated
// exactly as its NVS path validates today.
static bool display_power_validate(const void *bytes, size_t len)
{
    if (len != sizeof(display_power_cfg_blob_t)) {
        return false;
    }
    const display_power_cfg_blob_t *blob = (const display_power_cfg_blob_t *)bytes;
    if (blob->version != DISPLAY_POWER_CFG_VERSION) {
        return false;
    }
    if (blob->brightness_percent > 100 ||
        !display_power_timeout_setting_is_valid((display_timeout_setting_t)blob->timeout_setting) ||
        (blob->keep_on_while_firing != 0 && blob->keep_on_while_firing != 1) ||
        (blob->display_on_error != 0 && blob->display_on_error != 1)) {
        return false;
    }
    return true;
}

esp_err_t display_power_cfg_start(void)
{
    apply_defaults(); // safe defaults stand until proven otherwise below
    s_display_power_rev = 0;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- display power settings stay at defaults this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_OK; // non-fatal, same convention as unit_pref_start()/ramp_assist_cfg_start()
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    bool nvs_valid = false;
    display_power_cfg_blob_t nvs_blob;
    memset(&nvs_blob, 0, sizeof(nvs_blob));
    uint32_t nvs_rev = 0;
    if (err == HAL_NOT_FOUND) {
        // namespace never written -- defaults are the expected steady state
    } else if (err != HAL_OK) {
        ESP_LOGW(TAG, "nvs_open_from_partition failed: %s -- display power settings stay at defaults this boot",
                 hal_status_to_name(err));
    } else {
        display_power_cfg_blob_t blob;
        memset(&blob, 0, sizeof(blob));
        size_t len = sizeof(blob);
        hal_status_t rerr = hal_kv_get_blob(&h, NVS_KEY_DISPLAY_POWER, &blob, &len);
        if (rerr == HAL_OK) {
            if (display_power_validate(&blob, len)) {
                nvs_valid = true;
                nvs_blob = blob;
            } else {
                // Wrong size or unrecognized version (corruption, or a newer
                // firmware's wider schema read by this older build), or an
                // out-of-range field inside an otherwise well-formed blob --
                // refuse it rather than trust a layout/value this build
                // cannot vouch for. Same "corrupt must resolve to the SAFE
                // state" rule ramp_assist_cfg.c's out-of-range-byte guard
                // documents.
                ESP_LOGW(TAG, "stored display_power blob is size %u (expected %u) / version %u -- defaulting",
                         (unsigned)len, (unsigned)sizeof(blob), (unsigned)blob.version);
            }
        } else if (rerr != HAL_NOT_FOUND) {
            ESP_LOGW(TAG, "display_power_cfg read failed: %s -- defaults stay in effect this boot",
                     hal_status_to_name(rerr));
        }
        if (nvs_valid) {
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, NVS_KEY_DISPLAY_POWER_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    display_power_cfg_blob_t resolved = nvs_blob;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(DISPLAY_POWER_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_valid, nvs_rev,
                                           display_power_validate, &resolved, &resolved_rev, &used_file);
    if (!have_value) {
        return ESP_OK; // neither side had anything trustworthy -- defaults stand
    }

    s_brightness_percent = resolved.brightness_percent;
    s_timeout_setting = (display_timeout_setting_t)resolved.timeout_setting;
    s_keep_on_while_firing = (resolved.keep_on_while_firing != 0);
    s_display_on_error = (resolved.display_on_error != 0);
    s_display_power_rev = resolved_rev;
    ESP_LOGI(TAG,
             "display power settings loaded (source=%s, rev=%lu): brightness=%u%% timeout_setting=%u "
             "keep_on_while_firing=%s display_on_error=%s",
             used_file ? "file" : "NVS", (unsigned long)s_display_power_rev, (unsigned)s_brightness_percent,
             (unsigned)s_timeout_setting, s_keep_on_while_firing ? "true" : "false",
             s_display_on_error ? "true" : "false");
    return ESP_OK;
}

uint8_t display_power_cfg_brightness_percent(void) { return s_brightness_percent; }
display_timeout_setting_t display_power_cfg_timeout_setting(void) { return s_timeout_setting; }
bool display_power_cfg_keep_on_while_firing(void) { return s_keep_on_while_firing; }
bool display_power_cfg_display_on_error(void) { return s_display_on_error; }

esp_err_t display_power_cfg_set(uint8_t brightness_percent, display_timeout_setting_t timeout_setting,
                                bool keep_on_while_firing, bool display_on_error)
{
    if (brightness_percent > 100 || !display_power_timeout_setting_is_valid(timeout_setting)) {
        // Refuse outright -- never clamp/truncate, same discipline as
        // settings_http.c's other POST handlers (e.g. the tz handler this
        // module's own HTTP wiring in settings_http.c mirrors).
        return ESP_ERR_INVALID_ARG;
    }

    // In-RAM truth first -- live for the very next display_power_policy_step()
    // caller regardless of whether the NVS write below succeeds, same
    // ordering as unit_pref_set()/ramp_assist_cfg_set_enabled().
    s_brightness_percent = brightness_percent;
    s_timeout_setting = timeout_setting;
    s_keep_on_while_firing = keep_on_while_firing;
    s_display_on_error = display_on_error;

    uint32_t new_rev = s_display_power_rev + 1;
    display_power_cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = DISPLAY_POWER_CFG_VERSION;
    blob.brightness_percent = brightness_percent;
    blob.timeout_setting = (uint8_t)timeout_setting;
    blob.keep_on_while_firing = keep_on_while_firing ? 1 : 0;
    blob.display_on_error = display_on_error ? 1 : 0;

    // cfg file ONLY -- see unit_pref_set() and docs/CONFIG_FILESYSTEM.md.
    esp_err_t err = pref_cfg_fs_commit(DISPLAY_POWER_FILE_PATH, &blob, sizeof(blob), new_rev, "display power settings");
    if (err == ESP_OK) {
        s_display_power_rev = new_rev;
        ESP_LOGI(TAG, "display power settings saved: brightness=%u%% timeout_setting=%u keep_on_while_firing=%s "
                      "display_on_error=%s",
                 (unsigned)brightness_percent, (unsigned)timeout_setting,
                 keep_on_while_firing ? "true" : "false", display_on_error ? "true" : "false");
    }
    return err;
}

void display_power_cfg_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                             uint32_t *nvs_rev, bool *diverged)
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

    display_power_cfg_blob_t f_blob;
    memset(&f_blob, 0, sizeof(f_blob));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(DISPLAY_POWER_FILE_PATH, sizeof(f_blob), display_power_validate, &f_blob, &f_rev, &f_valid);

    bool n_valid = false;
    display_power_cfg_blob_t n_blob;
    memset(&n_blob, 0, sizeof(n_blob));
    uint32_t n_rev = 0;
    if (nvs_partition_init(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            display_power_cfg_blob_t blob;
            memset(&blob, 0, sizeof(blob));
            size_t len = sizeof(blob);
            if (hal_kv_get_blob(&h, NVS_KEY_DISPLAY_POWER, &blob, &len) == HAL_OK &&
                display_power_validate(&blob, len)) {
                n_valid = true;
                n_blob = blob;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_DISPLAY_POWER_REV, &rev) == HAL_OK) {
                    n_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
    }

    bool content_equal = f_valid && n_valid && (memcmp(&f_blob, &n_blob, sizeof(f_blob)) == 0);
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
