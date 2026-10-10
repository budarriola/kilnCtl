#include "aux_outputs_cfg.h"
#include "cfgfs_file_validators.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "MAX31856.h"
#include "cfg_fs_status.h"
#include "cfg_save_lock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ota_image_crc.h"
#include "hal_esp_common.h"
#include "hal_kv.h"
#include "kiln_io.h"
#include "nvs_key_check.h"
#include "legacy_nvs_erase.h"
#include "pref_cfg_fs.h"
#include "relay_authority.h" /* relay_authority_reset_in_flight() -- aux_outputs_cfg_set() */

static const char *TAG = "aux_outputs_cfg";

#define KILN_NVS_PARTITION  "kiln_nvs"
#define NVS_NAMESPACE       "kiln_cfg"
#define NVS_KEY_AUX_OUT     "aux_out_cfg"
#define NVS_KEY_AUX_OUT_REV "aux_out_rev"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_AUX_OUT);
NVS_KEY_LEN_CHECK(NVS_KEY_AUX_OUT_REV);


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

/* s_lock guards the RAM globals above (short critical sections only, never
 * across flash I/O or another module's calls). s_set_lock serializes whole
 * set() calls so the unlocked save in the middle cannot interleave with another
 * writer. s_lock is created in start() (idempotent); NULL (prestart) = no
 * locking, never a hang. s_set_lock is a cfg_save_lock_t (created lazily): it
 * is held across pref_cfg_fs_commit(), which waits on the flash worker, so it
 * must reserve the worker first (cfg_save_lock.h,
 * docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md "Save mutex vs. flash worker").
 * Lock order: s_set_lock -> s_lock. */
static SemaphoreHandle_t s_lock = NULL;
static cfg_save_lock_t s_set_lock = CFG_SAVE_LOCK_INIT;

static void ao_lock(SemaphoreHandle_t l)
{
    if (l) {
        (void)xSemaphoreTake(l, portMAX_DELAY);
    }
}
static void ao_unlock(SemaphoreHandle_t l)
{
    if (l) {
        (void)xSemaphoreGive(l);
    }
}

static uint32_t blob_checksum(const aux_outputs_blob_t *b)
{
    /* CRC32/IEEE (zlib): same parameters the local copy used, so the on-flash blobs stay valid. */
    return ota_image_crc32((const uint8_t *)b, offsetof(aux_outputs_blob_t, crc32));
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
bool aux_outputs_cfg_file_validate(const void *bytes, size_t len)
{
    if (len != sizeof(aux_outputs_blob_t)) {
        return false;
    }
    const aux_outputs_blob_t *b = (const aux_outputs_blob_t *)bytes;
    if (b->version == 0 || b->crc32 != blob_checksum(b)) {
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
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    /* All NVS / cfg-fs I/O below runs into locals; s_lock is taken only to publish. */
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
            if (hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, NULL, &len) == HAL_OK && len > sizeof(raw)) {
                /* Wider than any layout this build could have written: a newer
                 * schema. Quarantine rather than default, so set() can never
                 * overwrite it. */
                nvs_newer = true;
            } else if (len > 0 && len <= sizeof(raw)) {
                size_t rl = len;
                if (hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, raw, &rl) == HAL_OK && rl > 0) {
                    if (rl == sizeof(nvs_blob) && aux_outputs_cfg_file_validate(raw, rl)) {
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
    bool have_value = pref_cfg_fs_resolve_nvs_retired(AUX_OUTPUTS_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_valid, nvs_rev,
                                          aux_outputs_cfg_file_validate, &resolved, &resolved_rev, &used_file);
    if (used_file && !nvs_newer) {
        /* persfx3 MED-3: the file is authoritative; retire the frozen NVS copy so no later file loss can
         * resurrect a rule the operator has since disabled. Retried next boot on failure. */
        static const char *const k_legacy[] = {NVS_KEY_AUX_OUT, NVS_KEY_AUX_OUT_REV};
        (void)legacy_nvs_erase_keys(TAG, KILN_NVS_PARTITION, NVS_NAMESPACE, k_legacy, 2);
    }
    ao_lock(s_lock);
    apply_defaults();
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
    ao_unlock(s_lock);
    return ESP_OK;
}

bool aux_outputs_cfg_get(uint8_t relay, aux_output_t *out)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || out == NULL) {
        return false;
    }
    ao_lock(s_lock);
    const aux_output_entry_t e_copy = s_entries[relay - 1];
    const aux_output_entry_t *e = &e_copy;
    uint8_t bit = (uint8_t)(1u << (relay - 1));
    out->enabled = (s_enabled_mask & bit) != 0;
    out->conflicted = (s_conflict_mask & bit) != 0;
    ao_unlock(s_lock);
    out->tc_zone = e->tc_zone_plus1 == 0 ? (uint8_t)AUX_TC_ZONE_NONE : (uint8_t)(e->tc_zone_plus1 - 1u);
    out->hyst_c = e->hyst_c == 0.0f ? AUX_HYST_C_DEFAULT : e->hyst_c;
    out->min_on_s = e->min_on_s == 0 ? (uint16_t)AUX_MIN_ON_OFF_S_DEFAULT : e->min_on_s;
    out->min_off_s = e->min_off_s == 0 ? (uint16_t)AUX_MIN_ON_OFF_S_DEFAULT : e->min_off_s;
    return true;
}

bool aux_outputs_cfg_get_raw(uint8_t relay, aux_output_entry_t *out)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || out == NULL) {
        return false;
    }
    ao_lock(s_lock);
    *out = s_entries[relay - 1];
    ao_unlock(s_lock);
    return true;
}

