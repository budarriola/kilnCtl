// Host tests for iter_tune_store.c (docs/ITER_TUNE_REDESIGN.md sec 8
// row 7): round-trip, wrong-version reject, truncated-blob reject, and the
// cfg LittleFS read-through, same conventions test_kiln_cfg_store.c uses for
// its own store. Since the dual-write window closed (owner decision
// 2026-10-07) saves go to the cfg file ONLY: the NVS copy is a read-only
// legacy fallback, so these tests also prove NVS-to-cfg migration at start
// and that a save is REFUSED, not silently dropped, while cfg is unmounted.
#ifdef _WIN32
#include <direct.h>
#define TIT_MKDIR(p) _mkdir(p)
#define TIT_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TIT_MKDIR(p) mkdir(p, 0755)
#define TIT_RMDIR(p) rmdir(p)
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

// Pulls in the static internals (nvs_load_raw/cfg_fs_load_raw/etc.) the same
// way test_kiln_cfg_store.c reaches kiln_cfg_store.c's.
#include "../drivers/persist/iter_tune_store.c"

#define TIT_SCRATCH_BASE "cfg_fs_test_iter_tune_store"

// The store no longer owns a file codec (pref_cfg_fs does); the tests that
// hand-build or inspect a file keep their own copy of the documented
// "<4-byte LE rev><raw blob>" layout.
#define ITER_TUNE_FILE_BUF_MAX (4 + sizeof(iter_tune_store_blob_t))

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Seeds the legacy NVS copy (blob + rev) the way the pre-close dual-write
// code left it on a real board.
static void seed_nvs(const iter_tune_store_blob_t *blob, uint32_t rev)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open to seed the legacy NVS copy");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, blob, sizeof(*blob)) == HAL_OK, "legacy blob seeded");
    TEST_CHECK(hal_kv_set_u32(&h, ITER_TUNE_NVS_KEY_REV, rev) == HAL_OK, "legacy rev seeded");
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static bool nvs_has_blob(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    iter_tune_store_blob_t raw = {0};
    size_t len = sizeof(raw);
    bool ok = hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &raw, &len) == HAL_OK;
    hal_kv_close(&h);
    return ok;
}

// Same "delete known filenames before rmdir" fix class as
// test_kiln_cfg_store.c's reset_state_cfg_fs() -- TIT_RMDIR only succeeds
// against an EMPTY directory, so a leftover iter_tune.bin (or its .tmp/
// staging copy) from a prior run of this binary defeats it silently,
// leaving a non-empty cfg_fs_test_iter_tune_store/ at the repo root
// (step 7 review, 2026-09-23, finding 3).
static void tit_scratch_clean(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", TIT_SCRATCH_BASE, ITER_TUNE_CFG_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", TIT_SCRATCH_BASE, ITER_TUNE_CFG_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", TIT_SCRATCH_BASE);
    TIT_RMDIR(tmp);
    TIT_RMDIR(TIT_SCRATCH_BASE);
}

static void tit_reset_all(void)
{
    fake_kv_reset_all();
    iter_tune_store_reset_for_test();
}

static iter_tune_store_zone_t make_zone(float kp)
{
    iter_tune_store_zone_t z = {0};
    z.enabled = 1;
    z.has_anchor = 1;
    z.anchor_kp = kp;
    z.anchor_ki = kp * 0.1f;
    z.anchor_kd = kp * 0.01f;
    z.has_baseline = 1;
    z.baseline_kp = kp;
    z.baseline_ki = kp * 0.1f;
    z.baseline_kd = kp * 0.01f;
    z.status = 1;
    z.stop_reason = 0;
    return z;
}

static void test_blob_validate(void)
{
    iter_tune_store_blob_t blob = {0};
    blob.version = ITER_TUNE_STORE_VERSION;
    blob.zone_count = 1;
    blob.zone[0] = make_zone(12.0f);
    TEST_CHECK(iter_tune_store_blob_validate(&blob, sizeof(blob)), "valid blob accepted");

    TEST_CHECK(!iter_tune_store_blob_validate(&blob, sizeof(blob) - 1), "wrong length rejected");

    iter_tune_store_blob_t bad_version = blob;
    bad_version.version = (uint8_t)(ITER_TUNE_STORE_VERSION + 1);
    TEST_CHECK(!iter_tune_store_blob_validate(&bad_version, sizeof(bad_version)), "wrong version rejected");

    iter_tune_store_blob_t bad_count = blob;
    bad_count.zone_count = ITER_TUNE_STORE_MAX_ZONES + 1;
    TEST_CHECK(!iter_tune_store_blob_validate(&bad_count, sizeof(bad_count)), "out-of-range zone_count rejected");

    TEST_CHECK(!iter_tune_store_blob_validate(NULL, sizeof(blob)), "NULL rejected");
}

