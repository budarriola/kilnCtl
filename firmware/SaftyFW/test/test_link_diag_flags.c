// Host tests for firmware/SaftyFW/src/link_diag_flags.c -- proves Frame B
// (SAFETY_CMD_DIAG)'s calibration_missing bit follows the boolean passed in
// (which link_task.c now sources from config_store_is_calibration_missing()
// -- TODO.md Phase 8), and (2026-09-16) that clear_trip_diag_present (bit3)
// follows its own argument the same way (link_task.c sources it from
// clear_trip_diag_get_cached().magic_ok). No pico-sdk/FreeRTOS dependency,
// same discipline as the other *_policy.c host tests in this directory.
//
// 2026-09-22: added bit5/bit6 (S1_ABS_MAX_TEMP_DISABLED/S8_RATE_GUARD_
// DISABLED) coverage, same shape as bit3/bit4 above -- SAFETY_MODEL.md's
// "guards with no defensible default ship disabled, and say so in
// telemetry" line was unchecked for S1/S8 until this pass wired a distinct
// signal for each.
#include "test_common.h"

#include "../src/link_diag_flags.h"
#include "kilnlink/kilnlink_diag.h"

static void test_calibration_missing_bit_follows_the_argument(void)
{
    TEST_SECTION("calibration_missing bit follows the record, not hard-coded to 1");

    uint8_t flags_true =
        link_diag_flags_compute(/*calibration_missing=*/true, false, false, false, false, false, false);
    uint8_t flags_false =
        link_diag_flags_compute(/*calibration_missing=*/false, false, false, false, false, false, false);

    TEST_CHECK((flags_true & KILNLINK_DIAG_FLAG_CALIBRATION_MISSING) != 0u,
               "calibration_missing=true -> bit set");
    TEST_CHECK((flags_false & KILNLINK_DIAG_FLAG_CALIBRATION_MISSING) == 0u,
               "calibration_missing=false -> bit CLEAR -- this is the actual fix: before it, "
               "the bit was hard-coded to 1 regardless of this argument");
}

static void test_sim_context_seen_bit_independent(void)
{
    TEST_SECTION("sim_context_seen bit is independent of calibration_missing, both directions");

    uint8_t both_false = link_diag_flags_compute(false, false, false, false, false, false, false);
    uint8_t sim_only = link_diag_flags_compute(false, true, false, false, false, false, false);
    uint8_t cal_only = link_diag_flags_compute(true, false, false, false, false, false, false);
    uint8_t both_true = link_diag_flags_compute(true, true, false, false, false, false, false);

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

    uint8_t flags = link_diag_flags_compute(true, true, true, false, false, false, false);
    TEST_CHECK((flags & (uint8_t)KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT) == 0u,
               "bit2 stays clear even with every other input true");
}

