#include "pico_update_attempts.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "pico_upd_attempts";

/* Same partition/namespace as boot_guard.c's CURRENT record -- see that
 * file's NVS_KEY_REC comment for why "kiln_cfg" (not a module-private
 * namespace) is the one namespace on this board proven to actually persist
 * writes across a reboot on this bench unit's flash. */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "kiln_cfg"
/* 10 chars -- NVS_KEY_LEN_CHECK() below enforces the 15-char cap. */
#define NVS_KEY_REC "pauattempt"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_REC);

/* Bumped whenever pico_update_attempts_record_t's layout changes -- same
 * "discard rather than migrate" convention as boot_guard.c/run_state.c: a
 * lost attempt-count record costs at most one fresh budget, never a reason
 * to mis-parse an old layout.
 *
 * v2 (2026-09-20): added last_slot, the persisted embedded-slot alternation
 * for docs/PICO_AUTO_UPDATE.md's embedded-image work -- see
 * pico_update_attempts_next_slot(). A v1 record on flash simply fails
 * record_is_valid() and is discarded (fresh budget), same as any other
 * corruption -- there is no v1->v2 migration here, matching this module's
 * own "discard rather than migrate" convention (NOT the CONFIG_MIGRATION_
 * CHAIN_PLAN.md policy, which applies to the two processors' PERSISTENT
 * CONFIG stores, not this attempt-budget scratch counter). */
#define PUA_RECORD_VERSION 2

/* Single-slot record -- see this module's header top comment. pair_hash
 * says WHICH (expected, observed) pair the count/failed/last_slot fields
 * belong to; a load for a different pair is treated as no record at all. */
typedef struct {
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t pair_hash;
    uint32_t attempt_count;
    uint32_t failed;    /* 0 or 1 -- kept as uint32_t so the struct stays 4-aligned throughout */
    uint32_t last_slot; /* 0 = SaftyFW_slotA, 1 = SaftyFW_slotB; the slot last pushed for this pair, or the Pico's reported active slot after a disagreement correction. The unknown-wire fallback treats it as the believed active slot (next = the other one). */
    uint32_t crc32;
} pico_update_attempts_record_t;

typedef char pua_record_size_check[(sizeof(pico_update_attempts_record_t) == 24) ? 1 : -1];

/* Table-less CRC32 (IEEE 802.3/zlib polynomial) -- copied from
 * boot_guard.c's crc32_compute() rather than shared, so this module builds
 * standalone the same way boot_guard.c does for its own host tests. Verified
 * against the same "123456789" -> 0xCBF43926 vector in
 * test_pico_update_attempts.c. */
static uint32_t crc32_compute(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t record_crc(const pico_update_attempts_record_t *rec)
{
    return crc32_compute(rec, offsetof(pico_update_attempts_record_t, crc32));
}

static bool record_is_valid(const pico_update_attempts_record_t *rec)
{
    return rec->version == PUA_RECORD_VERSION && rec->crc32 == record_crc(rec);
}

uint32_t pico_update_attempts_pair_hash(const char *expected, const uint8_t *observed,
                                         uint8_t observed_len)
{
    /* One CRC32 pass over expected_len, expected bytes, a 0xFF boundary
     * marker, observed_len, then observed bytes -- the explicit length
     * prefixes and marker byte keep "ab"+"c" from hashing the same as
     * "a"+"bc" (a real risk once two variable-length fields are
     * concatenated). Fixed on-stack scratch buffer, no allocation, bounded
     * by PICO_AUTO_UPDATE_MAX_COMMIT_LEN (64) on each side. */
    size_t elen = (expected != NULL) ? strlen(expected) : 0;
    if (elen > 64) {
        elen = 64; /* defensive cap; callers never pass more in practice */
    }
    uint8_t scratch[1 + 64 + 1 + 1 + 64];
    size_t off = 0;
    scratch[off++] = (uint8_t)elen;
    if (elen > 0 && expected != NULL) {
        memcpy(scratch + off, expected, elen);
    }
    off += elen;
    scratch[off++] = 0xFFu; /* boundary marker */
    scratch[off++] = observed_len;
    if (observed_len > 0 && observed != NULL) {
        memcpy(scratch + off, observed, observed_len);
    }
    off += observed_len;
    return crc32_compute(scratch, off);
}

static hal_status_t persist_record(uint32_t pair_hash, uint32_t count, bool failed, int last_slot)
{
    pico_update_attempts_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = PUA_RECORD_VERSION;
    rec.pair_hash = pair_hash;
    rec.attempt_count = count;
    rec.failed = failed ? 1u : 0u;
    rec.last_slot = (last_slot != 0) ? 1u : 0u;
    rec.crc32 = record_crc(&rec);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, NVS_KEY_REC, &rec, sizeof(rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

/* Loads the raw record regardless of which pair it belongs to. Returns
 * false (and leaves *out untouched) for "nothing valid to load" -- missing,
 * wrong size, wrong version, or a bad CRC all collapse to the same answer,
 * same tolerant-default reasoning as boot_guard.c's load_count(). */
static bool load_record(pico_update_attempts_record_t *out)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    pico_update_attempts_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec)) {
        return false;
    }
    if (!record_is_valid(&rec)) {
        ESP_LOGW(TAG, "pico-update-attempts record failed its version/CRC check -- treating as no "
                      "record (fresh budget)");
        return false;
    }
    *out = rec;
    return true;
}