static void test_cfg_round_trip(void)
{
    tit_scratch_clean();
    TIT_MKDIR(TIT_SCRATCH_BASE);
    TEST_CHECK(cfg_fs_init(TIT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts for the round trip");
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start on empty store succeeds");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "no zone persisted yet");

    iter_tune_store_zone_t z0 = make_zone(10.0f);
    TEST_CHECK(iter_tune_store_set_zone(0, &z0) == ESP_OK, "set_zone 0 succeeds");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 10.0f, "zone 0 reads back after set");
    TEST_CHECK(!nvs_has_blob(), "the save went to the cfg file ONLY -- no NVS blob was written");

    // Simulate a reboot: reset in-RAM state only; the file is the sole source.
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start reloads persisted store");
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 10.0f &&
                   out.enabled == 1 && out.has_anchor == 1,
               "zone 0 survives a simulated reboot");

    TEST_CHECK(iter_tune_store_set_zone(ITER_TUNE_STORE_MAX_ZONES, &z0) == ESP_ERR_INVALID_ARG,
               "out-of-range zone_index rejected");
    TEST_CHECK(iter_tune_store_set_zone(0, NULL) == ESP_ERR_INVALID_ARG, "NULL zone rejected");

    cfg_fs_deinit();
    tit_scratch_clean();
}

// Owner decision 2026-10-07: with cfg unmounted a save is REFUSED (an error
// the HTTP route turns into a 503), never silently dropped and never parked
// in NVS.
static void test_set_zone_refused_when_unmounted(void)
{
    tit_scratch_clean();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs is unmounted for the refusal test");
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start succeeds with cfg unmounted");

    iter_tune_store_zone_t z0 = make_zone(10.0f);
    uint32_t rev_before = s_rev;
    TEST_CHECK(iter_tune_store_set_zone(0, &z0) == ESP_ERR_INVALID_STATE,
               "set_zone is refused with ESP_ERR_INVALID_STATE while cfg is unmounted");
    TEST_CHECK(s_rev == rev_before, "the rev does not advance on a refused save");
    TEST_CHECK(!nvs_has_blob(), "the refused save did NOT fall back to writing NVS");

    // The change is live in RAM (documented) but a reboot forgets it.
    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 10.0f, "refused save still applies live");
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start after the refused save");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "a refused save does not survive a reboot");
}

// First boot after the close: the legacy NVS copy is adopted and migrated
// into the file, then the file serves the next boot without NVS.
static void test_nvs_copy_migrates_into_cfg_at_start(void)
{
    tit_scratch_clean();
    TIT_MKDIR(TIT_SCRATCH_BASE);
    TEST_CHECK(cfg_fs_init(TIT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts for the migration test");
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    iter_tune_store_blob_t legacy = {0};
    legacy.version = ITER_TUNE_STORE_VERSION;
    legacy.zone_count = 1;
    legacy.zone[0] = make_zone(21.0f);
    seed_nvs(&legacy, 4u);

    uint8_t filebuf[ITER_TUNE_FILE_BUF_MAX];
    size_t file_len = 0;
    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, filebuf, sizeof(filebuf), &file_len) != ESP_OK,
               "no cfg file exists before the first boot");

    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start adopts the legacy NVS copy");
    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 21.0f, "legacy zone is served");
    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, filebuf, sizeof(filebuf), &file_len) == ESP_OK &&
                   file_len == sizeof(filebuf) && get_u32_le(filebuf) == 4u,
               "the NVS copy was migrated into the cfg file at its own rev");

    // Drop the NVS copy entirely: the file alone must now carry the store.
    fake_kv_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start with only the migrated file");
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 21.0f, "file alone serves the migrated zone");

    cfg_fs_deinit();
    tit_scratch_clean();
}

// Unmounted first boot after the close: NVS still serves the read (fallback),
// and nothing is migrated anywhere.
static void test_nvs_fallback_serves_when_unmounted(void)
{
    tit_scratch_clean();
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);
    iter_tune_store_blob_t legacy = {0};
    legacy.version = ITER_TUNE_STORE_VERSION;
    legacy.zone_count = 1;
    legacy.zone[0] = make_zone(33.0f);
    seed_nvs(&legacy, 2u);

    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start with cfg unmounted and a legacy NVS copy");
    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 33.0f,
               "NVS stays readable as the fallback when cfg is unmounted");
}

