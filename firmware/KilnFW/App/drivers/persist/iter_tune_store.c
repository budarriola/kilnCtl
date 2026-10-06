#include "iter_tune_store.h"

#include <string.h>

#include "esp_log.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

#include "cfg_fs.h"

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

// cfg LittleFS dual-write file, same "<4-byte LE rev><raw blob>" shape as
// zones_config_cfg_fs.c/kiln_cfg_store_cfg_fs.c.
/* ITER_TUNE_CFG_FILE_PATH now lives in iter_tune_store.h (kiln-scope reset names it). */
#define ITER_TUNE_FILE_BUF_MAX (4 + sizeof(iter_tune_store_blob_t))

static iter_tune_store_blob_t s_blob;
static bool s_loaded;      // true once iter_tune_store_start() ran (even if it found nothing)
static uint32_t s_rev;
static bool s_schema_refused_newer;   // true if a newer-than-known version was seen and refused
static uint8_t s_schema_refused_version;

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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
// KNOWN LIMITATION (step 7 review, 2026-09-23, finding 3): a future
// size-CHANGING version (e.g. a v3 whose blob is a different length than
// today's 100 bytes) is NEVER reported by this function -- the caller's own
// length check (hal_kv_get_blob's `len` out-param for NVS, or the cfg_fs
// file-length check) already rejects it before note_schema_verdict() is even
// reached with a size that could match `sizeof(iter_tune_store_blob_t)`.
// Such a boot instead falls silently into the ordinary "nothing persisted"
// bucket, exactly like plain corruption, rather than getting the loud
// newer-than-known banner. This is accepted for now (test_larger_blob_size_change_not_reported_current_limitation()
// in test_iter_tune_store.c locks in and documents this exact gap) rather
// than adding a NULL/small-buffer NVS size-probe read ahead of any real v3;
// see docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 0.1's iter_tune row and
// docs/ITER_TUNE_REDESIGN_PLAN.md step 7 for the same note. Revisit this
// when a size-changing version is actually designed.
static void note_schema_verdict(const void *bytes, size_t len) {
    if (bytes == NULL || len != sizeof(iter_tune_store_blob_t)) {
        return;
    }
    uint8_t version = ((const uint8_t *)bytes)[0];
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

static bool nvs_load_raw(iter_tune_store_blob_t *out, uint32_t *out_rev) {
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
        return false;
    }
    if (!iter_tune_store_blob_validate(out, len)) {
        note_schema_verdict(out, len);
        return false;
    }
    *out_rev = rev;
    return true;
}

static bool nvs_save_raw(const iter_tune_store_blob_t *in, uint32_t rev) {
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return false;
    }
    bool ok = hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, in, sizeof(*in)) == HAL_OK;
    ok = ok && hal_kv_set_u32(&h, ITER_TUNE_NVS_KEY_REV, rev) == HAL_OK;
    ok = ok && hal_kv_commit(&h) == HAL_OK;
    hal_kv_close(&h);
    if (!ok) {
        ESP_LOGE(TAG, "nvs write failed");
    }
    return ok;
}

// File wins ONLY on a strictly higher rev than NVS -- same tie-break
// convention as kiln_cfg_store_cfg_fs_resolve()/zones_config_cfg_fs.c.
static bool cfg_fs_load_raw(iter_tune_store_blob_t *out, uint32_t *out_rev) {
    if (!cfg_fs_is_available()) {
        return false;
    }
    uint8_t buf[ITER_TUNE_FILE_BUF_MAX];
    size_t out_len = 0;
    if (cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, buf, sizeof(buf), &out_len) != ESP_OK) {
        return false;
    }
    if (out_len != 4 + sizeof(iter_tune_store_blob_t)) {
        return false;
    }
    uint32_t rev = get_u32_le(buf);
    if (!iter_tune_store_blob_validate(buf + 4, out_len - 4)) {
        note_schema_verdict(buf + 4, out_len - 4);
        return false;
    }
    memcpy(out, buf + 4, sizeof(*out));
    *out_rev = rev;
    return true;
}

