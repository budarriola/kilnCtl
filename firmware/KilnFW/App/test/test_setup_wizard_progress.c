// Host tests for App/drivers/persist/setup_wizard_progress.c -- the whole-
// kiln setup wizard's thin, versioned NVS progress record
// (docs/SETUP_WIZARD_PLAN.md implementation step 1).
//
// THE LOAD-BEARING PROPERTY THIS FILE EXISTS TO PROVE: readiness is
// authoritative over the stored record. A step recorded DONE whose
// readiness item is not ready must render REGRESSED, never DONE -- see
// test_readiness_overrides_stored_regressed() and, above all,
// test_NEGATIVE_stored_state_must_not_win_over_readiness() below.
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "../drivers/persist/setup_wizard_progress.c"

static void reset(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(NVS_PARTITION);
}

// ---------------------------------------------------------------------
// Round trip + defaults
// ---------------------------------------------------------------------

static void test_defaults_on_empty_nvs(void)
{
    reset();
    esp_err_t err = setup_wizard_progress_start();
    TEST_CHECK(err == ESP_OK, "setup_wizard_progress_start() succeeds against an empty stub");
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        setup_wizard_step_t s;
        TEST_CHECK(setup_wizard_progress_get_step(i, &s) == ESP_OK, "get_step succeeds for a valid index");
        TEST_CHECK(s.state == SETUP_WIZ_STEP_PENDING, "empty NVS: every step defaults to PENDING");
        TEST_CHECK(s.ts == 0, "empty NVS: ts defaults to 0");
        TEST_CHECK(s.note[0] == '\0', "empty NVS: note defaults to empty");
    }
}

static void test_round_trip(void)
{
    reset();
    setup_wizard_progress_start();

    esp_err_t err = setup_wizard_progress_set_step(6, SETUP_WIZ_STEP_DONE, NULL);
    TEST_CHECK(err == ESP_OK, "set_step(6, DONE) succeeds");
    err = setup_wizard_progress_set_step(7, SETUP_WIZ_STEP_SKIPPED, "deferred until owner present");
    TEST_CHECK(err == ESP_OK, "set_step(7, SKIPPED, note) succeeds");

    /* Simulate reboot: reset in-RAM state, reload from the stubbed flash. */
    apply_defaults();
    err = setup_wizard_progress_start();
    TEST_CHECK(err == ESP_OK, "reload after simulated reboot succeeds");

    setup_wizard_step_t s6, s7;
    setup_wizard_progress_get_step(6, &s6);
    setup_wizard_progress_get_step(7, &s7);
    TEST_CHECK(s6.state == SETUP_WIZ_STEP_DONE, "step 6 survives the round trip as DONE");
    TEST_CHECK(s7.state == SETUP_WIZ_STEP_SKIPPED, "step 7 survives the round trip as SKIPPED");
    TEST_CHECK(strcmp(s7.note, "deferred until owner present") == 0, "step 7's note survives the round trip");

    /* Every OTHER step must still read PENDING -- proves set_step() only
     * ever touches the one index it was called with. */
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        if (i == 6 || i == 7) continue;
        setup_wizard_step_t s;
        setup_wizard_progress_get_step(i, &s);
        TEST_CHECK(s.state == SETUP_WIZ_STEP_PENDING, "untouched steps stay PENDING after a sibling's set_step");
    }
}

static void test_get_all_matches_get_step(void)
{
    reset();
    setup_wizard_progress_start();
    setup_wizard_progress_set_step(2, SETUP_WIZ_STEP_DONE, NULL);

    setup_wizard_step_t all[SETUP_WIZARD_STEP_COUNT];
    setup_wizard_progress_get_all(all);
    TEST_CHECK(all[2].state == SETUP_WIZ_STEP_DONE, "get_all reflects the same state get_step reports");
}

// ---------------------------------------------------------------------
// Migration from the initial (v1) version
// ---------------------------------------------------------------------

