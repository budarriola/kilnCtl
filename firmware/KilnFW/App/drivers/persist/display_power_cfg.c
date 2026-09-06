#include "display_power_cfg.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "display_power_cfg";

// Same partition/namespace unit_pref.c/ramp_assist_cfg.c use for exactly
// this shape of value -- see those modules' header comments for the fuller
// rationale this one deliberately does not repeat.
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "kiln_cfg"
#define NVS_KEY_DISPLAY_POWER "display_power"

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

// Copied verbatim from unit_pref.c/ramp_assist_cfg.c's identical
// nvs_partition_init() -- same partition, same rationale, same erase-only-
// the-broken-partition scope.
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

static void apply_defaults(void)
{
    s_brightness_percent = 100;
    s_timeout_setting = DISPLAY_TIMEOUT_NEVER;
    s_keep_on_while_firing = true;
    s_display_on_error = true;
}

esp_err_t display_power_cfg_start(void)
{
    apply_defaults(); // safe defaults stand until proven otherwise below

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- display power settings stay at defaults this boot",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        return ESP_OK; // non-fatal, same convention as unit_pref_start()/ramp_assist_cfg_start()
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; // namespace never written -- defaults are the expected steady state
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open_from_partition failed: %s -- display power settings stay at defaults this boot",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    display_power_cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY_DISPLAY_POWER, &blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; // key never set -- defaults stand
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display_power_cfg read failed: %s -- defaults stay in effect this boot", esp_err_to_name(err));
        return ESP_OK;
    }
    if (len != sizeof(blob) || blob.version != DISPLAY_POWER_CFG_VERSION) {
        // Wrong size or unrecognized version (corruption, or a newer
        // firmware's wider schema read by this older build) -- refuse it
        // rather than trust a layout this build cannot vouch for. Same
        // "corrupt must resolve to the SAFE state" rule ramp_assist_cfg.c's
        // out-of-range-byte guard documents.
        ESP_LOGW(TAG, "stored display_power blob is size %u (expected %u) / version %u (expected %u) -- "
                      "defaulting", (unsigned)len, (unsigned)sizeof(blob), (unsigned)blob.version,
                 (unsigned)DISPLAY_POWER_CFG_VERSION);
        return ESP_OK;
    }
    if (blob.brightness_percent > 100 || !display_power_timeout_setting_is_valid((display_timeout_setting_t)blob.timeout_setting) ||
        (blob.keep_on_while_firing != 0 && blob.keep_on_while_firing != 1) ||
        (blob.display_on_error != 0 && blob.display_on_error != 1)) {
        // LOAD-BEARING, same reasoning as ramp_assist_cfg.c's raw!=0&&raw!=1
        // guard: a bit-flipped/garbage field must not be trusted as a valid
        // in-range value just because the blob's size and version matched.
        ESP_LOGW(TAG, "stored display_power blob has an out-of-range field -- defaulting");
        return ESP_OK;
    }

    s_brightness_percent = blob.brightness_percent;
    s_timeout_setting = (display_timeout_setting_t)blob.timeout_setting;
    s_keep_on_while_firing = (blob.keep_on_while_firing != 0);
    s_display_on_error = (blob.display_on_error != 0);
    ESP_LOGI(TAG, "display power settings loaded: brightness=%u%% timeout_setting=%u keep_on_while_firing=%s "
                  "display_on_error=%s",
             (unsigned)s_brightness_percent, (unsigned)s_timeout_setting,
             s_keep_on_while_firing ? "true" : "false", s_display_on_error ? "true" : "false");
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

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- display power settings not persisted",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        return part_err;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition failed: %s -- display power settings not persisted",
                 esp_err_to_name(err));
        return err;
    }

    display_power_cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = DISPLAY_POWER_CFG_VERSION;
    blob.brightness_percent = brightness_percent;
    blob.timeout_setting = (uint8_t)timeout_setting;
    blob.keep_on_while_firing = keep_on_while_firing ? 1 : 0;
    blob.display_on_error = display_on_error ? 1 : 0;

    err = nvs_set_blob(h, NVS_KEY_DISPLAY_POWER, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist display power settings: %s -- will not survive a reboot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "display power settings saved: brightness=%u%% timeout_setting=%u keep_on_while_firing=%s "
                      "display_on_error=%s",
                 (unsigned)brightness_percent, (unsigned)timeout_setting,
                 keep_on_while_firing ? "true" : "false", display_on_error ? "true" : "false");
    }
    return err;
}
