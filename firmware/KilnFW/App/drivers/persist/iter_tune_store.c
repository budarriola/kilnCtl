#include "iter_tune_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

#include "cfg_fs.h"
#include "cfg_fs_status.h"
#include "pref_cfg_fs.h"
#include "relay_authority.h" /* relay_authority_reset_in_flight() -- no save during a factory reset */
#include "persist_scratch.h"

static const char *TAG = "iter_tune_store";

// Own partition: same "kiln_nvs" data partition every other non-safety-
// critical store in this tree uses (see kiln_cfg_store.c's header comment
// for why -- it survives an ordinary reflash untouched). NEW, INDEPENDENT
// namespace, deliberately never "adap_tune" (the old module's namespace).
#define ITER_TUNE_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_PARTITION);

#define ITER_TUNE_NVS_NAMESPACE "iter_tune"
#define ITER_TUNE_NVS_KEY_BLOB "ittblob"
#define ITER_TUNE_NVS_KEY_REV "ittrev"
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_KEY_BLOB);
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_KEY_REV);

// cfg LittleFS file: "<4-byte LE rev><raw blob>" via pref_cfg_fs.h. Since the
// dual-write window closed (docs/CONFIG_FILESYSTEM.md) this file is the ONLY
// place a save goes; the NVS copy above is read-only legacy, read once at
// boot as a fallback and migrated into the file.
/* ITER_TUNE_CFG_FILE_PATH now lives in iter_tune_store.h (kiln-scope reset names it). */

static iter_tune_store_blob_t s_blob;
static bool s_loaded;      // true once iter_tune_store_start() ran (even if it found nothing)
static uint32_t s_rev;
static bool s_schema_refused_newer;   // true if a newer-than-known version was seen and refused
static uint8_t s_schema_refused_version;

bool iter_tune_store_blob_validate(const void *bytes, size_t len) {
    if (bytes == NULL || len != sizeof(iter_tune_store_blob_t)) {
        return false;
    }
    iter_tune_store_blob_t tmp;
    memcpy(&tmp, bytes, sizeof(tmp));
    // Accept the current version, and v1 -- byte-compatible per this file's
    // header comment, migrated forward by migrate_v1_to_current() once a
    // caller has a blob it can persist back. Any other version (including
    // newer-than-current) is never trusted here; the newer-than-known case
    // is additionally reported loudly by the callers below via
    // note_schema_verdict(), which this function does not do itself since it
    // is documented PURE (no logging, no globals).
    if (tmp.version != ITER_TUNE_STORE_VERSION && tmp.version != ITER_TUNE_STORE_VERSION_V1) {
        return false;
    }
    if (tmp.zone_count > ITER_TUNE_STORE_MAX_ZONES) {
        return false;
    }
    for (uint8_t i = 0; i < tmp.zone_count; i++) {
        const iter_tune_store_zone_t *z = &tmp.zone[i];
        if (z->enabled > 1 || z->has_anchor > 1 || z->has_baseline > 1) {
            return false;
        }
    }
    return true;
}

// Not pure (logs, touches s_schema_refused_*): called by the two load paths
// below, right after a validate() failure, to distinguish "newer than this
// build knows" from ordinary corruption/truncation. Only meaningful when the
// blob is exactly the right SIZE (a truncated blob's trailing bytes,
// including a would-be version byte past the copied region, are never
// examined) -- version and zone_count are the first two bytes of every
// schema this file has ever shipped and must never move.
//
// A size-CHANGING newer version is handled separately for the NVS path by
// note_oversized_nvs_blob() (the fixed-size read fails first).
static void note_version_byte(uint8_t version) {
    if (version > ITER_TUNE_STORE_VERSION) {
        s_schema_refused_newer = true;
        s_schema_refused_version = version;
        ESP_LOGE(TAG,
                 "iter_tune store version %u is NEWER than this build knows (%u) -- refusing it and "
                 "falling back to per-zone defaults; this looks like a downgrade onto data a newer "
                 "build wrote",
                 (unsigned)version, (unsigned)ITER_TUNE_STORE_VERSION);
    }
}

static void note_schema_verdict(const void *bytes, size_t len) {
    if (bytes == NULL || len != sizeof(iter_tune_store_blob_t)) {
        return;
    }
    note_version_byte(((const uint8_t *)bytes)[0]);
}