static void test_nvs_wrong_version_and_truncated(void)
{
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    iter_tune_store_blob_t blob = {0};
    blob.version = ITER_TUNE_STORE_VERSION;
    blob.zone_count = 1;
    blob.zone[0] = make_zone(5.0f);

    // Wrong version written directly to the fake NVS backing store --
    // start() must fall back to "nothing persisted" rather than trusting it.
    iter_tune_store_blob_t bad_version = blob;
    bad_version.version = (uint8_t)(ITER_TUNE_STORE_VERSION + 1);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open for wrong-version fixture");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &bad_version, sizeof(bad_version)) == HAL_OK,
               "wrong-version blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates a wrong-version blob");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "wrong-version blob is never trusted");

    // Truncated (one byte short) blob -- same rejection.
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open for truncated fixture");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &blob, sizeof(blob) - 1) == HAL_OK,
               "truncated blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates a truncated blob");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "truncated blob is never trusted");
}

// Reads the RAW NVS blob/rev directly, bypassing the public
// iter_tune_store_get_zone()/s_blob API entirely -- needed because an
// in-RAM-only migration and an eager on-disk re-persist are otherwise
// indistinguishable through the public API alone (step 7 review, 2026-09-23,
// finding 2): a buggy "always re-derive v2 in RAM on every load"
// implementation would pass any assertion made only through
// iter_tune_store_get_zone()/s_blob.
static void read_raw_nvs(uint8_t *out_version, uint32_t *out_rev)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open to inspect raw NVS state");
    iter_tune_store_blob_t raw = {0};
    size_t len = sizeof(raw);
    TEST_CHECK(hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &raw, &len) == HAL_OK, "raw NVS blob read back");
    uint32_t rev = 0;
    hal_kv_get_u32(&h, ITER_TUNE_NVS_KEY_REV, &rev);
    hal_kv_close(&h);
    *out_version = raw.version;
    *out_rev = rev;
}

