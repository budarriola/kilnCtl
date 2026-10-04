#include "aux_outputs_cfg.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "MAX31856.h"
#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "kiln_io.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"

static const char *TAG = "aux_outputs_cfg";

#define KILN_NVS_PARTITION  "kiln_nvs"
#define NVS_NAMESPACE       "kiln_cfg"
#define NVS_KEY_AUX_OUT     "aux_out_cfg"
#define NVS_KEY_AUX_OUT_REV "aux_out_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_AUX_OUT);
NVS_KEY_LEN_CHECK(NVS_KEY_AUX_OUT_REV);

#define AUX_OUTPUTS_CFG_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    aux_output_entry_t entries[AUX_OUTPUTS_COUNT];
    uint32_t crc32; /* over every byte before this field */
} aux_outputs_blob_t;

_Static_assert(AUX_OUTPUTS_COUNT == KILN_IO_RELAY_COUNT, "one aux entry per relay");
_Static_assert(sizeof(aux_output_entry_t) == 12, "aux entry layout is on-flash");
_Static_assert(sizeof(aux_outputs_blob_t) == 56, "aux blob layout is on-flash");
_Static_assert(sizeof(aux_outputs_blob_t) <= PREF_CFG_FS_MAX_ITEM, "aux blob must fit pref_cfg_fs");

/* RAM state. s_entries is the PERSISTED view (what set() writes back);
 * s_enabled_mask is the EFFECTIVE view (conflict/quarantine applied). */
static aux_output_entry_t s_entries[AUX_OUTPUTS_COUNT];
static uint8_t s_enabled_mask = 0;
static uint8_t s_conflict_mask = 0;
static bool s_quarantined = false;
static uint32_t s_rev = 0;

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t blob_crc(const aux_outputs_blob_t *b)
{
    return crc32_ieee((const uint8_t *)b, offsetof(aux_outputs_blob_t, crc32));
}