/* Intentional silent no-op when cfg is not mounted (owner decision
 * 2026-10-06, docs/CONFIG_FILESYSTEM.md): unlike the HTTP save routes, which
 * refuse with 503 and CFG_FS_NOT_MOUNTED_TEXT, this store is written from the
 * autotune/iteration path with no request to answer, and NVS (above) still
 * holds the write, so there is nobody to surface an error to and no data is
 * lost. The not-mounted state is already visible on /api/readiness
 * ("cfg_fs" item). Do not "fix" this into an error return. */
static void cfg_fs_save_raw(const iter_tune_store_blob_t *in, uint32_t rev) {
    if (!cfg_fs_is_available()) {
        return;
    }
    uint8_t buf[ITER_TUNE_FILE_BUF_MAX];
    put_u32_le(buf, rev);
    memcpy(buf + 4, in, sizeof(*in));
    esp_err_t err = cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, buf, sizeof(buf));
    if (err != ESP_OK) {
        // Non-fatal: NVS is authoritative and already holds the write.
        ESP_LOGE(TAG, "cfg_fs dual-write failed: %d", (int)err);
    }
}

esp_err_t iter_tune_store_start(void) {
    s_schema_refused_newer = false;
    s_schema_refused_version = 0;

    iter_tune_store_blob_t nvs_blob;
    uint32_t nvs_rev = 0;
    bool nvs_ok = nvs_load_raw(&nvs_blob, &nvs_rev);

    iter_tune_store_blob_t file_blob;
    uint32_t file_rev = 0;
    bool file_ok = cfg_fs_load_raw(&file_blob, &file_rev);

    // Log a rev disagreement the same way zones_config_cfg_fs.c/
    // kiln_cfg_store_cfg_fs.c do (step 7 review, 2026-09-23, advisory A5) --
    // both sides being present but not agreeing is worth a boot-time
    // breadcrumb even though the tie-break below resolves it safely either
    // way.
    if (file_ok && nvs_ok && file_rev != nvs_rev) {
        ESP_LOGW(TAG, "iter_tune file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting %s (strictly higher rev wins)",
                 (unsigned long)file_rev, (unsigned long)nvs_rev, (file_rev > nvs_rev) ? "FILE" : "NVS");
    }

    // Note (step 7 review, 2026-09-23, advisory finding 5): `nvs_ok` is also
    // false when NVS held a blob that validate() rejected as newer-than-known
    // (note_schema_verdict() above already flagged s_schema_refused_newer in
    // that case) -- this branch does not distinguish that from an ordinary
    // missing/corrupt NVS entry, so a valid older FILE still wins here and
    // gets written back into NVS via nvs_save_raw() below, clobbering the
    // refused-newer NVS blob. No behavior change: this is the same
    // file-wins-and-catches-NVS-up rule as any other disagreement, just
    // worth naming since a "refused newer" NVS blob is otherwise a boot
    // condition worth being deliberate about overwriting.
    if (file_ok && (!nvs_ok || file_rev > nvs_rev)) {
        s_blob = file_blob;
        s_rev = file_rev;
        // File was ahead of NVS -- catch NVS up so both sides agree going
        // forward (same repair-on-read convention kiln_cfg_store_cfg_fs
        // uses).
        nvs_save_raw(&s_blob, s_rev);
    } else if (nvs_ok) {
        s_blob = nvs_blob;
        s_rev = nvs_rev;
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
    s_rev++;
    bool nvs_ok = nvs_save_raw(&s_blob, s_rev);
    cfg_fs_save_raw(&s_blob, s_rev);
    return nvs_ok ? ESP_OK : ESP_FAIL;
}

void iter_tune_store_reset_for_test(void) {
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.version = ITER_TUNE_STORE_VERSION;
    s_rev = 0;
    s_loaded = false;
    s_schema_refused_newer = false;
    s_schema_refused_version = 0;
}
