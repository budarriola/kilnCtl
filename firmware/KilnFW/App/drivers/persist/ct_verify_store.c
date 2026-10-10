// ct_verify_store -- see ct_verify_store.h for what this stores and why the
// fingerprint, not a remembered-to-clear flag, is what keeps it honest.
//
// This file deliberately depends on NOTHING but hal_kv, pref_cfg_fs and the C library. The
// fingerprint INPUTS are gathered elsewhere (zone_sweep_collect_ct_fingerprint_in(),
// zones_current_sweep_task.c) precisely so that this module stays linkable
// into any host-test executable without dragging the safety config cache,
// the zones config stack or the HTTP layer behind it.
#include "ct_verify_store.h"

#include <math.h>
#include <string.h>

#include "cfg_fs.h"
#include "cfg_fs_status.h"
#include "esp_log.h"
#include "hal_kv.h"
#include "hal_status.h"
#include "nvs_key_check.h"
#include "legacy_nvs_erase.h"
#include "pref_cfg_fs.h"

static const char *TAG = "ct_verify_store";

/* Same partition/namespace family display_power_cfg.c/unit_pref.c use. The
 * namespace is its own (`ct_verify`) rather than the shared `kiln_cfg`
 * because this is a measurement result about one board, not a preference --
 * an erase of the verdict must not be able to take a preference with it.
 * docs/CT_ATTRIBUTION_VERIFICATION.md, "Schema and storage cost". */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "ct_verify"
#define NVS_KEY_VERDICT    "verdict_v1"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_VERDICT);

/* The blob's cap in the plan is 64 bytes. Asserting it here rather than
 * trusting the arithmetic means a future field that quietly pushes past the
 * cap fails the build instead of the plan. */
_Static_assert(sizeof(ct_verify_blob_t) <= 64,
               "ct_verify blob must stay under the plan's 64-byte NVS budget");
_Static_assert(sizeof(ct_verify_blob_t) <= PREF_CFG_FS_MAX_ITEM, "ct_verify blob must fit pref_cfg_fs");
/* The header keeps its own channel/zone counts so it does not have to pull in
 * the zones config stack; this is where the two are held together. */
_Static_assert(CT_VERIFY_CHANNELS == 3u, "ct_verify channels must match ZONE_CT_CHANNEL_COUNT");
_Static_assert(CT_VERIFY_MAX_ZONES == 3u, "ct_verify zones must match this board's zone count");

/* In-RAM truth. `s_have` false means NEVER RUN, which every reader must treat
 * as "no verdict" -- never as a pass. */
static bool s_have;
static ct_verify_blob_t s_blob;
/* rev of the cfg file as last verified on flash; the legacy NVS blob has none (rev 0). */
static uint32_t s_rev;

/* ---- fingerprint ------------------------------------------------------ */

/* FNV-1a, 32-bit. Chosen because it is trivially reimplementable in a test
 * and has no endianness or table dependency -- this hash is never compared
 * across builds of different firmwares, only against a value this same
 * function produced on this same board, so its only requirements are that it
 * be deterministic and that it change when any input byte changes. */
#define FNV32_OFFSET 2166136261u
#define FNV32_PRIME  16777619u

static uint32_t fnv_bytes(uint32_t h, const void *bytes, size_t len)
{
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= FNV32_PRIME;
    }
    return h;
}

static uint32_t fnv_u8(uint32_t h, uint8_t v) { return fnv_bytes(h, &v, sizeof(v)); }

static uint32_t fnv_u16(uint32_t h, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu) };
    return fnv_bytes(h, b, sizeof(b));
}

/* CANONICALIZED float hashing. Two reads of an identical configuration must
 * produce an identical hash, so the two ways an IEEE float can differ while
 * MEANING the same thing are collapsed first:
 *   - every NaN payload hashes as one fixed NaN token (an unfetched param can
 *     read back as any NaN; "unset" is one configuration, not many), and
 *   - -0.0f hashes as +0.0f (they compare equal, so they must hash equal, or
 *     a sign flip nothing can observe would read as a configuration change
 *     and silently invalidate a good verdict).
 * Everything else hashes its exact bit pattern: a 1-ULP change in a clamp
 * ratio IS a configuration change. */
static uint32_t fnv_f32(uint32_t h, float v)
{
    uint32_t bits;
    if (v != v) {
        bits = 0x7FC00000u; /* one canonical quiet NaN token */
    } else if (v == 0.0f) {
        bits = 0u;          /* collapses -0.0f onto +0.0f */
    } else {
        memcpy(&bits, &v, sizeof(bits));
    }
    uint8_t b[4] = { (uint8_t)(bits & 0xFFu), (uint8_t)((bits >> 8) & 0xFFu),
                     (uint8_t)((bits >> 16) & 0xFFu), (uint8_t)((bits >> 24) & 0xFFu) };
    return fnv_bytes(h, b, sizeof(b));
}

