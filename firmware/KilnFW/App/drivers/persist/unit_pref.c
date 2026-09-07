#include "unit_pref.h"

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "unit_pref";

// Same partition zones_http.c/touch_cal_store.c use for board-wide config,
// same namespace profiles_builtin.c's hidden-mask and zones_http.c's zones
// blob use for small persisted preferences -- this is exactly that kind of
// value (a single small scalar, not a struct needing its own version/migration
// story), so it gets a key in the existing namespace rather than a new one.
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "kiln_cfg"
#define NVS_KEY_UNIT_PREF  "unit_pref"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_UNIT_PREF);

static unit_pref_t s_unit_pref = UNIT_PREF_CELSIUS;

// Brings up one NVS partition, erasing ONLY that partition if its contents
// are unusable -- copied verbatim from zones_http.c/touch_cal_store.c's
// nvs_partition_init() (same partition, same rationale: NO_FREE_PAGES /
// NEW_VERSION_FOUND have no other cure, and the erase must stay scoped to the
// partition that is actually broken).
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

esp_err_t unit_pref_start(void)
{
    s_unit_pref = UNIT_PREF_CELSIUS;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- defaulting to Celsius this boot",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return ESP_OK; // non-fatal, same convention as touch_cal_store_load()
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_NOT_FOUND) {
        // Namespace never written (fresh board, or zones/profiles wrote it
        // first but this key specifically was never set) -- Celsius is the
        // expected steady state, not an error.
        return ESP_OK;
    }
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "hal_kv_open failed: %s -- defaulting to Celsius this boot",
                 hal_status_to_name(err));
        return ESP_OK;
    }

    uint8_t raw = (uint8_t)UNIT_PREF_CELSIUS;
    err = hal_kv_get_u8(&h, NVS_KEY_UNIT_PREF, &raw);
    hal_kv_close(&h);
    if (err == HAL_NOT_FOUND) {
        return ESP_OK; // key never set -- Celsius default stands
    }
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "unit_pref read failed: %s -- defaulting to Celsius this boot", hal_status_to_name(err));
        return ESP_OK;
    }
    if (raw != (uint8_t)UNIT_PREF_CELSIUS && raw != (uint8_t)UNIT_PREF_FAHRENHEIT) {
        // Out-of-range stored byte (corruption, or a future firmware's wider
        // enum read by this older build) -- refuse it rather than trust a
        // value that isn't one of the two this build knows how to render.
        ESP_LOGW(TAG, "stored unit_pref value %u is out of range -- defaulting to Celsius", (unsigned)raw);
        return ESP_OK;
    }

    s_unit_pref = (unit_pref_t)raw;
    ESP_LOGI(TAG, "unit preference: %s", s_unit_pref == UNIT_PREF_FAHRENHEIT ? "Fahrenheit" : "Celsius");
    return ESP_OK;
}

unit_pref_t unit_pref_get(void)
{
    return s_unit_pref;
}

esp_err_t unit_pref_set(unit_pref_t pref)
{
    if (pref != UNIT_PREF_CELSIUS && pref != UNIT_PREF_FAHRENHEIT) {
        return ESP_ERR_INVALID_ARG;
    }

    // Live immediately -- the very next LCD redraw tick and the next
    // GET /api/status both need to see this, whether or not the NVS write
    // below succeeds (same "in-RAM truth first" reasoning zones_http.c's
    // zones_config_set_pid()/set_model() use).
    s_unit_pref = pref;

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- unit preference not persisted",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
        return hal_status_to_esp_err(part_err);
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "hal_kv_open failed: %s -- unit preference not persisted",
                 hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_u8(&h, NVS_KEY_UNIT_PREF, (uint8_t)pref);
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);

    if (err != HAL_OK) {
        ESP_LOGE(TAG, "could not persist unit preference: %s -- will not survive a reboot",
                 hal_status_to_name(err));
    } else {
        ESP_LOGI(TAG, "unit preference saved: %s", pref == UNIT_PREF_FAHRENHEIT ? "Fahrenheit" : "Celsius");
    }
    return hal_status_to_esp_err(err);
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