bool pico_update_attempts_load(uint32_t pair_hash, uint32_t *out_count, bool *out_failed)
{
    pico_update_attempts_record_t rec;
    bool have = load_record(&rec) && rec.pair_hash == pair_hash;
    if (out_count != NULL) {
        *out_count = have ? rec.attempt_count : 0u;
    }
    if (out_failed != NULL) {
        *out_failed = have && rec.failed != 0u;
    }
    return have;
}

/* Strict read-back, same distinction as boot_guard.c's verify_persisted_count():
 * "could not read it back" must NOT collapse to "confirmed 0/absent" here --
 * a verify step needs to tell a genuine match apart from an unreadable
 * write, and this caller treats those two outcomes differently (retry vs.
 * give up quietly). */
static bool verify_record(uint32_t pair_hash, uint32_t expected_count, bool expected_failed,
                           int expected_last_slot)
{
    pico_update_attempts_record_t rec;
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec) || !record_is_valid(&rec)) {
        return false;
    }
    return rec.pair_hash == pair_hash && rec.attempt_count == expected_count
           && (rec.failed != 0u) == expected_failed
           && rec.last_slot == ((expected_last_slot != 0) ? 1u : 0u);
}

static hal_status_t erase_then_persist(uint32_t pair_hash, uint32_t count, bool failed, int last_slot)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    hal_status_t erase_err = hal_kv_erase_key(&h, NVS_KEY_REC);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        ESP_LOGW(TAG, "erase_then_persist: hal_kv_erase_key failed: %s -- writing anyway",
                 hal_status_to_name(erase_err));
    }
    hal_kv_close(&h);
    return persist_record(pair_hash, count, failed, last_slot);
}

/* Shared write-verify-retry-once sequence -- boot_guard.c's
 * clear_persisted_counter_verified_locked(), generalized to an arbitrary
 * (count, failed, last_slot) tuple instead of always clearing to 0. Never
 * trust the write call's own return code alone (2026-09-08 boot_guard audit:
 * a HAL_OK write was observed not to reach flash) -- read it back before
 * reporting success. */
static bool write_verified(uint32_t pair_hash, uint32_t count, bool failed, int last_slot)
{
    hal_status_t err = persist_record(pair_hash, count, failed, last_slot);
    bool verified = (err == HAL_OK) && verify_record(pair_hash, count, failed, last_slot);
    if (!verified) {
        ESP_LOGW(TAG, "pico-update-attempts write did not verify on the first attempt -- retrying "
                      "once with an explicit erase-then-write");
        err = erase_then_persist(pair_hash, count, failed, last_slot);
        verified = (err == HAL_OK) && verify_record(pair_hash, count, failed, last_slot);
    }
    if (!verified) {
        ESP_LOGE(TAG, "pico-update-attempts write for pair 0x%08lx (count=%lu failed=%d slot=%d) did not "
                      "verify after retry -- caller must not trust this attempt was persisted",
                 (unsigned long)pair_hash, (unsigned long)count, (int)failed, last_slot);
    }
    return verified;
}

bool pico_update_attempts_record_attempt(uint32_t pair_hash, int slot_tried, uint32_t *out_new_count)
{
    uint32_t prior_count = 0;
    bool have = pico_update_attempts_load(pair_hash, &prior_count, NULL);
    uint32_t new_count = have ? (prior_count + 1u) : 1u;
    if (out_new_count != NULL) {
        *out_new_count = new_count;
    }
    /* A fresh pair starts with failed=false; recording an attempt for the
     * SAME pair preserves whatever failed flag it already had (recording an
     * attempt is orthogonal to recording a failure -- see
     * pico_update_attempts_record_failure()). */
    bool prior_failed = false;
    if (have) {
        (void)pico_update_attempts_load(pair_hash, NULL, &prior_failed);
    }
    return write_verified(pair_hash, new_count, prior_failed, slot_tried);
}

