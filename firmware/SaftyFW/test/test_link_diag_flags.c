// Host tests for firmware/SaftyFW/src/link_diag_flags.c -- proves Frame B
// (SAFETY_CMD_DIAG)'s calibration_missing bit follows the boolean passed in
// (which link_task.c now sources from config_store_is_calibration_missing()
// -- TODO.md Phase 8), and (2026-09-16) that clear_trip_diag_present (bit3)
// follows its own argument the same way (link_task.c sources it from
// clear_trip_diag_get_cached().magic_ok). No pico-sdk/FreeRTOS dependency,
// same discipline as the other *_policy.c host tests in this directory.
#include "test_common.h"

#include "../src/link_diag_flags.h"
#include "kilnlink/kilnlink_diag.h"

static void test_calibration_missing_bit_follows_the_argument(void)
{
    TEST_SECTION("calibration_missing bit follows the record, not hard-coded to 1");

    uint8_t flags_true = link_diag_flags_compute(/*calibration_missing=*/true, false, false, false);
    uint8_t flags_false = link_diag_flags_compute(/*calibration_missing=*/false, false, false, false);

    TEST_CHECK((flags_true & KILNLINK_DIAG_FLAG_CALIBRATION_MISSING) != 0u,
               "calibration_missing=true -> bit set");
    TEST_CHECK((flags_false & KILNLINK_DIAG_FLAG_CALIBRATION_MISSING) == 0u,
               "calibration_missing=false -> bit CLEAR -- this is the actual fix: before it, "
               "the bit was hard-coded to 1 regardless of this argument");
}

static void test_sim_context_seen_bit_independent(void)
{
    TEST_SECTION("sim_context_seen bit is independent of calibration_missing, both directions");

    uint8_t both_false = link_diag_flags_compute(false, false, false, false);
    uint8_t sim_only = link_diag_flags_compute(false, true, false, false);
    uint8_t cal_only = link_diag_flags_compute(true, false, false, false);
    uint8_t both_true = link_diag_flags_compute(true, true, false, false);

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

    uint8_t flags = link_diag_flags_compute(true, true, true, false);
    TEST_CHECK((flags & (uint8_t)KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT) == 0u,
               "bit2 stays clear even with every other input true");
}

static void test_clear_trip_diag_present_bit_follows_the_argument(void)
{
    TEST_SECTION("clear_trip_diag_present (bit3) bit follows the argument, not hard-coded");

    uint8_t flags_true = link_diag_flags_compute(false, false, /*clear_trip_diag_present=*/true, false);
    uint8_t flags_false = link_diag_flags_compute(false, false, /*clear_trip_diag_present=*/false, false);

    TEST_CHECK((flags_true & (uint8_t)KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT) != 0u,
               "clear_trip_diag_present=true -> bit3 set -- this is the actual fix: before it, "
               "clear_trip_diag was read and cached at boot but never reached this frame at "
               "all (main.c's TODO said so explicitly)");
    TEST_CHECK((flags_false & (uint8_t)KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT) == 0u,
               "clear_trip_diag_present=false -> bit3 CLEAR");
}

static void test_clear_trip_diag_present_bit_independent(void)
{
    TEST_SECTION("clear_trip_diag_present (bit3) is independent of the other two inputs, both "
                 "directions");

    uint8_t none = link_diag_flags_compute(false, false, false, false);
    uint8_t clear_trip_only = link_diag_flags_compute(false, false, true, false);
    uint8_t all_three = link_diag_flags_compute(true, true, true, false);

    TEST_CHECK(none == 0u, "no flag -> flags byte is 0");
    TEST_CHECK(clear_trip_only == (uint8_t)KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT,
               "clear_trip_diag_present alone -> only bit3");
    TEST_CHECK(all_three == (uint8_t)(KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN |
                                        KILNLINK_DIAG_FLAG_CALIBRATION_MISSING |
                                        KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT),
               "all three set -> exactly those three bits, bit2 (estop_unwired_suspect) still "
               "untouched");
}

static void test_tc_reconfig_gave_up_bit_follows_the_argument(void)
{
    TEST_SECTION("tc_reconfig_gave_up (bit4) bit follows the argument, not hard-coded");

    uint8_t flags_true =
        link_diag_flags_compute(false, false, false, /*tc_reconfig_gave_up=*/true);
    uint8_t flags_false =
        link_diag_flags_compute(false, false, false, /*tc_reconfig_gave_up=*/false);

    TEST_CHECK((flags_true & (uint8_t)KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP) != 0u,
               "tc_reconfig_gave_up=true -> bit4 set -- this is the actual fix: before it, "
               "thermo_task.c's s_reconfig_gave_up never reached this frame at all, only "
               "SWD could see it");
    TEST_CHECK((flags_false & (uint8_t)KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP) == 0u,
               "tc_reconfig_gave_up=false -> bit4 CLEAR");
}

static void test_tc_reconfig_gave_up_bit_independent(void)
{
    TEST_SECTION("tc_reconfig_gave_up (bit4) is independent of the other three inputs, both "
                 "directions");

    uint8_t none = link_diag_flags_compute(false, false, false, false);
    uint8_t gave_up_only = link_diag_flags_compute(false, false, false, true);
    uint8_t all_four = link_diag_flags_compute(true, true, true, true);

    TEST_CHECK(none == 0u, "no flag -> flags byte is 0");
    TEST_CHECK(gave_up_only == (uint8_t)KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP,
               "tc_reconfig_gave_up alone -> only bit4");
    TEST_CHECK(all_four == (uint8_t)(KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN |
                                       KILNLINK_DIAG_FLAG_CALIBRATION_MISSING |
                                       KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT |
                                       KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP),
               "all four set -> exactly those four bits, bit2 (estop_unwired_suspect) still "
               "untouched");
}

void run_test_link_diag_flags(void)
{
    test_calibration_missing_bit_follows_the_argument();
    test_sim_context_seen_bit_independent();
    test_estop_unwired_suspect_never_set();
    test_clear_trip_diag_present_bit_follows_the_argument();
    test_clear_trip_diag_present_bit_independent();
    test_tc_reconfig_gave_up_bit_follows_the_argument();
    test_tc_reconfig_gave_up_bit_independent();
}
