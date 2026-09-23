// Host tests for iter_tune_store.c (docs/ITER_TUNE_REDESIGN_PLAN.md sec 8
// row 7): round-trip, wrong-version reject, truncated-blob reject, and the
// cfg LittleFS dual-write tie-break, same conventions test_kiln_cfg_store.c
// uses for its own store.
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

// Same "delete known filenames before rmdir" fix class as
// test_kiln_cfg_store.c's reset_state_cfg_fs() -- TIT_RMDIR only succeeds
// against an EMPTY directory, so a leftover iter_tune.bin (or its .tmp/
// staging copy) from a prior run of this binary defeats it silently,
// leaving a non-empty cfg_fs_test_iter_tune_store/ at the repo root
// (Opus review of 5f2acb7f, finding 3).
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

static void test_nvs_round_trip(void)
{
    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start on empty store succeeds");
    TEST_CHECK(!iter_tune_store_get_zone(0, NULL), "no zone persisted yet");

    iter_tune_store_zone_t z0 = make_zone(10.0f);
    TEST_CHECK(iter_tune_store_set_zone(0, &z0) == ESP_OK, "set_zone 0 succeeds");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 10.0f, "zone 0 reads back after set");

    // Simulate a reboot: reset in-RAM state only, reload from the (fake) NVS
    // backing store. NOTE (Opus review of 5f2acb7f, finding 4): this is a
    // same-version persistence round trip, NOT a schema migration -- only
    // ITER_TUNE_STORE_VERSION 1 exists today, so there is no v1->v2 case to
    // migrate yet. See docs/ITER_TUNE_REDESIGN_PLAN.md row 7 for the honest
    // acceptance-criteria status.
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start reloads persisted store");
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 10.0f &&
                   out.enabled == 1 && out.has_anchor == 1,
               "zone 0 survives a simulated reboot");

    TEST_CHECK(iter_tune_store_set_zone(ITER_TUNE_STORE_MAX_ZONES, &z0) == ESP_ERR_INVALID_ARG,
               "out-of-range zone_index rejected");
    TEST_CHECK(iter_tune_store_set_zone(0, NULL) == ESP_ERR_INVALID_ARG, "NULL zone rejected");
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

static void test_cfg_fs_dual_write_tie_break(void)
{
    tit_scratch_clean(); // pre-clean: a prior run's crash/abort can leave files behind
    TIT_MKDIR(TIT_SCRATCH_BASE);
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs starts unmounted");
    TEST_CHECK(cfg_fs_init(TIT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    tit_reset_all();
    hal_kv_init_partition(ITER_TUNE_NVS_PARTITION);

    iter_tune_store_zone_t z = make_zone(7.0f);
    TEST_CHECK(iter_tune_store_set_zone(0, &z) == ESP_OK, "set_zone dual-writes to cfg_fs too");

    // Reload from scratch: both NVS and the cfg file agree (same rev) --
    // must still read back correctly regardless of which side wins.
    iter_tune_store_reset_for_test();
    TEST_CHECK(iter_tune_store_start() == ESP_OK, "start resolves NVS/cfg_fs agreement");
    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out) && out.anchor_kp == 7.0f, "resolved zone matches");

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
    test_nvs_round_trip();
    test_nvs_wrong_version_and_truncated();
    test_cfg_fs_dual_write_tie_break();
}
