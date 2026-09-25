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
    TEST_CHECK(!firing_shadow_get_status(&status),
               "get_status reports not-loaded before firing_shadow_store_start() -- never loads lazily");
    firing_shadow_store_start();
    TEST_CHECK(firing_shadow_get_status(&status), "get_status succeeds once the store is loaded");
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
    firing_shadow_store_start();
    firing_shadow_status_t reloaded;
    TEST_CHECK(firing_shadow_get_status(&reloaded), "get_status reloads from NVS after reset");
    TEST_CHECK(reloaded.firings_scored == 1, "verdict-summary counter survives a simulated reboot");
    TEST_CHECK(reloaded.accept_count + reloaded.reject_count + reloaded.insufficient_count +
                       reloaded.no_matched_pairs_count + reloaded.alloc_failed_count ==
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
    firing_shadow_store_start();
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
    firing_shadow_store_start();
    TEST_CHECK(firing_shadow_get_status(&status), "get_status tolerates a truncated blob");
    TEST_CHECK(status.firings_scored == 0, "truncated blob is never trusted");
}

// Store v1 -> v2 upgrade (2026-09-24 review advisory): a real v1 blob (32 B,
// version 1, no alloc_failed_count) left by older firmware must be MIGRATED,
// carrying firings_scored and every other v1 counter forward -- step 8's "at
// least 5 firings scored" gate must not silently reset to 0 just because the
// blob grew a new field. The only genuinely new field, alloc_failed_count,
// has no v1 history and must come up zeroed, never guessed.
static void test_v1_blob_migrated_counts_survive(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    firing_shadow_store_blob_v1_t v1 = {0};
    v1.version = 1;
    v1.firings_scored = 7;
    v1.accept_count = 4;
    v1.reject_count = 2;
    v1.insufficient_count = 1;
    v1.no_matched_pairs_count = 3;
    v1.last_verdict = (uint8_t)FIRING_COMPARE_ACCEPT;
    v1.last_composite_normalised = 0.5f;

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE,
                            FIRING_SHADOW_NVS_PARTITION) == HAL_OK,
               "hal_kv open for v1 fixture");
    TEST_CHECK(hal_kv_set_blob(&h, FIRING_SHADOW_NVS_KEY, &v1, sizeof(v1)) == HAL_OK, "v1 blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    firing_shadow_reset_for_test();
    firing_shadow_store_start();
    firing_shadow_status_t status;
    TEST_CHECK(firing_shadow_get_status(&status), "get_status tolerates a v1 blob");
    TEST_CHECK(status.firings_scored == 7, "firings_scored survives v1 -> v2 migration");
    TEST_CHECK(status.accept_count == 4 && status.reject_count == 2 && status.insufficient_count == 1
                   && status.no_matched_pairs_count == 3,
               "every v1 counter survives the migration, not just firings_scored");
    TEST_CHECK(status.last_verdict == (uint8_t)FIRING_COMPARE_ACCEPT && status.last_composite_normalised == 0.5f,
               "last_verdict/last_composite_normalised survive the migration too");
    TEST_CHECK(status.alloc_failed_count == 0,
               "alloc_failed_count has no v1 history and starts at 0, not a guess");
}