static void test_migration_from_v1(void)
{
    reset();
    setup_wizard_progress_start(); /* opens the namespace once, establishes defaults */

    setup_wizard_progress_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1;
    v1.steps[3].state = (uint8_t)SETUP_WIZ_STEP_DONE;
    v1.steps[3].ts = 12345;
    v1.steps[9].state = (uint8_t)SETUP_WIZ_STEP_SKIPPED;
    v1.steps[9].ts = 999;

    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NVS_PARTITION);
    TEST_CHECK(open_err == HAL_OK, "test setup: hal_kv_open for the v1 stash succeeds");
    hal_status_t set_err = hal_kv_set_blob(&h, NVS_KEY_PROGRESS, &v1, sizeof(v1));
    TEST_CHECK(set_err == HAL_OK, "test setup: stashing the v1-shaped blob succeeds");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    apply_defaults();
    esp_err_t err = setup_wizard_progress_start();
    TEST_CHECK(err == ESP_OK, "start() against a v1 blob still returns ESP_OK");

    setup_wizard_step_t s3, s9, s0;
    setup_wizard_progress_get_step(3, &s3);
    setup_wizard_progress_get_step(9, &s9);
    setup_wizard_progress_get_step(0, &s0);
    TEST_CHECK(s3.state == SETUP_WIZ_STEP_DONE && s3.ts == 12345, "v1 migration: step 3's state+ts carry forward");
    TEST_CHECK(s9.state == SETUP_WIZ_STEP_SKIPPED && s9.ts == 999, "v1 migration: step 9's state+ts carry forward");
    TEST_CHECK(s3.note[0] == '\0', "v1 migration: note (did not exist in v1) defaults to empty");
    TEST_CHECK(s0.state == SETUP_WIZ_STEP_PENDING, "v1 migration: an untouched v1 step still reads PENDING");
}

// ---------------------------------------------------------------------
// Unknown-step rejection
// ---------------------------------------------------------------------

static void test_set_step_rejects_out_of_range_index(void)
{
    reset();
    setup_wizard_progress_start();

    esp_err_t err = setup_wizard_progress_set_step(SETUP_WIZARD_STEP_COUNT, SETUP_WIZ_STEP_DONE, NULL);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "set_step refuses step_index == SETUP_WIZARD_STEP_COUNT");
    err = setup_wizard_progress_set_step(200, SETUP_WIZ_STEP_DONE, NULL);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "set_step refuses a wildly out-of-range step_index");

    setup_wizard_step_t out;
    err = setup_wizard_progress_get_step(SETUP_WIZARD_STEP_COUNT, &out);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "get_step refuses step_index == SETUP_WIZARD_STEP_COUNT");
}

static void test_set_step_rejects_invalid_state(void)
{
    reset();
    setup_wizard_progress_start();

    esp_err_t err = setup_wizard_progress_set_step(1, (setup_wizard_step_state_t)77, NULL);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "set_step refuses an out-of-range state value");

    setup_wizard_step_t s;
    setup_wizard_progress_get_step(1, &s);
    TEST_CHECK(s.state == SETUP_WIZ_STEP_PENDING, "a refused set_step leaves the step's in-RAM state untouched");
}

// ---------------------------------------------------------------------
// NVS unavailable degrades cleanly
// ---------------------------------------------------------------------

static void test_partition_init_failure_degrades_to_defaults(void)
{
    fake_kv_reset_all();
    /* The fake models a fixed number of partition slots
     * (FAKE_KV_MAX_PARTITIONS==4). Filling every slot with unrelated
     * partitions before setup_wizard_progress_start() ever runs forces its
     * own hal_kv_init_partition(NVS_PARTITION) to fail with HAL_NO_MEM --
     * a real stand-in for "NVS unavailable", not a special-cased stub. */
    TEST_CHECK(hal_kv_init_partition("p1") == HAL_OK, "test setup: fill partition slot 1");
    TEST_CHECK(hal_kv_init_partition("p2") == HAL_OK, "test setup: fill partition slot 2");
    TEST_CHECK(hal_kv_init_partition("p3") == HAL_OK, "test setup: fill partition slot 3");
    TEST_CHECK(hal_kv_init_partition("p4") == HAL_OK, "test setup: fill partition slot 4 (all full now)");

    esp_err_t err = setup_wizard_progress_start();
    TEST_CHECK(err == ESP_OK, "start() against a failed partition init still returns ESP_OK (non-fatal)");
    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        setup_wizard_step_t s;
        setup_wizard_progress_get_step(i, &s);
        TEST_CHECK(s.state == SETUP_WIZ_STEP_PENDING, "partition-init failure: every step reads the safe PENDING default");
    }
}

static void test_corrupt_blob_size_falls_back_to_defaults(void)
{
    reset();
    setup_wizard_progress_start();

    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NVS_PARTITION);
    uint8_t wrong_size_blob[3] = { 1, 2, 3 };
    hal_kv_set_blob(&h, NVS_KEY_PROGRESS, wrong_size_blob, sizeof(wrong_size_blob));
    hal_kv_commit(&h);
    hal_kv_close(&h);

    apply_defaults();
    esp_err_t err = setup_wizard_progress_start();
    TEST_CHECK(err == ESP_OK, "start() against a wrong-size blob still returns ESP_OK (non-fatal)");
    setup_wizard_step_t s0;
    setup_wizard_progress_get_step(0, &s0);
    TEST_CHECK(s0.state == SETUP_WIZ_STEP_PENDING, "wrong-size blob: falls back to PENDING default");
}