// Step 7 acceptance gap (2), 2026-09-23, REWRITTEN per review finding 1+2:
// exercises the schema-migration mechanism (v1 -> v2, byte-compatible) AND
// proves the on-disk copy stays at v1 until a REAL write happens --
// iter_tune_store_start() deliberately migrates s_blob in RAM only, never
// re-persisting on a bare load (see iter_tune_store.c's iter_tune_store_
// start() comment for the rollback-safety rationale: re-tagging the on-disk
// copy to v2 before any real v2 writer exists would make a v1-firmware
// rollback right after this boot read the store as version-mismatched).
static void test_v1_old_layout_migrates(void)
{
    tit_scratch_clean();
    TIT_MKDIR(TIT_SCRATCH_BASE);
    TEST_CHECK(cfg_fs_init(TIT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts for the migration test");

    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    // A v1 record: byte-compatible with the current struct (see
    // iter_tune_store.h's header comment), just tagged with the OLD version
    // number and its carry_count byte left at the zero every v1 writer used.
    // Written to BOTH NVS and the cfg file at rev 3, same as a real dual-
    // written v1 store would be.
    iter_tune_store_blob_t v1_blob = {0};
    v1_blob.version = ITER_TUNE_STORE_VERSION_V1;
    v1_blob.zone_count = 1;
    v1_blob.zone[0] = make_zone(15.0f);
    v1_blob.zone[0].carry_count = 0;

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open for v1 fixture");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &v1_blob, sizeof(v1_blob)) == HAL_OK,
               "v1 blob written directly, bypassing this build's writer");
    TEST_CHECK(hal_kv_set_u32(&h, ITER_TUNE_NVS_KEY_REV, 3u) == HAL_OK, "v1 fixture rev written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    uint8_t filebuf[ITER_TUNE_FILE_BUF_MAX];
    put_u32_le(filebuf, 3u);
    memcpy(filebuf + 4, &v1_blob, sizeof(v1_blob));
    TEST_CHECK(cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK,
               "v1 blob also written directly to the cfg file at the same rev");

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start loads and migrates a v1 blob");
    TEST_CHECK(!iter_tune_store_schema_refused(NULL), "a known-old version is not a refusal");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 15.0f && out.enabled == 1,
               "v1 zone content survives migration unchanged");
    TEST_CHECK(s_blob.version == ITER_TUNE_STORE_VERSION, "in-RAM blob re-tagged to the current version");

    // The on-disk copies must NOT have been re-tagged by a bare load: still
    // v1, still rev 3, in both NVS and the cfg file.
    uint8_t raw_version = 0;
    uint32_t raw_rev = 0;
    read_raw_nvs(&raw_version, &raw_rev);
    TEST_CHECK(raw_version == ITER_TUNE_STORE_VERSION_V1,
               "raw on-disk NVS blob stays tagged v1 after a bare load (no eager re-persist)");
    TEST_CHECK(raw_rev == 3u, "raw on-disk NVS rev is untouched by a bare load");

    uint8_t filebuf_after[ITER_TUNE_FILE_BUF_MAX];
    size_t file_len = 0;
    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, filebuf_after, sizeof(filebuf_after), &file_len) == ESP_OK,
               "raw cfg file read back after a bare load");
    TEST_CHECK(file_len == sizeof(filebuf_after) && get_u32_le(filebuf_after) == 3u,
               "raw cfg file rev is untouched by a bare load");
    TEST_CHECK(((iter_tune_store_blob_t *)(filebuf_after + 4))->version == ITER_TUNE_STORE_VERSION_V1,
               "raw cfg file blob stays tagged v1 after a bare load (no eager re-persist)");

    // NOW perform a real write. This must persist the in-RAM (already
    // migrated) blob, which always stamps the current version -- so a real
    // write is exactly the moment the on-disk copies first become v2.
    iter_tune_store_zone_t z1 = make_zone(16.0f);
    TEST_CHECK(iter_tune_store_set_zone(1, &z1) == ESP_OK, "a real write after migration succeeds");

    // The real write goes to the cfg file ONLY: the legacy NVS copy is left
    // exactly as it was (still v1, still rev 3).
    read_raw_nvs(&raw_version, &raw_rev);
    TEST_CHECK(raw_version == ITER_TUNE_STORE_VERSION_V1 && raw_rev == 3u,
               "a real write no longer touches the legacy NVS copy");

    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, filebuf_after, sizeof(filebuf_after), &file_len) == ESP_OK,
               "raw cfg file read back after a real write");
    TEST_CHECK(file_len == sizeof(filebuf_after) && get_u32_le(filebuf_after) == 4u,
               "a real write bumps the cfg file rev exactly once");
    TEST_CHECK(((iter_tune_store_blob_t *)(filebuf_after + 4))->version == ITER_TUNE_STORE_VERSION,
               "a real write re-tags the cfg file blob to v2 too");

    // Reboot again: the now-v2 on-disk blob loads cleanly with no further
    // migration needed, and both zones (the v1-carried zone 0 and the new
    // zone 1) survive.
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start reloads the now-v2 blob");
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 15.0f,
               "the originally-migrated zone 0 survives a second reboot");
    TEST_CHECK(iter_tune_store_get_zone(1, &out) && out.anchor_kp == 16.0f,
               "the post-migration real write survives a second reboot");
    TEST_CHECK(s_blob.version == ITER_TUNE_STORE_VERSION, "second load is already current-version");

    cfg_fs_deinit();
    tit_scratch_clean();
}

// Step 7 acceptance gap (2): the refusal half of the same mechanism -- a
// version NEWER than this build knows must be refused (never partially
// trusted) AND reported loudly via iter_tune_store_schema_refused(), not
// folded silently into the same bucket as a truncated/corrupt blob.
static void test_newer_version_refused_and_reported(void)
{
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    iter_tune_store_blob_t future_blob = {0};
    future_blob.version = (uint8_t)(ITER_TUNE_STORE_VERSION + 1);
    future_blob.zone_count = 1;
    future_blob.zone[0] = make_zone(20.0f);

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open for future-version fixture");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &future_blob, sizeof(future_blob)) == HAL_OK,
               "future-version blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates a newer-than-known blob");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "newer-than-known blob is never trusted");

    uint8_t reported_version = 0;
    TEST_CHECK(iter_tune_store_schema_refused(&reported_version), "newer-than-known is reported as a refusal");
    TEST_CHECK(reported_version == (uint8_t)(ITER_TUNE_STORE_VERSION + 1),
               "reported version matches the rejected blob's version byte");

    // The truncated-blob case (already covered above) must NOT set this flag
    // -- only a specifically NEWER version does.
    iter_tune_store_reset_for_test();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);
    iter_tune_store_blob_t ok_blob = {0};
    ok_blob.version = ITER_TUNE_STORE_VERSION;
    ok_blob.zone_count = 1;
    ok_blob.zone[0] = make_zone(1.0f);
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open for truncated fixture (schema_refused check)");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, &ok_blob, sizeof(ok_blob) - 1) == HAL_OK,
               "truncated blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates a truncated blob (schema_refused check)");
    TEST_CHECK(!iter_tune_store_schema_refused(NULL), "a truncated blob is corruption, not a version refusal");
}