bool pico_update_attempts_record_failure(uint32_t pair_hash)
{
    uint32_t count = 0;
    (void)pico_update_attempts_load(pair_hash, &count, NULL);
    pico_update_attempts_record_t rec;
    int last_slot = load_record(&rec) && rec.pair_hash == pair_hash ? (int)rec.last_slot : 0;
    return write_verified(pair_hash, count, true, last_slot);
}

/* Reset-one-side bug class (CLAUDE.md): this ESP-persisted `last_slot` is a
 * derived EXPECTATION of the Pico's actual `active_slot`, joined only by the
 * implicit contract "the ESP alternated correctly last time". A Pico
 * reflashed by other means (SWD, `debug_program(peer="pico")`) or a Pico
 * that rejects a write for a reason unrelated to slot linkage silently
 * leaves this side's `last_slot` stale.
 *
 * Owner decision 2026-10-02 (docs/PICO_AUTO_UPDATE.md sec 12): the
 * Pico's own report wins. When the wire slot is known (safety_link.h's
 * `pico_active_slot_known`/`pico_active_slot_is_b`) the next slot is the
 * opposite of the reported active slot; the persisted `last_slot` guess is
 * used only when the wire slot is unknown. When the two disagree,
 * pico_update_attempts_next_slot() logs it once at WARN and rewrites the
 * persisted guess to the reported slot, so the pair stops drifting.
 *
 * Pure selection, no I/O: unit-tested directly. */
int pico_update_attempts_select_slot(bool have_record, int last_slot, bool wire_known,
                                     bool wire_active_is_b, bool *out_disagree)
{
    bool disagree = have_record && wire_known && ((last_slot != 0) != wire_active_is_b);
    if (out_disagree != NULL) {
        *out_disagree = disagree;
    }
    if (wire_known) {
        return wire_active_is_b ? 0 : 1;
    }
    return (have_record && last_slot == 0) ? 1 : 0; /* fresh pair, or last was B -> start/return to A */
}

bool pico_update_attempts_next_slot(uint32_t pair_hash, bool wire_known, bool wire_active_is_b,
                                    int *out_slot)
{
    pico_update_attempts_record_t rec;
    bool have = load_record(&rec) && rec.pair_hash == pair_hash;
    bool disagree = false;
    int next = pico_update_attempts_select_slot(have, have ? (int)rec.last_slot : 0, wire_known,
                                                wire_active_is_b, &disagree);
    if (disagree) {
        int active = wire_active_is_b ? 1 : 0;
        /* After a failed push the Pico still runs the old slot, so last pushed != reported active
         * is expected on an ordinary retry boot (count > 0): INFO. WARN otherwise. */
        const bool expected_after_failed_push = rec.attempt_count > 0u;
        const char fmt_last = rec.last_slot != 0u ? 'B' : 'A';
        const char fmt_wire = active != 0 ? 'B' : 'A';
        const char fmt_next = next != 0 ? 'B' : 'A';
        if (expected_after_failed_push) {
            ESP_LOGI(TAG, "persisted last_slot=%c differs from the Pico's reported active slot %c -- "
                          "using the Pico's report, next slot %c, updating the persisted guess",
                     fmt_last, fmt_wire, fmt_next);
        } else {
            ESP_LOGW(TAG, "persisted last_slot=%c differs from the Pico's reported active slot %c -- "
                          "using the Pico's report, next slot %c, updating the persisted guess",
                     fmt_last, fmt_wire, fmt_next);
        }
        if (!write_verified(pair_hash, rec.attempt_count, rec.failed != 0u, active)) {
            ESP_LOGW(TAG, "could not persist the corrected last_slot guess (the Pico's report still "
                          "decides the slot next boot)");
        }
    }
    if (out_slot != NULL) {
        *out_slot = next;
    }
    return have;
}

bool pico_update_attempts_clear(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    hal_status_t erase_err = hal_kv_erase_key(&h, NVS_KEY_REC);
    hal_status_t commit_err = HAL_OK;
    if (erase_err == HAL_OK) {
        commit_err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        ESP_LOGW(TAG, "pico_update_attempts_clear: hal_kv_erase_key failed: %s",
                 hal_status_to_name(erase_err));
        return false;
    }
    if (commit_err != HAL_OK) {
        return false;
    }
    pico_update_attempts_record_t rec;
    bool still_there = load_record(&rec);
    if (still_there) {
        ESP_LOGE(TAG, "pico_update_attempts_clear: record still readable after erase+commit -- "
                      "not confirmed cleared");
        return false;
    }
    return true;
}