static void test_clear_trip_diag_present_bit_follows_the_argument(void)
{
    TEST_SECTION("clear_trip_diag_present (bit3) bit follows the argument, not hard-coded");

    uint8_t flags_true = link_diag_flags_compute(false, false, /*clear_trip_diag_present=*/true,
                                                  false, false, false, false);
    uint8_t flags_false = link_diag_flags_compute(false, false, /*clear_trip_diag_present=*/false,
                                                   false, false, false, false);

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

    uint8_t none = link_diag_flags_compute(false, false, false, false, false, false, false);
    uint8_t clear_trip_only = link_diag_flags_compute(false, false, true, false, false, false, false);
    uint8_t all_three = link_diag_flags_compute(true, true, true, false, false, false, false);

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
        link_diag_flags_compute(false, false, false, /*tc_reconfig_gave_up=*/true, false, false, false);
    uint8_t flags_false =
        link_diag_flags_compute(false, false, false, /*tc_reconfig_gave_up=*/false, false, false, false);

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

    uint8_t none = link_diag_flags_compute(false, false, false, false, false, false, false);
    uint8_t gave_up_only = link_diag_flags_compute(false, false, false, true, false, false, false);
    uint8_t all_four = link_diag_flags_compute(true, true, true, true, false, false, false);

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

static void test_abs_max_temp_disabled_bit_follows_the_argument(void)
{
    TEST_SECTION("abs_max_temp_disabled (bit5, S1) bit follows the argument, not hard-coded");

    uint8_t flags_true = link_diag_flags_compute(false, false, false, false,
                                                  /*abs_max_temp_disabled=*/true, false, false);
    uint8_t flags_false = link_diag_flags_compute(false, false, false, false,
                                                   /*abs_max_temp_disabled=*/false, false, false);

    TEST_CHECK((flags_true & (uint8_t)KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED) != 0u,
               "abs_max_temp_disabled=true -> bit5 set -- this is the actual fix: before it, "
               "S1 shipping disabled-by-zero (abs_max_temp_c == 0) had no distinct wire signal "
               "at all, only the bundled CALIBRATION_MISSING commissioning summary");
    TEST_CHECK((flags_false & (uint8_t)KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED) == 0u,
               "abs_max_temp_disabled=false -> bit5 CLEAR");
}

static void test_rate_guard_disabled_bit_follows_the_argument(void)
{
    TEST_SECTION("rate_guard_disabled (bit6, S8) bit follows the argument, not hard-coded");

    uint8_t flags_true = link_diag_flags_compute(false, false, false, false, false,
                                                  /*rate_guard_disabled=*/true, false);
    uint8_t flags_false = link_diag_flags_compute(false, false, false, false, false,
                                                   /*rate_guard_disabled=*/false, false);

    TEST_CHECK((flags_true & (uint8_t)KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED) != 0u,
               "rate_guard_disabled=true -> bit6 set -- same fix as bit5, for S8 "
               "(max_rate_c_per_min == 0) instead of S1");
    TEST_CHECK((flags_false & (uint8_t)KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED) == 0u,
               "rate_guard_disabled=false -> bit6 CLEAR");
}

static void test_s1_s8_disabled_bits_independent(void)
{
    TEST_SECTION("abs_max_temp_disabled (bit5) and rate_guard_disabled (bit6) are independent "
                 "of each other and of the other four inputs, both directions");

    uint8_t none = link_diag_flags_compute(false, false, false, false, false, false, false);
    uint8_t s1_only = link_diag_flags_compute(false, false, false, false, true, false, false);
    uint8_t s8_only = link_diag_flags_compute(false, false, false, false, false, true, false);
    uint8_t all_six = link_diag_flags_compute(true, true, true, true, true, true, false);

    TEST_CHECK(none == 0u, "no flag -> flags byte is 0");
    TEST_CHECK(s1_only == (uint8_t)KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED,
               "abs_max_temp_disabled alone -> only bit5");
    TEST_CHECK(s8_only == (uint8_t)KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED,
               "rate_guard_disabled alone -> only bit6");
    TEST_CHECK(all_six == (uint8_t)(KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN |
                                      KILNLINK_DIAG_FLAG_CALIBRATION_MISSING |
                                      KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT |
                                      KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP |
                                      KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED |
                                      KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED),
               "all six set -> exactly those six bits, bit2 (estop_unwired_suspect) still "
               "untouched");
}

static void test_config_volatile_dirty_bit_follows_the_argument(void)
{
    TEST_SECTION("config_volatile_dirty (bit7) bit follows the argument, not hard-coded");

    uint8_t flags_true = link_diag_flags_compute(false, false, false, false, false, false,
                                                  /*config_volatile_dirty=*/true);
    uint8_t flags_false = link_diag_flags_compute(false, false, false, false, false, false,
                                                   /*config_volatile_dirty=*/false);

    TEST_CHECK((flags_true & (uint8_t)KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY) != 0u,
               "config_volatile_dirty=true -> bit7 set -- this is the fix: before it, a RAM-only "
               "config_store_write_volatile() install that bumped config_version had no distinct "
               "wire signal telling a reader the bump would not survive a reboot");
    TEST_CHECK((flags_false & (uint8_t)KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY) == 0u,
               "config_volatile_dirty=false -> bit7 CLEAR");
}

static void test_config_volatile_dirty_bit_independent(void)
{
    TEST_SECTION("config_volatile_dirty (bit7) is independent of the other six inputs, both "
                 "directions");

    uint8_t none = link_diag_flags_compute(false, false, false, false, false, false, false);
    uint8_t dirty_only = link_diag_flags_compute(false, false, false, false, false, false, true);
    uint8_t all_seven = link_diag_flags_compute(true, true, true, true, true, true, true);

    TEST_CHECK(none == 0u, "no flag -> flags byte is 0");
    TEST_CHECK(dirty_only == (uint8_t)KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY,
               "config_volatile_dirty alone -> only bit7");
    TEST_CHECK(all_seven == (uint8_t)(KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN |
                                        KILNLINK_DIAG_FLAG_CALIBRATION_MISSING |
                                        KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT |
                                        KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP |
                                        KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED |
                                        KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED |
                                        KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY),
               "all seven set -> exactly those seven bits, bit2 (estop_unwired_suspect) still "
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
    test_abs_max_temp_disabled_bit_follows_the_argument();
    test_rate_guard_disabled_bit_follows_the_argument();
    test_s1_s8_disabled_bits_independent();
    test_config_volatile_dirty_bit_follows_the_argument();
    test_config_volatile_dirty_bit_independent();
}