// A size-CHANGING newer-version NVS blob (larger than today's struct) must be
// reported as "newer", not folded into corruption, and must NOT be erased or
// overwritten (downgrade-then-upgrade keeps its tuning).
static void tit_seed_raw(const uint8_t *bytes, size_t n)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) == HAL_OK,
               "hal_kv open for raw fixture");
    TEST_CHECK(hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, bytes, n) == HAL_OK, "raw blob written directly");
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static size_t tit_raw_len(void)
{
    hal_kv_handle_t h;
    size_t len = 0;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        return 0;
    }
    if (hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, NULL, &len) != HAL_OK) {
        len = 0;
    }
    hal_kv_close(&h);
    return len;
}

static void test_larger_newer_blob_reported_newer_and_preserved(void)
{
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    uint8_t oversized[sizeof(iter_tune_store_blob_t) + 8];
    memset(oversized, 0, sizeof(oversized));
    oversized[0] = (uint8_t)(ITER_TUNE_STORE_VERSION + 1); // newer-than-known
    tit_seed_raw(oversized, sizeof(oversized));

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates an oversized newer blob without crashing");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "oversized blob is never trusted (falls back to defaults)");
    uint8_t v = 0;
    TEST_CHECK(iter_tune_store_schema_refused(&v) && v == (uint8_t)(ITER_TUNE_STORE_VERSION + 1),
               "a size-changing newer version is reported as newer, not corruption");
    TEST_CHECK(tit_raw_len() == sizeof(oversized), "the newer blob is preserved, not erased or rewritten");
}

// Negative: a CURRENT-version blob of the wrong size, or with bad contents,
// is still corruption (no newer verdict).
static void test_current_version_wrong_size_still_corruption(void)
{
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    uint8_t oversized[sizeof(iter_tune_store_blob_t) + 8];
    memset(oversized, 0, sizeof(oversized));
    oversized[0] = ITER_TUNE_STORE_VERSION;
    tit_seed_raw(oversized, sizeof(oversized));
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates a wrong-size current blob");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "wrong-size current blob is not trusted");
    TEST_CHECK(!iter_tune_store_schema_refused(NULL), "wrong-size current-version blob is corruption, not newer");

    // Right size, current version, invalid contents (zone_count too large).
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);
    iter_tune_store_blob_t bad = {0};
    bad.version = ITER_TUNE_STORE_VERSION;
    bad.zone_count = (uint8_t)(ITER_TUNE_STORE_MAX_ZONES + 1);
    tit_seed_raw((const uint8_t *)&bad, sizeof(bad));
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates an invalid current-version blob");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "invalid current-version blob is not trusted");
    TEST_CHECK(!iter_tune_store_schema_refused(NULL), "invalid current-version blob is corruption, not newer");
}