uint32_t ct_verify_fingerprint(const ct_verify_fingerprint_in_t *in)
{
    if (!in) {
        return CT_VERIFY_FINGERPRINT_NONE;
    }
    /* Hashed field by field in a FIXED order, never as a memcpy of the whole
     * struct: struct padding is uninitialized memory, and hashing it would
     * make an identical configuration produce a different fingerprint run to
     * run -- every stored verdict would read STALE forever, which fails
     * safe but makes the feature useless. */
    uint32_t h = FNV32_OFFSET;
    h = fnv_u8(h, in->zone_count);
    h = fnv_u8(h, in->ct_installed);
    h = fnv_u8(h, in->ct_topology);
    for (size_t z = 0; z < CT_VERIFY_MAX_ZONES; z++) {
        h = fnv_u8(h, in->zone_ct_channel[z]);
        h = fnv_u8(h, in->zone_relay_mask[z]);
        h = fnv_f32(h, in->i_normal_a[z]);
    }
    for (size_t c = 0; c < CT_VERIFY_CHANNELS; c++) {
        h = fnv_u8(h, in->ch_fitted[c]);
        h = fnv_u8(h, in->ct_source[c]);
        h = fnv_f32(h, in->a_fs[c]);
        h = fnv_f32(h, in->zero_mv[c]);
        h = fnv_f32(h, in->gain[c]);
        h = fnv_f32(h, in->k_ct_v_per_a[c]);
        h = fnv_u16(h, in->zero_counts[c]);
        h = fnv_f32(h, in->trim_offset_a[c]);
        h = fnv_f32(h, in->trim_gain[c]);
    }
    /* 0 is reserved for "no fingerprint" and must never be a live value, or a
     * never-stored verdict would compare equal to a live configuration and
     * read as fresh. One in 2^32 configurations lands here; mapping it to 1
     * costs nothing and closes that case. */
    if (h == CT_VERIFY_FINGERPRINT_NONE) {
        h = 1u;
    }
    return h;
}

/* ---- blob validation -------------------------------------------------- */

bool ct_verify_blob_validate(const void *bytes, size_t len)
{
    if (!bytes || len != sizeof(ct_verify_blob_t)) {
        return false;
    }
    const ct_verify_blob_t *b = (const ct_verify_blob_t *)bytes;
    if (b->version != CT_VERIFY_BLOB_VERSION) {
        return false;
    }
    if (b->zone_count > CT_VERIFY_MAX_ZONES) {
        return false;
    }
    if (b->reserved[0] != 0u || b->reserved[1] != 0u) {
        return false;
    }
    /* A stored fingerprint of NONE would compare unequal to every live
     * fingerprint and so read STALE, which is the safe direction -- but it is
     * also a value ct_verify_fingerprint() never produces, so its presence
     * means the bytes did not come from this code. Refuse rather than carry
     * an unexplained blob forward. */
    if (b->fingerprint == CT_VERIFY_FINGERPRINT_NONE) {
        return false;
    }
    for (size_t z = 0; z < CT_VERIFY_MAX_ZONES; z++) {
        const ct_verify_zone_t *zv = &b->zone[z];
        if (zv->reserved != 0u) {
            return false;
        }
        /* zone_ct_verdict_t is {INCONCLUSIVE, PASS, FAIL} -- three values. A
         * fourth would be a verdict this build cannot interpret, and the one
         * interpretation that must never be guessed at is PASS. */
        if (zv->verdict > 2u) {
            return false;
        }
        /* responded_ch == CT_VERIFY_CHANNELS is the "no channel responded"
         * sentinel; anything above it names a channel that does not exist. */
        if (zv->responded_ch > CT_VERIFY_CHANNELS) {
            return false;
        }
        /* K10-12: the measured/threshold amps are shown to the operator; a NaN or inf is not a
         * measurement this code produced. */
        if (!isfinite(zv->measured_a) || !isfinite(zv->threshold_a)) {
            return false;
        }
    }
    return true;
}

/* ---- NVS -------------------------------------------------------------- */

/* Reads the legacy NVS blob. quiet suppresses the logs (status polls). */
static bool nvs_load(ct_verify_blob_t *out, bool quiet)
{
    hal_status_t part_err = hal_kv_init_partition(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        if (!quiet) {
            ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- no legacy CT verdict this boot",
                     KILN_NVS_PARTITION, hal_status_to_name(part_err));
        }
        return false;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_NOT_FOUND) {
        /* Never verified on this board: the expected state, not a warning. */
        return false;
    }
    if (err != HAL_OK) {
        if (!quiet) {
            ESP_LOGW(TAG, "nvs_open failed: %s -- no legacy CT verdict this boot", hal_status_to_name(err));
        }
        return false;
    }
    ct_verify_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    size_t len = sizeof(blob);
    hal_status_t rerr = hal_kv_get_blob(&h, NVS_KEY_VERDICT, &blob, &len);
    hal_kv_close(&h);
    if (rerr == HAL_OK) {
        if (ct_verify_blob_validate(&blob, len)) {
            *out = blob;
            return true;
        }
        /* Wrong size, unknown version, or an out-of-range field. Refused
         * outright rather than partially trusted: the only field anyone acts
         * on is the verdict, and a half-understood verdict that happens to
         * read PASS is exactly the defect this store exists to prevent. */
        if (!quiet) {
            ESP_LOGW(TAG, "stored CT verdict blob is size %u (expected %u) / version %u -- ignoring it",
                     (unsigned)len, (unsigned)sizeof(blob), (unsigned)blob.version);
        }
    } else if (rerr != HAL_NOT_FOUND && !quiet) {
        ESP_LOGW(TAG, "CT verdict read failed: %s -- treating as never verified", hal_status_to_name(rerr));
    }
    return false;
}

