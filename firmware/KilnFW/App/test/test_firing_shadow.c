// Host tests for firing_shadow.c (docs/ITER_TUNE_REDESIGN_PLAN.md step 8,
// "shadow mode"). Proves: the shadow hook runs at firing end and scores
// what it saw, it NEVER changes a gain (structurally -- this module has no
// gain-write API at all, and neither iter_tune_store_blob_t nor any zone
// gain field is ever touched here), and its own compact verdict-summary NVS
// blob round-trips the same way iter_tune_store.c's does.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

// Pulls in the static internals, same convention as test_iter_tune_store.c
// reaching iter_tune_store.c.
#include "../drivers/control/firing_shadow.c"

static void fs_reset_all(void)
{
    fake_kv_reset_all();
    firing_shadow_reset_for_test();
}

// Feeds one whole, simple firing: a single ramp segment (segment_index 0)
// tracking cleanly within band for enough ticks to clear
// FIRING_SCORE_MIN_SCORED_TICKS_DEFAULT, then ends it.
static void feed_one_firing(uint8_t zone_index, float start_c, float rate_c_per_hr, int n_ticks,
                             float dt_s, float error_c)
{
    float target_c = start_c;
    for (int i = 0; i < n_ticks; i++) {
        target_c += rate_c_per_hr / 3600.0f * dt_s;
        float actual_c = target_c - error_c;
        firing_shadow_zone_tick(zone_index, /*actual_valid=*/true, actual_c, target_c,
                                 /*dwelling=*/false, /*segment_index=*/0, dt_s);
    }
    firing_shadow_finish_firing();
}

static void test_first_firing_has_no_verdict(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    firing_shadow_status_t status;
    TEST_CHECK(firing_shadow_get_status(&status), "get_status always succeeds");
    TEST_CHECK(status.firings_scored == 0, "nothing scored before any firing");

    feed_one_firing(/*zone_index=*/0, 100.0f, 120.0f, 90, 1.0f, 0.2f);

    TEST_CHECK(firing_shadow_get_status(&status), "get_status after first firing");
    TEST_CHECK(status.firings_scored == 0,
               "first firing this boot has nothing to compare against -- no verdict recorded");
}

static void test_second_firing_produces_and_persists_verdict(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    feed_one_firing(0, 100.0f, 120.0f, 90, 1.0f, 0.2f);
    feed_one_firing(0, 100.0f, 120.0f, 90, 1.0f, 0.2f);

    firing_shadow_status_t status;
    TEST_CHECK(firing_shadow_get_status(&status), "get_status after second firing");
    TEST_CHECK(status.firings_scored == 1, "second (matching) firing produces one verdict");

    // Simulate a reboot: reset RAM only, reload from the (fake) NVS backing
    // store -- the RAM-only previous-firing reference is lost (by design),
    // but the persisted verdict-summary counters survive.
    firing_shadow_reset_for_test();
    firing_shadow_status_t reloaded;
    TEST_CHECK(firing_shadow_get_status(&reloaded), "get_status reloads from NVS after reset");
    TEST_CHECK(reloaded.firings_scored == 1, "verdict-summary counter survives a simulated reboot");
    TEST_CHECK(reloaded.accept_count + reloaded.reject_count + reloaded.insufficient_count +
                       reloaded.no_matched_pairs_count ==
                   1,
               "exactly one verdict bucket incremented");
}

// The structural guarantee: nothing in this translation unit writes to any
// iter_tune_store_blob_t or zone gain field -- there is no such symbol
// linked into this test at all, so a call that somehow tried to touch one
// would fail to link rather than fail silently. This test additionally
// confirms firing_shadow's own persisted blob namespace/key are distinct
// from iter_tune's, so a shared-partition collision can't overwrite gains
// either.
static void test_never_touches_iter_tune_namespace(void)
{
    TEST_CHECK(strcmp(FIRING_SHADOW_NVS_NAMESPACE, "iter_tune") != 0,
               "firing_shadow uses its own NVS namespace, never iter_tune's");
    TEST_CHECK(strlen(FIRING_SHADOW_NVS_NAMESPACE) <= 15, "namespace key length fits NVS's limit");
    TEST_CHECK(strlen(FIRING_SHADOW_NVS_KEY) <= 15, "blob key length fits NVS's limit");
}

static void test_wrong_version_and_truncated_blob_rejected(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    firing_shadow_store_blob_t blob = {0};
    blob.version = (uint8_t)(FIRING_SHADOW_STORE_VERSION + 1);
    blob.firings_scored = 7;

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE,
                            FIRING_SHADOW_NVS_PARTITION) == HAL_OK,
               "hal_kv open for wrong-version fixture");
    TEST_CHECK(hal_kv_set_blob(&h, FIRING_SHADOW_NVS_KEY, &blob, sizeof(blob)) == HAL_OK,
               "wrong-version blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    firing_shadow_reset_for_test();
    firing_shadow_status_t status;
    TEST_CHECK(firing_shadow_get_status(&status), "get_status tolerates a wrong-version blob");
    TEST_CHECK(status.firings_scored == 0, "wrong-version blob is never trusted");

    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);
    blob.version = FIRING_SHADOW_STORE_VERSION;
    TEST_CHECK(hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE,
                            FIRING_SHADOW_NVS_PARTITION) == HAL_OK,
               "hal_kv open for truncated fixture");
    TEST_CHECK(hal_kv_set_blob(&h, FIRING_SHADOW_NVS_KEY, &blob, sizeof(blob) - 1) == HAL_OK,
               "truncated blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    firing_shadow_reset_for_test();
    TEST_CHECK(firing_shadow_get_status(&status), "get_status tolerates a truncated blob");
    TEST_CHECK(status.firings_scored == 0, "truncated blob is never trusted");
}

static void test_invalid_and_out_of_range_ticks_ignored(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    // Out-of-range zone_index must not crash and must not advance any state.
    firing_shadow_zone_tick((uint8_t)MAX31856_CHANNEL_COUNT, true, 100.0f, 100.0f, false, 0, 1.0f);
    // Invalid sample must be skipped, not scored.
    firing_shadow_zone_tick(0, /*actual_valid=*/false, 999.0f, 100.0f, false, 0, 1.0f);

    firing_shadow_finish_firing(); // must not crash with nothing accumulated
    firing_shadow_status_t status;
    TEST_CHECK(firing_shadow_get_status(&status), "get_status after a no-op firing");
    TEST_CHECK(status.firings_scored == 0, "a firing with no valid ticks scores nothing");
}

void run_test_firing_shadow(void)
{
    test_first_firing_has_no_verdict();
    test_second_firing_produces_and_persists_verdict();
    test_never_touches_iter_tune_namespace();
    test_wrong_version_and_truncated_blob_rejected();
    test_invalid_and_out_of_range_ticks_ignored();
}