// cfg-file twin of the NVS oversized-blob fix: a size-changing NEWER file is
// reported newer and preserved byte-identical; a current-version wrong-size
// file stays plain corruption (no report).
static void test_cfg_wrong_size_newer_vs_corrupt(void)
{
    tit_scratch_clean();
    TIT_MKDIR(TIT_SCRATCH_BASE);
    TEST_CHECK(cfg_fs_init(TIT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts for wrong-size cfg test");
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    uint8_t big[4 + sizeof(iter_tune_store_blob_t) + 8];
    memset(big, 0, sizeof(big));
    big[0] = 7; // rev
    big[4] = (uint8_t)(ITER_TUNE_STORE_VERSION + 1);
    big[sizeof(big) - 1] = 0xAB;
    TEST_CHECK(cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, big, sizeof(big)) == ESP_OK, "newer file written");
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates larger newer cfg file");
    uint8_t rv = 0;
    TEST_CHECK(iter_tune_store_schema_refused(&rv) && rv == (uint8_t)(ITER_TUNE_STORE_VERSION + 1),
               "larger newer cfg file reported as NEWER");
    uint8_t back[sizeof(big) + 16];
    size_t got = 0;
    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, back, sizeof(back), &got) == ESP_OK && got == sizeof(big) &&
                   memcmp(back, big, sizeof(big)) == 0,
               "newer cfg file preserved byte-identical");

    // Current-version wrong size: corruption, not a newer report.
    big[4] = ITER_TUNE_STORE_VERSION;
    TEST_CHECK(cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, big, sizeof(big)) == ESP_OK, "wrong-size file written");
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates wrong-size current-version file");
    TEST_CHECK(!iter_tune_store_schema_refused(NULL), "current-version wrong-size file is corruption, not newer");

    // A newer file larger than the probe buffer (cfg_fs_read fails INVALID_SIZE)
    // must still count as NEWER and stay untouched.
    static uint8_t huge[4 + PREF_CFG_FS_MAX_LARGE_ITEM + 64 + 100];
    memset(huge, 0, sizeof(huge));
    huge[0] = 9;
    huge[4] = (uint8_t)(ITER_TUNE_STORE_VERSION + 1);
    huge[sizeof(huge) - 1] = 0xCD;
    TEST_CHECK(cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, huge, sizeof(huge)) == ESP_OK, "over-cap newer file written");
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start tolerates over-cap newer cfg file");
    TEST_CHECK(iter_tune_store_schema_refused(NULL), "over-cap newer cfg file reported as NEWER");
    static uint8_t hback[sizeof(huge) + 16];
    got = 0;
    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, hback, sizeof(hback), &got) == ESP_OK && got == sizeof(huge) &&
                   memcmp(hback, huge, sizeof(huge)) == 0,
               "over-cap newer cfg file preserved byte-identical");

    cfg_fs_deinit();
    tit_scratch_clean();
}

static void test_cfg_fs_dual_write_tie_break(void)
{
    tit_scratch_clean(); // pre-clean: a prior run's crash/abort can leave files behind
    TIT_MKDIR(TIT_SCRATCH_BASE);
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs starts unmounted");
    TEST_CHECK(cfg_fs_init(TIT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    iter_tune_store_zone_t z = make_zone(7.0f);
    TEST_CHECK(iter_tune_store_set_zone(0, &z) == ESP_OK, "set_zone writes the cfg file");

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start reloads from the cfg file");
    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 7.0f, "resolved zone matches");

    // A legacy NVS copy at a rev ABOVE the file (a stale file, e.g. restored
    // from an old backup) must win and overwrite the file.
    iter_tune_store_blob_t nvs_ahead = {0};
    nvs_ahead.version = ITER_TUNE_STORE_VERSION;
    nvs_ahead.zone_count = 1;
    nvs_ahead.zone[0] = make_zone(55.0f);
    seed_nvs(&nvs_ahead, 500u);
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start resolves the NVS-ahead case");
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 55.0f,
               "a legacy NVS copy at a higher rev beats a stale file");
    uint8_t chk[ITER_TUNE_FILE_BUF_MAX];
    size_t chk_len = 0;
    TEST_CHECK(cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, chk, sizeof(chk), &chk_len) == ESP_OK &&
                   get_u32_le(chk) == 500u,
               "the winning NVS copy was resynced into the file");

    // Now make the FILE strictly ahead of NVS by writing a higher-rev file
    // directly, and confirm the file wins per the documented tie-break.
    iter_tune_store_blob_t ahead = {0};
    ahead.version = ITER_TUNE_STORE_VERSION;
    ahead.zone_count = 1;
    ahead.zone[0] = make_zone(99.0f);
    uint8_t filebuf[ITER_TUNE_FILE_BUF_MAX];
    put_u32_le(filebuf, 999999u); // far ahead of whatever s_rev currently is
    memcpy(filebuf + 4, &ahead, sizeof(ahead));
    TEST_CHECK(cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK,
               "higher-rev file written directly");

    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start resolves file-ahead case");
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 99.0f,
               "strictly-higher-rev file wins over NVS, per the documented tie-break");

    cfg_fs_deinit();
    tit_scratch_clean();
}

void run_test_iter_tune_store(void)
{
    test_blob_validate();
    test_cfg_round_trip();
    test_cfg_wrong_size_newer_vs_corrupt();
    test_set_zone_refused_when_unmounted();
    test_nvs_copy_migrates_into_cfg_at_start();
    test_nvs_fallback_serves_when_unmounted();
    test_nvs_wrong_version_and_truncated();
    test_v1_old_layout_migrates();
    test_newer_version_refused_and_reported();
    test_larger_newer_blob_reported_newer_and_preserved();
    test_current_version_wrong_size_still_corruption();
    test_cfg_fs_dual_write_tie_break();
}