// ---------------------------------------------------------------------
// Wire-format name <-> enum round trip (the endpoint's JSON shape)
// ---------------------------------------------------------------------

static void test_state_name_round_trip(void)
{
    setup_wizard_step_state_t states[] = { SETUP_WIZ_STEP_PENDING, SETUP_WIZ_STEP_DONE, SETUP_WIZ_STEP_SKIPPED };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        const char *name = setup_wizard_step_state_name(states[i]);
        setup_wizard_step_state_t back;
        TEST_CHECK(setup_wizard_step_state_from_name(name, &back), "state_from_name accepts state_name's own output");
        TEST_CHECK(back == states[i], "round trip through the wire-format name preserves the state");
    }
    setup_wizard_step_state_t dummy;
    TEST_CHECK(!setup_wizard_step_state_from_name("regressed", &dummy),
               "\"regressed\" is a computed effective state, never a valid STORED state name");
    TEST_CHECK(!setup_wizard_step_state_from_name("bogus", &dummy), "an unrecognized name is refused");
}

// ---------------------------------------------------------------------
// Readiness precedence -- THE core anti-drift property
// ---------------------------------------------------------------------

static void test_readiness_overrides_stored_regressed(void)
{
    /* Stored DONE + readiness NOT ready + applicable -> REGRESSED. */
    setup_wizard_effective_state_t eff =
        setup_wizard_progress_effective_state(SETUP_WIZ_STEP_DONE, /*applicable=*/true, /*ready=*/false);
    TEST_CHECK(eff == SETUP_WIZ_EFFECTIVE_REGRESSED, "stored DONE + readiness not-ready -> REGRESSED, never DONE");

    /* Stored DONE + readiness ready -> DONE stands. */
    eff = setup_wizard_progress_effective_state(SETUP_WIZ_STEP_DONE, true, true);
    TEST_CHECK(eff == SETUP_WIZ_EFFECTIVE_DONE, "stored DONE + readiness ready -> DONE");

    /* Stored DONE, no applicable readiness item (step 4/8/9/11) -> DONE
     * stands, nothing to regress against. */
    eff = setup_wizard_progress_effective_state(SETUP_WIZ_STEP_DONE, false, false);
    TEST_CHECK(eff == SETUP_WIZ_EFFECTIVE_DONE, "no applicable readiness item -> stored DONE is reported as-is");

    /* SKIPPED is never regressed by this rule -- only a DONE claim can be
     * contradicted. */
    eff = setup_wizard_progress_effective_state(SETUP_WIZ_STEP_SKIPPED, true, false);
    TEST_CHECK(eff == SETUP_WIZ_EFFECTIVE_SKIPPED, "stored SKIPPED is not regressed by a not-ready readiness item");

    /* PENDING stands regardless. */
    eff = setup_wizard_progress_effective_state(SETUP_WIZ_STEP_PENDING, true, false);
    TEST_CHECK(eff == SETUP_WIZ_EFFECTIVE_PENDING, "stored PENDING stays PENDING");
}

// THE NEGATIVE TEST this task calls out as the most important one in it:
// prove the precedence rule can actually fail, by making stored state win
// over readiness (the wrong, pre-fix behaviour), and showing a test catch
// it -- see this task's own report for the failing line quoted, and the
// hand restore.
//
// This test is written the way the PRODUCTION rule must behave (readiness
// wins); the accompanying negative-test pass breaks
// setup_wizard_progress_effective_state() itself (flips the precedence so
// stored DONE wins unconditionally) to prove this assertion actually
// catches that regression, then restores the function by hand.
static void test_NEGATIVE_stored_state_must_not_win_over_readiness(void)
{
    setup_wizard_effective_state_t eff =
        setup_wizard_progress_effective_state(SETUP_WIZ_STEP_DONE, /*applicable=*/true, /*ready=*/false);
    TEST_CHECK(eff != SETUP_WIZ_EFFECTIVE_DONE,
               "a wizard reporting DONE while the underlying readiness item is not_done would be a false "
               "all-clear -- this must never happen");
    TEST_CHECK(eff == SETUP_WIZ_EFFECTIVE_REGRESSED, "the correct effective state is REGRESSED, not DONE");
}

void run_test_setup_wizard_progress(void)
{
    test_defaults_on_empty_nvs();
    test_round_trip();
    test_get_all_matches_get_step();
    test_migration_from_v1();
    test_set_step_rejects_out_of_range_index();
    test_set_step_rejects_invalid_state();
    test_partition_init_failure_degrades_to_defaults();
    test_corrupt_blob_size_falls_back_to_defaults();
    test_state_name_round_trip();
    test_readiness_overrides_stored_regressed();
    test_NEGATIVE_stored_state_must_not_win_over_readiness();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
