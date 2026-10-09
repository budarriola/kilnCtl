// Host test for App/drivers/safety/heat_owner_active_decide.c.
//
// 2026-09-15 (Opus adversarial re-review, F6): the HEAT_OWNER_ACTIVE flag
// producer (safety_build_and_send_context() in safety_link_frames.c) had no
// test at all -- its one compiling host executable, test_safety_link_compile
// (which links safety_link_frame.c, singular, NOT safety_link_frames.c,
// plural -- confirmed by grep, so this new file adds no link-time overlap
// with that executable), only got there via
// fake_danger_mode_for_safety_link.c, which fakes both
// heat_enable_is_held() and danger_mode_active() fixed false. Nothing
// anywhere asserted the flag actually flips true for a PAUSED firing, a
// held profile/autotune claimant, or danger mode -- the exact non-RUNNING
// cases N1's Pico-side gate exists to cover.
//
// heat_owner_active_decide() was pulled out of safety_link_frames.c as a
// pure function (state + three bools -> bool) specifically so it could be
// host-tested directly without linking heat_enable.c, danger_mode.c or
// profile_executor.c (all out of scope for this pass). This file includes
// the real production .c directly, same convention as the other single-
// function host tests in this directory.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/safety/heat_owner_active_decide.c"

static void test_running_is_active(void)
{
    TEST_SECTION("heat_owner_active_decide -- RUNNING alone is active");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_RUNNING, false, false, false) == true,
               "PROFILE_EXEC_RUNNING with no claimants and no danger mode is still active");
}

static void test_paused_is_active(void)
{
    TEST_SECTION("heat_owner_active_decide -- PAUSED alone is active (F6)");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_PAUSED, false, false, false) == true,
               "a PAUSED firing still owns heat (relays may resume without a fresh claim)");
}

static void test_profile_claimant_held_is_active(void)
{
    TEST_SECTION("heat_owner_active_decide -- profile claimant held alone is active (F6)");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_IDLE, true, false, false) == true,
               "IDLE executor state with the profile heat-enable claimant held is still active");
}

static void test_autotune_claimant_held_is_active(void)
{
    TEST_SECTION("heat_owner_active_decide -- autotune claimant held alone is active (F6)");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_IDLE, false, true, false) == true,
               "IDLE executor state with the autotune heat-enable claimant held is still active");
}

static void test_danger_mode_is_active(void)
{
    TEST_SECTION("heat_owner_active_decide -- danger mode alone is active (F6)");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_IDLE, false, false, true) == true,
               "IDLE executor state with danger mode active is still active");
}

static void test_all_false_is_inactive(void)
{
    TEST_SECTION("heat_owner_active_decide -- IDLE/DONE/FAULTED with no claimants, no danger mode is inactive");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_IDLE, false, false, false) == false,
               "IDLE, nothing held, no danger mode -> not active");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_DONE, false, false, false) == false,
               "DONE, nothing held, no danger mode -> not active");
    TEST_CHECK(heat_owner_active_decide(PROFILE_EXEC_FAULTED, false, false, false) == false,
               "FAULTED, nothing held, no danger mode -> not active");
}

int main(void)
{
    test_running_is_active();
    test_paused_is_active();
    test_profile_claimant_held_is_active();
    test_autotune_claimant_held_is_active();
    test_danger_mode_is_active();
    test_all_false_is_inactive();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
