// Host tests for firmware/SaftyFW/src/link_diag_flags.c -- proves Frame B
// (SAFETY_CMD_DIAG)'s calibration_missing bit follows the boolean passed in
// (which link_task.c now sources from config_store_is_calibration_missing()
// -- TODO.md Phase 8). No pico-sdk/FreeRTOS dependency, same discipline as
// the other *_policy.c host tests in this directory.
#include "test_common.h"

#include "../src/link_diag_flags.h"
#include "kilnlink/kilnlink_diag.h"

static void test_calibration_missing_bit_follows_the_argument(void)
{
    TEST_SECTION("calibration_missing bit follows the record, not hard-coded to 1");

    uint8_t flags_true = link_diag_flags_compute(/*calibration_missing=*/true, false);
    uint8_t flags_false = link_diag_flags_compute(/*calibration_missing=*/false, false);

    TEST_CHECK((flags_true & KILNLINK_DIAG_FLAG_CALIBRATION_MISSING) != 0u,
               "calibration_missing=true -> bit set");
    TEST_CHECK((flags_false & KILNLINK_DIAG_FLAG_CALIBRATION_MISSING) == 0u,
               "calibration_missing=false -> bit CLEAR -- this is the actual fix: before it, "
               "the bit was hard-coded to 1 regardless of this argument");
}

static void test_sim_context_seen_bit_independent(void)
{
    TEST_SECTION("sim_context_seen bit is independent of calibration_missing, both directions");

    uint8_t both_false = link_diag_flags_compute(false, false);
    uint8_t sim_only = link_diag_flags_compute(false, true);
    uint8_t cal_only = link_diag_flags_compute(true, false);
    uint8_t both_true = link_diag_flags_compute(true, true);

    TEST_CHECK(both_false == 0u, "neither flag -> flags byte is 0");
    TEST_CHECK(sim_only == (uint8_t)KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN,
               "sim_context_seen alone -> only that bit");
    TEST_CHECK(cal_only == (uint8_t)KILNLINK_DIAG_FLAG_CALIBRATION_MISSING,
               "calibration_missing alone -> only that bit");
    TEST_CHECK(both_true == (uint8_t)(KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN |
                                        KILNLINK_DIAG_FLAG_CALIBRATION_MISSING),
               "both set -> both bits, nothing else touched");
}

static void test_estop_unwired_suspect_never_set(void)
{
    TEST_SECTION("estop_unwired_suspect (bit2) is never set -- no detection heuristic exists");

    uint8_t flags = link_diag_flags_compute(true, true);
    TEST_CHECK((flags & (uint8_t)KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT) == 0u,
               "bit2 stays clear even with both other inputs true");
}

void run_test_link_diag_flags(void)
{
    test_calibration_missing_bit_follows_the_argument();
    test_sim_context_seen_bit_independent();
    test_estop_unwired_suspect_never_set();
}