// v1 -> v2 is byte-compatible (see this file's header comment): v1's
// reserved[0] byte, always zero, becomes v2's carry_count, also zero in
// every v1 record. Migration is therefore just re-tagging the version byte.
// Returns true if it changed anything (caller should re-persist).
static bool migrate_v1_to_current(iter_tune_store_blob_t *blob) {
    if (blob->version != ITER_TUNE_STORE_VERSION_V1) {
        return false;
    }
    ESP_LOGI(TAG, "iter_tune store v%u on disk, migrated in RAM to v%u until the next real write",
             (unsigned)ITER_TUNE_STORE_VERSION_V1, (unsigned)ITER_TUNE_STORE_VERSION);
    blob->version = ITER_TUNE_STORE_VERSION;
    return true;
}

// The stored NVS blob is a different size than this build's struct, so the
// fixed-size read above failed (HAL_INVALID_SIZE). Read the header version
// byte of the real blob (version is byte 0 of every schema, never moves) so a
// size-CHANGING newer-firmware blob is reported as "newer", not as
// corruption. Read-only: never erases or rewrites the blob, so a
// downgrade-then-upgrade keeps its tuning. A blob at version <= current with
// the wrong size stays plain corruption (no report).
#define ITER_TUNE_NVS_PROBE_MAX 4096u
static void note_oversized_nvs_blob(void) {
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        return;
    }
    size_t real_len = 0;
    if (hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, NULL, &real_len) == HAL_OK && real_len >= 1 &&
        real_len <= ITER_TUNE_NVS_PROBE_MAX) {
        uint8_t *buf = (uint8_t *)persist_scratch_alloc(real_len);
        if (buf != NULL) {
            size_t got = real_len;
            if (hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, buf, &got) == HAL_OK && got >= 1) {
                note_version_byte(buf[0]);
            }
            free(buf);
        }
    }
    hal_kv_close(&h);
}

static bool nvs_load_raw(iter_tune_store_blob_t *out, uint32_t *out_rev, bool note) {
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    hal_status_t rc = hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, out, &len);
    uint32_t rev = 0;
    hal_kv_get_u32(&h, ITER_TUNE_NVS_KEY_REV, &rev);
    hal_kv_close(&h);
    if (rc != HAL_OK) {
        if (note && rc == HAL_INVALID_SIZE) {
            note_oversized_nvs_blob();
        }
        return false;
    }
    if (!iter_tune_store_blob_validate(out, len)) {
        if (note) {
            note_schema_verdict(out, len);
        }
        return false;
    }
    *out_rev = rev;
    return true;
}

// pref_cfg_fs validator used at boot: the plain validator plus the loud
// newer-than-known report for a cfg file written by a newer build. (The
// status poll uses the plain validator so it never re-logs.)
static bool validate_and_note(const void *bytes, size_t len) {
    if (iter_tune_store_blob_validate(bytes, len)) {
        return true;
    }
    note_schema_verdict(bytes, len);
    return false;
}

esp_err_t iter_tune_store_start(void) {
    s_schema_refused_newer = false;
    s_schema_refused_version = 0;

    iter_tune_store_blob_t nvs_blob;
    memset(&nvs_blob, 0, sizeof(nvs_blob));
    uint32_t nvs_rev = 0;
    bool nvs_ok = nvs_load_raw(&nvs_blob, &nvs_rev, true);

    // Read-through (pref_cfg_fs.h): the cfg file wins on a strictly higher
    // rev, otherwise the NVS candidate stands and is migrated into the file
    // when cfg is mounted. A pre-rev legacy NVS copy reads as rev 0, so any
    // file written by this build (rev >= 1) beats it.
    iter_tune_store_blob_t resolved;
    uint32_t resolved_rev = 0;
    bool used_file = false;
    // A size-changing newer-firmware cfg file fails the length check and would
    // read as corruption -- and resolve() would then overwrite it with the NVS
    // copy. Probe first: report NEWER and keep the file untouched (NVS, if
    // valid, serves this boot without being migrated).
    bool have;
    uint8_t newer_ver = 0;
    if (pref_cfg_fs_probe_newer_wrong_size(ITER_TUNE_CFG_FILE_PATH, sizeof(nvs_blob), 0, ITER_TUNE_STORE_VERSION,
                                           &newer_ver)) {
        note_version_byte(newer_ver);
        have = nvs_ok;
        if (nvs_ok) {
            resolved = nvs_blob;
            resolved_rev = nvs_rev;
        }
    } else {
        have = pref_cfg_fs_resolve(ITER_TUNE_CFG_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_ok, nvs_rev,
                                   validate_and_note, &resolved, &resolved_rev, &used_file);
    }
    if (have) {
        s_blob = resolved;
        s_rev = resolved_rev;
    } else {
        memset(&s_blob, 0, sizeof(s_blob));
        s_blob.version = ITER_TUNE_STORE_VERSION;
        s_blob.zone_count = 0;
        s_rev = 0;
    }
    s_loaded = true;

    // Forward-migrate an old (v1) blob now that a winner has been resolved --
    // IN RAM ONLY. Deliberately does NOT re-persist here (step 7 review,
    // 2026-09-23, finding 1): re-tagging the on-disk copy to v2 on a bare
    // load, before any real v2 writer exists, would mean a rollback to v1
    // firmware after this boot reads the store as version-mismatched (v1's
    // validate() only accepts version==1) and treats every zone as
    // never-enabled -- a real, if currently low-impact, regression, since no
    // producer sets carry_count/anchor/baseline yet. Leaving the on-disk
    // bytes at v1 until iter_tune_store_set_zone() performs a REAL write
    // means a v1 rollback with no intervening write is fully lossless: NVS
    // and cfg_fs still hold a v1 blob v1 firmware can read. The on-disk
    // layout only becomes v2 the first time set_zone() persists (any write
    // always stamps s_blob.version = ITER_TUNE_STORE_VERSION, since s_blob
    // was migrated to v2 in RAM right here), at which point a v1 rollback
    // losing that specific write is expected and unavoidable -- same as any
    // other store in this tree.
    migrate_v1_to_current(&s_blob);
    return ESP_OK;
}