// A blob that is v1-SIZED but does not actually claim version 1 (corruption,
// or some future format that happens to collide on size) must not be
// migrated -- same fail-closed convention as the wrong-version/truncated
// tests above.
static void test_v1_sized_wrong_version_not_migrated(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    firing_shadow_store_blob_v1_t v1 = {0};
    v1.version = 99;
    v1.firings_scored = 7;

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, FIRING_SHADOW_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE,
                            FIRING_SHADOW_NVS_PARTITION) == HAL_OK,
               "hal_kv open for v1-sized wrong-version fixture");
    TEST_CHECK(hal_kv_set_blob(&h, FIRING_SHADOW_NVS_KEY, &v1, sizeof(v1)) == HAL_OK,
               "v1-sized wrong-version blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    firing_shadow_reset_for_test();
    firing_shadow_store_start();
    firing_shadow_status_t status;
    TEST_CHECK(firing_shadow_get_status(&status), "get_status tolerates a v1-sized wrong-version blob");
    TEST_CHECK(status.firings_scored == 0, "a v1-sized blob NOT claiming version 1 is never trusted");
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

// 2026-09-24 review advisory ("reset one side of a pair" class): the halt
// path skips firing_shadow_finish_firing() entirely when the caller's stack
// is PSRAM (firing_stats_persist()'s own caller_stack_is_external() guard),
// so without an explicit abandon call, an in-progress firing's per-zone
// segment state would silently carry into the next firing's first ticks.
// This test drives firing_shadow_zone_tick() directly (this file includes
// firing_shadow.c, so s_current_set/s_zone are visible) to prove
// firing_shadow_abandon_firing() actually discards that state, and that a
// clean firing fed afterward is scored as if the abandoned one never
// happened -- not merged with it.
static void test_abandon_firing_discards_in_progress_state_only(void)
{
    fs_reset_all();
    hal_kv_init_partition(FIRING_SHADOW_NVS_PARTITION);

    // First, a complete, ordinary firing -- becomes the RAM-only reference.
    feed_one_firing(0, 100.0f, 120.0f, 90, 1.0f, 0.2f);
    TEST_CHECK(s_have_previous, "first firing became the previous-firing reference");
    TEST_CHECK(s_current_set.count == 0, "s_current_set is empty again after finish_firing()");

    // Now start a second firing (a would-be halt-on-PSRAM-stack case): feed
    // some ticks, building up in-progress per-zone segment state, but never
    // call firing_shadow_finish_firing() -- simulating the guard refusing
    // and skipping it.
    for (int i = 0; i < 30; i++) {
        firing_shadow_zone_tick(0, /*actual_valid=*/true, 150.0f, 150.0f + 5.0f * i, /*dwelling=*/false,
                                 /*segment_index=*/0, 1.0f);
    }
    TEST_CHECK(s_zone[0].seg_active, "the abandoned firing left an open segment behind");

    firing_shadow_abandon_firing();

    TEST_CHECK(s_current_set.count == 0, "abandon_firing discards the in-progress score set");
    TEST_CHECK(!s_zone[0].seg_active, "abandon_firing closes the open per-zone segment without scoring it");
    TEST_CHECK(s_have_previous, "abandon_firing does not touch the previous-firing reference");

    firing_shadow_status_t status_before;
    TEST_CHECK(firing_shadow_get_status(&status_before), "get_status before the next clean firing");
    uint32_t scored_before = status_before.firings_scored;

    // A third, ordinary firing shaped exactly like the first -- if the
    // abandoned firing's state had leaked forward, this firing's own
    // segment tracking would start from a stale current_segment_index/
    // zone_captured/seg rather than a clean slate, and could either double-
    // count or silently skip its own first segment.
    feed_one_firing(0, 100.0f, 120.0f, 90, 1.0f, 0.2f);

    firing_shadow_status_t status_after;
    TEST_CHECK(firing_shadow_get_status(&status_after), "get_status after the next clean firing");
    TEST_CHECK(status_after.firings_scored == scored_before + 1,
               "the clean firing after an abandon is scored normally, exactly once");
    TEST_CHECK((firing_compare_verdict_t)status_after.last_verdict != FIRING_COMPARE_NO_MATCHED_PAIRS,
               "the clean firing matches the (untouched) previous reference -- the abandoned "
               "firing's partial state did not silently become part of either side of the pair");
}

void run_test_firing_shadow(void)
{
    test_first_firing_has_no_verdict();
    test_second_firing_produces_and_persists_verdict();
    test_never_touches_iter_tune_namespace();
    test_wrong_version_and_truncated_blob_rejected();
    test_v1_blob_migrated_counts_survive();
    test_v1_sized_wrong_version_not_migrated();
    test_invalid_and_out_of_range_ticks_ignored();
    test_abandon_firing_discards_in_progress_state_only();
}