static bool entry_valid(const aux_output_entry_t *e)
{
    if (e->enabled > 1 || e->reserved != 0) {
        return false;
    }
    if (e->tc_zone_plus1 > MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (!isfinite(e->hyst_c) || e->hyst_c < 0.0f || e->hyst_c > AUX_HYST_C_MAX ||
        (e->hyst_c != 0.0f && e->hyst_c < AUX_HYST_C_MIN)) {
        return false;
    }
    if (e->min_on_s > AUX_MIN_ON_OFF_S_MAX || (e->min_on_s != 0 && e->min_on_s < AUX_MIN_ON_OFF_S_MIN)) {
        return false;
    }
    if (e->min_off_s > AUX_MIN_ON_OFF_S_MAX || (e->min_off_s != 0 && e->min_off_s < AUX_MIN_ON_OFF_S_MIN)) {
        return false;
    }
    return true;
}

/* pref_cfg_fs validator: exact size + CRC; a CURRENT-version blob also needs
 * every entry in range. A NEWER version passes (its entries are not ours to
 * judge) so start() can quarantine it instead of silently defaulting. */
static bool aux_validate(const void *bytes, size_t len)
{
    if (len != sizeof(aux_outputs_blob_t)) {
        return false;
    }
    const aux_outputs_blob_t *b = (const aux_outputs_blob_t *)bytes;
    if (b->version == 0 || b->crc32 != blob_crc(b)) {
        return false;
    }
    if (b->version > AUX_OUTPUTS_CFG_VERSION) {
        return true;
    }
    for (unsigned i = 0; i < AUX_OUTPUTS_COUNT; i++) {
        if (!entry_valid(&b->entries[i])) {
            return false;
        }
    }
    return true;
}

static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

static void apply_defaults(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_enabled_mask = 0;
    s_conflict_mask = 0;
    s_quarantined = false;
    s_rev = 0;
}

esp_err_t aux_outputs_cfg_start(uint8_t zones_relay_union)
{
    apply_defaults();

    aux_outputs_blob_t nvs_blob;
    memset(&nvs_blob, 0, sizeof(nvs_blob));
    bool nvs_valid = false;
    uint32_t nvs_rev = 0;
    bool nvs_newer = false; /* NVS holds a blob of a version this build does not know */

    if (nvs_partition_init(KILN_NVS_PARTITION) != HAL_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed -- aux outputs stay disabled this boot", KILN_NVS_PARTITION);
    } else {
        hal_kv_handle_t h;
        hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
        if (err == HAL_OK) {
            uint8_t raw[PREF_CFG_FS_MAX_ITEM * 2];
            size_t len = 0;
            /* Probe the size first: a get_blob into a too-small buffer fails
             * (HAL_INVALID_SIZE) and would hide a wider, newer-version blob. */
            if (hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, NULL, &len) == HAL_OK && len > 0 && len <= sizeof(raw)) {
                size_t rl = len;
                if (hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, raw, &rl) == HAL_OK && rl > 0) {
                    if (rl == sizeof(nvs_blob) && aux_validate(raw, rl)) {
                        memcpy(&nvs_blob, raw, sizeof(nvs_blob));
                        nvs_valid = true;
                    } else if (raw[0] > AUX_OUTPUTS_CFG_VERSION) {
                        nvs_newer = true; /* wider/newer schema: quarantine, never default over it */
                    } else {
                        ESP_LOGW(TAG, "stored aux_out_cfg blob is invalid (size %u) -- defaulting", (unsigned)rl);
                    }
                }
            }
            if (nvs_valid) {
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_AUX_OUT_REV, &rev) == HAL_OK) {
                    nvs_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
    }

    aux_outputs_blob_t resolved = nvs_blob;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(AUX_OUTPUTS_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_valid, nvs_rev,
                                          aux_validate, &resolved, &resolved_rev, &used_file);
    if (nvs_newer) {
        s_quarantined = true;
    } else if (have_value) {
        if (resolved.version > AUX_OUTPUTS_CFG_VERSION) {
            s_quarantined = true;
        } else {
            memcpy(s_entries, resolved.entries, sizeof(s_entries));
            s_rev = resolved_rev;
        }
    }

    if (s_quarantined) {
        ESP_LOGW(TAG, "aux outputs store is a newer version -- quarantined, all aux disabled and never overwritten");
    } else {
        uint8_t want = 0;
        for (unsigned i = 0; i < AUX_OUTPUTS_COUNT; i++) {
            if (s_entries[i].enabled) {
                want |= (uint8_t)(1u << i);
            }
        }
        s_conflict_mask = aux_outputs_relay_conflict_mask(zones_relay_union, want);
        s_enabled_mask = (uint8_t)(want & ~s_conflict_mask);
        if (s_conflict_mask != 0) {
            ESP_LOGE(TAG, "aux/zone relay conflict mask 0x%02x -- aux forced disabled in RAM, zone untouched",
                     (unsigned)s_conflict_mask);
        }
    }
    return ESP_OK;
}

bool aux_outputs_cfg_get(uint8_t relay, aux_output_t *out)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || out == NULL) {
        return false;
    }
    const aux_output_entry_t *e = &s_entries[relay - 1];
    uint8_t bit = (uint8_t)(1u << (relay - 1));
    out->enabled = (s_enabled_mask & bit) != 0;
    out->conflicted = (s_conflict_mask & bit) != 0;
    out->tc_zone = e->tc_zone_plus1 == 0 ? (uint8_t)AUX_TC_ZONE_NONE : (uint8_t)(e->tc_zone_plus1 - 1u);
    out->hyst_c = e->hyst_c == 0.0f ? AUX_HYST_C_DEFAULT : e->hyst_c;
    out->min_on_s = e->min_on_s == 0 ? (uint16_t)AUX_MIN_ON_OFF_S_DEFAULT : e->min_on_s;
    out->min_off_s = e->min_off_s == 0 ? (uint16_t)AUX_MIN_ON_OFF_S_DEFAULT : e->min_off_s;
    return true;
}

