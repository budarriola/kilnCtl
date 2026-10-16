// See kiln_cfg_store_cfg_fs.h for the full design/rationale.
#include "kiln_cfg_store_cfg_fs.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"
#include "persist_scratch.h"

static const char *KCFG_FS_TAG = "kiln_cfg_store_fs";

static kiln_cfg_store_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;

void kiln_cfg_store_cfg_fs_set_write_fn(kiln_cfg_store_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void kiln_cfg_store_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

kiln_cfg_store_cfg_fs_write_fn_t kiln_cfg_store_cfg_fs_get_write_fn(void)
{
    return s_write_fn;
}

/* rev(4 bytes LE) + a byte-for-byte kiln_cfg_store_blob_t. Several KB
 * (KILN_CFG_MAX_COUNT slots x a full zones-config blob each) -- every buffer
 * of this size in this module is heap-allocated, never a stack local. See
 * this file's header comment ("STACK") for why. */
#define KCFG_FILE_BUF_MAX (4 + sizeof(kiln_cfg_store_blob_t))

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* persfx4 MED-3: keep a copy of an undecodable kiln_configs file as "<path>.bad" before anything can replace it.
 * An identical existing copy is not rewritten. Returns whether a copy of these bytes now exists. */
/* PS-02: whether the most recent .bad preserve attempt failed, so the quarantine reason never claims a copy exists. */
static bool s_last_preserve_failed;

bool kiln_cfg_store_cfg_fs_last_preserve_failed(void)
{
    return s_last_preserve_failed;
}

static bool kcfg_preserve_rejected_impl(const uint8_t *raw, size_t len);

static bool kcfg_preserve_rejected(const uint8_t *raw, size_t len)
{
    bool ok = kcfg_preserve_rejected_impl(raw, len);
    s_last_preserve_failed = !ok;
    return ok;
}

static bool kcfg_preserve_rejected_impl(const uint8_t *raw, size_t len)
{
    static const char kBad[] = KILN_CFG_STORE_FILE_PATH ".bad";
    if (!raw || len == 0) {
        return false;
    }
    uint8_t *old = persist_scratch_alloc(len);
    if (old) {
        size_t olen = 0;
        bool same = cfg_fs_read(kBad, old, len, &olen) == ESP_OK && olen == len && memcmp(old, raw, len) == 0;
        free(old);
        if (same) {
            return true;
        }
    }
    esp_err_t err = s_write_fn(kBad, raw, len);
    if (err == ESP_OK) {
        ESP_LOGE(KCFG_FS_TAG, "rejected kiln config store file preserved as %s (%u bytes)", kBad, (unsigned)len);
    } else {
        ESP_LOGE(KCFG_FS_TAG, "could not preserve rejected kiln config store file as %s: %s", kBad,
                 esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void kiln_cfg_store_cfg_fs_load_raw_ex(kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_valid,
                                       kiln_cfg_file_state_t *out_state, bool preserve)
{
    if (out_blob) {
        memset(out_blob, 0, sizeof(*out_blob));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (out_state) {
        *out_state = KILN_CFG_FILE_ABSENT;
    }
    if (!out_blob || !out_rev || !out_valid) {
        return;
    }
    s_last_preserve_failed = false;
    if (!cfg_fs_is_available()) {
        return;
    }

    uint8_t *raw = persist_scratch_alloc(KCFG_FILE_BUF_MAX);
    if (!raw) {
        /* persfx4 MED-3: an allocation failure is NOT "file absent" -- the file was never looked at. */
        ESP_LOGE(KCFG_FS_TAG, "file read buffer alloc failed (%u bytes) -- file state UNKNOWN",
                 (unsigned)KCFG_FILE_BUF_MAX);
        if (out_state) {
            *out_state = KILN_CFG_FILE_UNKNOWN;
        }
        return;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(KILN_CFG_STORE_FILE_PATH, raw, KCFG_FILE_BUF_MAX, &len);
    if (err != ESP_OK) {
        free(raw);
        if (err == ESP_ERR_NOT_FOUND) {
            return; /* never saved/migrated: the only real absence */
        }
        if (out_state) {
            /* ESP_ERR_INVALID_SIZE: bigger than any store this build writes, so newer firmware wrote it. Any
             * other error is an unreadable file: state unknown, never treated as absent. */
            *out_state = (err == ESP_ERR_INVALID_SIZE) ? KILN_CFG_FILE_NEWER : KILN_CFG_FILE_UNKNOWN;
        }
        ESP_LOGE(KCFG_FS_TAG, "kiln config store file read failed: %s -- file state %s", esp_err_to_name(err),
                 err == ESP_ERR_INVALID_SIZE ? "NEWER (oversized)" : "UNKNOWN");
        return;
    }
    if (len != KCFG_FILE_BUF_MAX) {
        /* A size other than "rev + one current blob" is never valid as-is. persmed MED-C: if the version byte
         * says it was written by an OLDER layout, it is migratable (kiln_cfg_store.c owns the frozen v1/v2
         * layouts and converts it), not corrupt -- the next version bump must not lock the store. Anything
         * else is REJECTED: preserved, never replaced. */
        if (len >= 5 && raw[4] >= 1 && raw[4] < KILN_CFG_STORE_VERSION) {
            ESP_LOGW(KCFG_FS_TAG, "kiln config store file is %u bytes with version byte %u (older layout) -- "
                                  "migratable",
                     (unsigned)len, (unsigned)raw[4]);
            free(raw);
            if (out_state) {
                *out_state = KILN_CFG_FILE_OLDER;
            }
            return;
        }
        ESP_LOGE(KCFG_FS_TAG, "kiln config store file is %u bytes, expected %u -- rejected (kept untouched)",
                 (unsigned)len, (unsigned)KCFG_FILE_BUF_MAX);
        if (preserve) {
            (void)kcfg_preserve_rejected(raw, len);
        }
        free(raw);
        if (out_state) {
            *out_state = KILN_CFG_FILE_REJECTED;
        }
        return;
    }

    uint32_t rev = get_u32_le(raw);
    /* Decoded straight into the caller's buffer rather than through a
     * kiln_cfg_store_blob_t local: that local was ~7.5 KiB on the main
     * task's 8192 B stack during boot -- half of the overflow that panicked
     * the board in docs/audits/boot_hang_2026-09-08.md. On a rejected file
     * out_blob is re-zeroed below, so a caller that ignores *out_valid
     * still sees the same all-zero blob it did before. */
    memcpy(out_blob, raw + 4, sizeof(*out_blob));

    if (out_blob->version != KILN_CFG_STORE_VERSION) {
        bool newer = out_blob->version > KILN_CFG_STORE_VERSION;
        /* persmed MED-C: an exact-size file at a version BELOW this build's is an older layout to migrate, not a
         * rejected file. Version 0 (never a real layout) stays corrupt. */
        bool older = !newer && out_blob->version >= 1;
        ESP_LOGE(KCFG_FS_TAG,
                 "kiln config store file (rev %lu) claims version %u, this firmware wants %u -- %s%s",
                 (unsigned long)rev, (unsigned)out_blob->version, (unsigned)KILN_CFG_STORE_VERSION,
                 newer ? "NEWER" : older ? "OLDER (migratable)" : "rejected", older ? "" : ", kept untouched");
        if (preserve && !newer && !older) {
            (void)kcfg_preserve_rejected(raw, len);
        }
        free(raw);
        memset(out_blob, 0, sizeof(*out_blob));
        if (out_state) {
            *out_state = newer ? KILN_CFG_FILE_NEWER : older ? KILN_CFG_FILE_OLDER : KILN_CFG_FILE_REJECTED;
        }
        return;
    }
    free(raw);

    *out_rev = rev;
    *out_valid = true;
    if (out_state) {
        *out_state = KILN_CFG_FILE_VALID;
    }
}

bool kiln_cfg_store_cfg_fs_preserve_bad(const void *raw, size_t len)
{
    return kcfg_preserve_rejected((const uint8_t *)raw, len);
}

bool kiln_cfg_store_cfg_fs_preserve_existing_file(void)
{
    if (!cfg_fs_is_available()) {
        return true; /* nothing on disk to lose */
    }
    size_t cap = KCFG_FILE_BUF_MAX * 2; /* also covers an oversized (newer) file up to twice this build's size */
    uint8_t *raw = persist_scratch_alloc(cap);
    if (!raw) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(KILN_CFG_STORE_FILE_PATH, raw, cap, &len);
    bool ok;
    if (err == ESP_ERR_NOT_FOUND) {
        ok = true; /* no file, nothing to keep */
    } else if (err == ESP_OK && len > 0) {
        ok = kcfg_preserve_rejected(raw, len);
    } else {
        ok = false; /* unreadable or oversized beyond the cap: cannot make a copy */
    }
    free(raw);
    return ok;
}

void kiln_cfg_store_cfg_fs_load_raw(kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_valid)
{
    kiln_cfg_store_cfg_fs_load_raw_ex(out_blob, out_rev, out_valid, NULL, false);
}

esp_err_t kiln_cfg_store_cfg_fs_save(const kiln_cfg_store_blob_t *blob, uint32_t rev)
{
    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *raw = persist_scratch_alloc(KCFG_FILE_BUF_MAX);
    if (!raw) {
        ESP_LOGE(KCFG_FS_TAG, "file write buffer alloc failed (%u bytes) -- refusing to write",
                 (unsigned)KCFG_FILE_BUF_MAX);
        return ESP_ERR_NO_MEM;
    }
    put_u32_le(raw, rev);
    memcpy(raw + 4, blob, sizeof(*blob));

    esp_err_t err = s_write_fn(KILN_CFG_STORE_FILE_PATH, raw, KCFG_FILE_BUF_MAX);
    free(raw);
    if (err != ESP_OK) {
        ESP_LOGW(KCFG_FS_TAG, "kiln config store file write (rev %lu) failed: %s", (unsigned long)rev,
                 esp_err_to_name(err));
    }
    return err;
}

bool kiln_cfg_store_cfg_fs_resolve(const kiln_cfg_store_blob_t *nvs_blob, bool nvs_valid, uint32_t nvs_rev,
                                    kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_used_file)
{
    return kiln_cfg_store_cfg_fs_resolve_ex(nvs_blob, nvs_valid, nvs_rev, out_blob, out_rev, out_used_file, NULL);
}

bool kiln_cfg_store_cfg_fs_resolve_ex(const kiln_cfg_store_blob_t *nvs_blob, bool nvs_valid, uint32_t nvs_rev,
                                      kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_used_file,
                                      kiln_cfg_file_state_t *out_state)
{
    if (out_state) {
        *out_state = KILN_CFG_FILE_ABSENT;
    }
    if (out_blob) {
        memset(out_blob, 0, sizeof(*out_blob));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_used_file) {
        *out_used_file = false;
    }
    if (!nvs_blob || !out_blob || !out_rev || !out_used_file) {
        return false;
    }

    kiln_cfg_store_blob_t *file_blob = kiln_cfg_store_blob_alloc();
    if (!file_blob) {
        /* Cannot even attempt the file side this call -- degrade to the NVS
         * candidate exactly like "partition absent"/"file absent" would. */
        /* persfx4 MED-3: the file side could not even be attempted. Do not decide from NVS and do not write:
         * report UNKNOWN so the caller quarantines (refuses mutators) instead of running on a frozen copy. */
        ESP_LOGE(KCFG_FS_TAG, "resolve: scratch buffer alloc failed -- file state UNKNOWN, nothing written");
        if (out_state) {
            *out_state = KILN_CFG_FILE_UNKNOWN;
        }
        return false;
    }

    uint32_t file_rev = 0;
    bool file_valid = false;
    kiln_cfg_file_state_t fstate = KILN_CFG_FILE_ABSENT;
    kiln_cfg_store_cfg_fs_load_raw_ex(file_blob, &file_rev, &file_valid, &fstate, true);
    if (out_state) {
        *out_state = fstate;
    }
    if (fstate == KILN_CFG_FILE_UNKNOWN || fstate == KILN_CFG_FILE_NEWER || fstate == KILN_CFG_FILE_REJECTED ||
        fstate == KILN_CFG_FILE_OLDER) {
        /* persmed MED-C: OLDER is reported, never decided here -- kiln_cfg_store.c migrates it. */
        /* persfx4 MED-3: a file that exists but cannot be used is never "absent". The frozen NVS blob does NOT
         * replace it and nothing is written: out_blob stays zeroed, trustworthy=false, and the caller quarantines. */
        free(file_blob);
        return false;
    }

    if (!file_valid) {
        /* No usable file. Fall back to the NVS candidate, and if it is
         * itself trustworthy, write it out -- lazy, one-shot migration, no
         * separate migration task. */
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        if (nvs_valid) {
            esp_err_t werr = kiln_cfg_store_cfg_fs_save(nvs_blob, nvs_rev);
            if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(KCFG_FS_TAG, "could not migrate NVS kiln config store to file: %s", esp_err_to_name(werr));
            }
        }
        free(file_blob);
        return nvs_valid;
    }

    if (!nvs_valid) {
        /* File is good, NVS side has nothing trustworthy -- use the file
         * outright. Not logged as a divergence: nothing on the NVS side to
         * disagree WITH. */
        *out_blob = *file_blob;
        *out_rev = file_rev;
        *out_used_file = true;
        free(file_blob);
        return true;
    }

    /* Both sides decoded to something valid -- compare content, not just
     * rev, so two independently-arrived-at-identical stores never get
     * logged as a spurious divergence. */
    bool differs = memcmp(file_blob, nvs_blob, sizeof(*file_blob)) != 0;
    if (!differs) {
        *out_blob = *file_blob;
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        free(file_blob);
        return true;
    }

    /* DIVERGENCE TIE-BREAK: the file wins only on a STRICTLY higher rev.
     * An EQUAL rev with differing content means NVS -- see this module's
     * header comment and check_cfg_fs_tie_break.ps1, which enforces `>` (not
     * `>=`) between file_rev and nvs_rev in every *_cfg_fs.c bridge module. */
    if (file_rev > nvs_rev) {
        ESP_LOGW(KCFG_FS_TAG,
                 "kiln config store file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly "
                 "higher rev)",
                 (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_blob = *file_blob;
        *out_rev = file_rev;
        *out_used_file = true;
        /* NVS resync happens on the next nvs_save_store() call the caller
         * drives once it adopts this result -- this module only ever writes
         * the file side (see header comment). */
    } else {
        ESP_LOGW(KCFG_FS_TAG,
                 "kiln config store file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS "
                 "(higher-or-equal rev), resyncing file",
                 (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = kiln_cfg_store_cfg_fs_save(nvs_blob, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(KCFG_FS_TAG, "could not resync kiln config store file from NVS: %s", esp_err_to_name(werr));
        }
    }
    free(file_blob);
    return true;
}