bool iter_tune_store_schema_refused(uint8_t *out_version) {
    if (s_schema_refused_newer && out_version != NULL) {
        *out_version = s_schema_refused_version;
    }
    return s_schema_refused_newer;
}

bool iter_tune_store_get_zone(uint8_t zone_index, iter_tune_store_zone_t *out) {
    if (!s_loaded || zone_index >= ITER_TUNE_STORE_MAX_ZONES || zone_index >= s_blob.zone_count) {
        return false;
    }
    if (out != NULL) {
        *out = s_blob.zone[zone_index];
    }
    return true;
}

esp_err_t iter_tune_store_set_zone(uint8_t zone_index, const iter_tune_store_zone_t *in) {
    if (zone_index >= ITER_TUNE_STORE_MAX_ZONES || in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        s_blob.version = ITER_TUNE_STORE_VERSION;
        s_blob.zone_count = 0;
        s_rev = 0;
        s_loaded = true;
    }
    s_blob.zone[zone_index] = *in;
    if (zone_index + 1 > s_blob.zone_count) {
        s_blob.zone_count = (uint8_t)(zone_index + 1);
    }
    // cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"):
    // a refused/failed write is returned, never masked by an NVS write. The
    // in-RAM blob keeps the new entry (live now, same contract as before);
    // the rev only advances once the write verified, so a retry reuses it.
    uint32_t new_rev = s_rev + 1;
    /* No save while a factory reset is in flight (HTTP audit L37 follow-up, MED-2): the write would land
     * on storage the reset is erasing. Narrows the window only; the erase takes no lock this path holds. */
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!relay_authority_reset_in_flight()) {
        err = pref_cfg_fs_commit(ITER_TUNE_CFG_FILE_PATH, &s_blob, sizeof(s_blob), new_rev, "iter_tune store");
    }
    if (err == ESP_OK) {
        s_rev = new_rev;
    }
    return err;
}

void iter_tune_store_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                          bool *diverged) {
    iter_tune_store_blob_t f;
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw_quiet(ITER_TUNE_CFG_FILE_PATH, sizeof(f), iter_tune_store_blob_validate, &f, &f_rev,
                               &f_valid);

    iter_tune_store_blob_t n;
    memset(&n, 0, sizeof(n));
    uint32_t n_rev = 0;
    bool n_valid = false;
    if (hal_kv_init_partition(ITER_TUNE_NVS_PARTITION) == HAL_OK) {
        n_valid = nvs_load_raw(&n, &n_rev, false);
    }
    bool content_equal = f_valid && n_valid && memcmp(&f, &n, sizeof(f)) == 0;
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

void iter_tune_store_reset_for_test(void) {
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.version = ITER_TUNE_STORE_VERSION;
    s_rev = 0;
    s_loaded = false;
    s_schema_refused_newer = false;
    s_schema_refused_version = 0;
}