uint8_t aux_outputs_cfg_enabled_mask(void) { return s_enabled_mask; }
uint8_t aux_outputs_cfg_conflict_mask(void) { return s_conflict_mask; }
bool aux_outputs_cfg_conflict(void) { return s_conflict_mask != 0; }
bool aux_outputs_cfg_quarantined(void) { return s_quarantined; }

esp_err_t aux_outputs_cfg_set(uint8_t relay, const aux_output_entry_t *entry, uint8_t zones_relay_union)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || entry == NULL || !entry_valid(entry)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_quarantined) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bit = (uint8_t)(1u << (relay - 1));
    if (entry->enabled && aux_outputs_relay_conflict(zones_relay_union, bit)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The OTHER entries' persisted state is carried through unchanged (a
     * start()-time conflicted one stays persisted as enabled and stays forced
     * off in RAM until its own set()). */
    aux_outputs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = AUX_OUTPUTS_CFG_VERSION;
    memcpy(blob.entries, s_entries, sizeof(s_entries));
    blob.entries[relay - 1] = *entry;
    blob.crc32 = blob_crc(&blob);

    /* In-RAM truth first. */
    s_entries[relay - 1] = *entry;
    s_conflict_mask = (uint8_t)(s_conflict_mask & ~bit);
    if (entry->enabled) {
        s_enabled_mask |= bit;
    } else {
        s_enabled_mask = (uint8_t)(s_enabled_mask & ~bit);
    }

    uint32_t new_rev = s_rev + 1;
    esp_err_t file_err = pref_cfg_fs_save(AUX_OUTPUTS_FILE_PATH, &blob, sizeof(blob), new_rev);
    if (file_err != ESP_OK && file_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "aux outputs file write failed: %s -- NVS remains the source of truth",
                 esp_err_to_name(file_err));
    }

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        return hal_status_to_esp_err(part_err);
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_AUX_OUT, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_AUX_OUT_REV, new_rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "could not persist aux outputs: %s -- will not survive a reboot", hal_status_to_name(err));
    } else {
        s_rev = new_rev;
    }
    return hal_status_to_esp_err(err);
}

void aux_outputs_cfg_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                          uint32_t *nvs_rev, bool *diverged)
{
    if (file_valid) *file_valid = false;
    if (file_rev) *file_rev = 0;
    if (nvs_valid) *nvs_valid = false;
    if (nvs_rev) *nvs_rev = 0;
    if (diverged) *diverged = false;

    aux_outputs_blob_t f_blob;
    memset(&f_blob, 0, sizeof(f_blob));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(AUX_OUTPUTS_FILE_PATH, sizeof(f_blob), aux_validate, &f_blob, &f_rev, &f_valid);

    bool n_valid = false;
    aux_outputs_blob_t n_blob;
    memset(&n_blob, 0, sizeof(n_blob));
    uint32_t n_rev = 0;
    if (nvs_partition_init(KILN_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
            aux_outputs_blob_t blob;
            memset(&blob, 0, sizeof(blob));
            size_t len = sizeof(blob);
            if (hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, &blob, &len) == HAL_OK && aux_validate(&blob, len)) {
                n_valid = true;
                n_blob = blob;
                uint32_t rev = 0;
                if (hal_kv_get_u32(&h, NVS_KEY_AUX_OUT_REV, &rev) == HAL_OK) {
                    n_rev = rev;
                }
            }
            hal_kv_close(&h);
        }
    }
    bool content_equal = f_valid && n_valid && (memcmp(&f_blob, &n_blob, sizeof(f_blob)) == 0);
    if (file_valid) *file_valid = f_valid;
    if (file_rev) *file_rev = f_rev;
    if (nvs_valid) *nvs_valid = n_valid;
    if (nvs_rev) *nvs_rev = n_rev;
    if (diverged) *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
}