bool aux_outputs_cfg_verify_persisted(void)
{
    /* Re-reads the cfg FILE, the only place a save lands since the dual-write
     * close (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"). Reading
     * NVS here would compare RAM against a copy no save ever updates again. */
    aux_outputs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    uint32_t rev = 0;
    bool valid = false;
    cfg_save_lock_take(&s_set_lock); /* no set() in flight: RAM and file are consistent */
    pref_cfg_fs_load_raw(AUX_OUTPUTS_FILE_PATH, sizeof(blob), aux_outputs_cfg_file_validate, &blob, &rev, &valid);
    ao_lock(s_lock);
    bool same = valid && rev == s_rev && blob.version == AUX_OUTPUTS_CFG_VERSION &&
                memcmp(blob.entries, s_entries, sizeof(s_entries)) == 0;
    ao_unlock(s_lock);
    cfg_save_lock_give(&s_set_lock);
    return same;
}

uint8_t aux_outputs_cfg_enabled_mask(void)
{
    ao_lock(s_lock);
    uint8_t v = s_enabled_mask;
    ao_unlock(s_lock);
    return v;
}
uint8_t aux_outputs_cfg_conflict_mask(void)
{
    ao_lock(s_lock);
    uint8_t v = s_conflict_mask;
    ao_unlock(s_lock);
    return v;
}
bool aux_outputs_cfg_conflict(void) { return aux_outputs_cfg_conflict_mask() != 0; }
bool aux_outputs_cfg_quarantined(void)
{
    ao_lock(s_lock);
    bool v = s_quarantined;
    ao_unlock(s_lock);
    return v;
}

bool aux_outputs_cfg_entry_valid(const aux_output_entry_t *entry)
{
    return entry != NULL && entry_valid(entry);
}