esp_err_t ct_verify_store_start(void)
{
    s_have = false;
    s_rev = 0;
    memset(&s_blob, 0, sizeof(s_blob));

    ct_verify_blob_t nvs_blob;
    memset(&nvs_blob, 0, sizeof(nvs_blob));
    bool nvs_ok = nvs_load(&nvs_blob, false);

    /* K10-14: a cfg file that EXISTS but does not validate (wrong size, failed validator, unreadable)
     * may be a newer-firmware or damaged verdict. pref_cfg_fs_resolve() treats it as absent and would
     * overwrite it with the older NVS verdict. Do not: serve no verdict (the safe state, never a pass),
     * leave the file for inspection, and say so loudly. */
    if (cfg_fs_is_available()) {
        uint8_t fbuf[4 + sizeof(ct_verify_blob_t) + 64];
        size_t flen = 0;
        esp_err_t ferr = cfg_fs_read(CT_VERIFY_CFG_FILE_PATH, fbuf, sizeof(fbuf), &flen);
        if (ferr != ESP_ERR_NOT_FOUND &&
            (ferr != ESP_OK || flen != 4 + sizeof(ct_verify_blob_t) ||
             !ct_verify_blob_validate(fbuf + 4, sizeof(ct_verify_blob_t)))) {
            ESP_LOGE(TAG,
                     "CT verdict file %s exists but is unusable (read=%s, %u bytes) -- NOT replacing it from NVS; "
                     "no verdict this boot",
                     CT_VERIFY_CFG_FILE_PATH, esp_err_to_name(ferr), (unsigned)flen);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    /* Read-through (pref_cfg_fs.h): the cfg file wins on a strictly higher
     * rev; otherwise the legacy NVS blob stands and, when cfg is mounted, is
     * migrated into the file. Non-fatal throughout: "no verdict" is the safe
     * state. */
    ct_verify_blob_t resolved;
    uint32_t rev = 0;
    bool used_file = false;
    if (pref_cfg_fs_resolve_nvs_retired(CT_VERIFY_CFG_FILE_PATH, &nvs_blob, sizeof(nvs_blob), nvs_ok, 0,
                            ct_verify_blob_validate, &resolved, &rev, &used_file)) {
        s_blob = resolved;
        s_rev = rev;
        s_have = true;
        if (used_file) {
            /* persfx3 MED-3: retire the frozen NVS verdict once the file is the adopted source. */
            static const char *const k_legacy[] = {NVS_KEY_VERDICT};
            (void)legacy_nvs_erase_keys(TAG, KILN_NVS_PARTITION, NVS_NAMESPACE, k_legacy, 1);
        }
        ESP_LOGI(TAG, "CT attribution verdict loaded: %u zones, fingerprint 0x%08lX (source=%s)",
                 (unsigned)s_blob.zone_count, (unsigned long)s_blob.fingerprint, used_file ? "file" : "NVS");
    }
    return ESP_OK;
}

bool ct_verify_store_get(ct_verify_blob_t *out)
{
    if (!s_have) {
        return false;
    }
    if (out) {
        *out = s_blob;
    }
    return true;
}

esp_err_t ct_verify_store_save(const ct_verify_blob_t *blob)
{
    if (!blob || !ct_verify_blob_validate(blob, sizeof(*blob))) {
        return ESP_ERR_INVALID_ARG;
    }

    /* K10-13: RAM is updated only AFTER the verdict is durable. Serving an unpersisted verdict would
     * show a result that silently reverts at reboot; on failure the previous verdict stays and the
     * error is returned. */

    /* cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"):
     * no NVS write follows, and a failure is returned, never masked. The rev
     * advances only after a verified write. */
    uint32_t new_rev = s_rev + 1;
    esp_err_t err = pref_cfg_fs_commit(CT_VERIFY_CFG_FILE_PATH, blob, sizeof(*blob), new_rev, "CT verdict");
    if (err != ESP_OK) {
        return err;
    }
    s_blob = *blob;
    s_have = true;
    s_rev = new_rev;
    return ESP_OK;
}

void ct_verify_store_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                          bool *diverged)
{
    ct_verify_blob_t f;
    memset(&f, 0, sizeof(f));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw_quiet(CT_VERIFY_CFG_FILE_PATH, sizeof(f), ct_verify_blob_validate, &f, &f_rev, &f_valid);

    ct_verify_blob_t n;
    memset(&n, 0, sizeof(n));
    bool n_valid = nvs_load(&n, true);
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
        *nvs_rev = 0; /* the NVS blob has no rev key */
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
}