esp_err_t aux_outputs_cfg_set(uint8_t relay, const aux_output_entry_t *entry, uint8_t zones_relay_union)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || entry == NULL || !entry_valid(entry)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t bit = (uint8_t)(1u << (relay - 1));
    if (entry->enabled && aux_outputs_relay_conflict(zones_relay_union, bit)) {
        return ESP_ERR_INVALID_STATE;
    }

    cfg_save_lock_take(&s_set_lock); /* one writer at a time; readers are not blocked by the save */

    /* Snapshot under the short lock. The OTHER entries' persisted state is
     * carried through unchanged (a start()-time conflicted one stays persisted
     * as enabled and stays forced off in RAM until its own set()). */
    aux_outputs_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = AUX_OUTPUTS_CFG_VERSION;
    ao_lock(s_lock);
    bool quarantined = s_quarantined;
    memcpy(blob.entries, s_entries, sizeof(s_entries));
    uint32_t new_rev = s_rev + 1;
    ao_unlock(s_lock);
    if (quarantined) {
        cfg_save_lock_give(&s_set_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* HTTP audit L37 follow-up (MED-2): no save while a factory reset is in flight -- the file would be
     * written back over storage the reset is erasing. Checked under s_set_lock, just before the commit.
     * Narrows the window only (the erase does not take s_set_lock). */
    if (relay_authority_reset_in_flight()) {
        cfg_save_lock_give(&s_set_lock);
        return ESP_ERR_INVALID_STATE;
    }
    blob.entries[relay - 1] = *entry;
    blob.crc32 = blob_checksum(&blob);

    /* Save OUTSIDE s_lock (flash I/O). RAM is committed only on success: a
     * failed save leaves RAM exactly as it was (F3). cfg file ONLY --
     * docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed". */
    esp_err_t err = pref_cfg_fs_commit(AUX_OUTPUTS_FILE_PATH, &blob, sizeof(blob), new_rev, "aux outputs");
    if (err == ESP_OK) {
        ao_lock(s_lock);
        s_entries[relay - 1] = *entry;
        s_conflict_mask = (uint8_t)(s_conflict_mask & ~bit);
        if (entry->enabled) {
            s_enabled_mask |= bit;
        } else {
            s_enabled_mask = (uint8_t)(s_enabled_mask & ~bit);
        }
        s_rev = new_rev;
        ao_unlock(s_lock);
    }
    cfg_save_lock_give(&s_set_lock);
    return err;
}

#define NVS_KEY_AUX_JRNL "aux_conv_jrnl"
NVS_KEY_LEN_CHECK(NVS_KEY_AUX_JRNL);

typedef struct {
    uint8_t version;
    aux_convert_journal_t j;
    uint32_t crc32; /* over every byte before this field */
} aux_convert_journal_blob_t;

static uint32_t jrnl_checksum(const aux_convert_journal_blob_t *b)
{
    return ota_image_crc32((const uint8_t *)b, offsetof(aux_convert_journal_blob_t, crc32));
}

bool aux_convert_journal_read(aux_convert_journal_t *out)
{
    if (nvs_partition_init(KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    aux_convert_journal_blob_t b;
    memset(&b, 0, sizeof(b));
    size_t len = sizeof(b);
    bool ok = hal_kv_get_blob(&h, NVS_KEY_AUX_JRNL, &b, &len) == HAL_OK && len == sizeof(b) && b.version == 1 &&
              b.crc32 == jrnl_checksum(&b);
    hal_kv_close(&h);
    if (ok && out != NULL) {
        *out = b.j;
    }
    return ok;
}

bool aux_convert_journal_write(const aux_convert_journal_t *j)
{
    /* Factory-reset writer fence: do not lazily re-init and write kiln_nvs after the reset erased it. */
    if (relay_authority_reset_refuses_writer()) {
        return false;
    }
    if (j == NULL || nvs_partition_init(KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    aux_convert_journal_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = 1;
    b.j = *j;
    b.crc32 = jrnl_checksum(&b);
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    hal_status_t err = hal_kv_set_blob(&h, NVS_KEY_AUX_JRNL, &b, sizeof(b));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return false;
    }
    aux_convert_journal_t back;
    return aux_convert_journal_read(&back) && memcmp(&back, j, sizeof(back)) == 0;
}

bool aux_convert_journal_clear(void)
{
    if (nvs_partition_init(KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    hal_status_t err = hal_kv_erase_key(&h, NVS_KEY_AUX_JRNL);
    if (err == HAL_OK || err == HAL_NOT_FOUND) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err == HAL_OK && !aux_convert_journal_read(NULL);
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
    pref_cfg_fs_load_raw(AUX_OUTPUTS_FILE_PATH, sizeof(f_blob), aux_outputs_cfg_file_validate, &f_blob, &f_rev, &f_valid);

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
            if (hal_kv_get_blob(&h, NVS_KEY_AUX_OUT, &blob, &len) == HAL_OK && aux_outputs_cfg_file_validate(&blob, len)) {
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
