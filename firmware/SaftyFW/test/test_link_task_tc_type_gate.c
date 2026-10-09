// test_link_task_tc_type_gate.c -- real host test for the REAL
// link_task_tc_type_gate_decide() (tasks/link_task_tc_type_gate.c), added
// per the 2026-09-15 Opus re-review (N1): "Add a real test for the
// heat-safe check and negative-test it against production code." This
// links the real production decision function -- the one link_task.c's
// link_task_heat_is_safe_for_tc_type_change() calls -- not a test-local
// reimplementation.
//
// A neutral, all-safe baseline input is built once and each check flips
// exactly one field away from it, so a failure below points at exactly
// which input the production code stopped honoring.
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "tasks/link_task_tc_type_gate.h"

// Mirrors snapshots.h's CONTEXT_FLAG_* values (the caller's real flag
// constants, passed through as parameters -- this test picks its own
// bit values deliberately different from the real ones' numeric layout so
// a test that accidentally hardcoded a flag value inside the decision
// function itself, instead of using the passed-in parameters, would be
// caught).
#define TEST_FLAG_HEAT_REQUESTED   0x40u
#define TEST_FLAG_PROFILE_RUNNING  0x02u
#define TEST_FLAG_HEAT_OWNER_ACTIVE 0x80u

static link_task_tc_type_gate_input_t safe_baseline(void)
{
    link_task_tc_type_gate_input_t in;
    memset(&in, 0, sizeof(in));
    in.relay_energized = false;
    in.enable_ever_seen = false;
    in.enable_age_ms = 0;
    in.degraded_no_context = false;
    in.context_ever_received = true;
    in.context_lock_available = true;
    in.context_valid = true;
    in.context_age_ms = 100u;
    in.context_max_age_ms = 5000u;
    in.context_flags = 0u;
    in.relay_on_continuous = false;
    in.any_current_present = false;
    return in;
}

static bool decide(const link_task_tc_type_gate_input_t *in)
{
    return link_task_tc_type_gate_decide(in, TEST_FLAG_HEAT_REQUESTED, TEST_FLAG_PROFILE_RUNNING,
                                          TEST_FLAG_HEAT_OWNER_ACTIVE);
}

void run_test_link_task_tc_type_gate(void)
{
    TEST_SECTION("link_task_tc_type_gate_decide(): the real decision link_task's tc-type "
                 "heat-safety gate uses (2026-09-15 Opus re-review N1)");

    link_task_tc_type_gate_input_t in = safe_baseline();
    TEST_CHECK(decide(&in), "the neutral all-safe baseline is accepted");

    // Check 1: hardware ground truth.
    in = safe_baseline();
    in.relay_energized = true;
    TEST_CHECK(!decide(&in), "K4 actually energized right now refuses regardless of anything else");

    // Check 2: recent/pending enable.
    in = safe_baseline();
    in.enable_ever_seen = true;
    in.enable_age_ms = 0;
    TEST_CHECK(!decide(&in), "a REQUEST_ENABLE(true) accepted 0ms ago refuses (relay may not have "
                             "physically closed yet)");

    in = safe_baseline();
    in.enable_ever_seen = true;
    in.enable_age_ms = LINK_TASK_TC_TYPE_GATE_ENABLE_RECENT_SAFE_WINDOW_MS - 1u;
    TEST_CHECK(!decide(&in), "an enable request just under the settle window still refuses");

    in = safe_baseline();
    in.enable_ever_seen = true;
    in.enable_age_ms = LINK_TASK_TC_TYPE_GATE_ENABLE_RECENT_SAFE_WINDOW_MS;
    TEST_CHECK(decide(&in), "an enable request at/past the settle window, with the relay actually "
                            "de-energized (check 1), is accepted");

    in = safe_baseline();
    in.enable_ever_seen = false;
    in.enable_age_ms = 0;
    TEST_CHECK(decide(&in), "never having seen an enable request this boot is the safe case, not "
                            "'unknown'");

    // Check 3: context staleness/availability, fail-closed.
    in = safe_baseline();
    in.degraded_no_context = true;
    TEST_CHECK(!decide(&in), "link_task's own DEGRADED_NO_CONTEXT state refuses");

    in = safe_baseline();
    in.context_ever_received = false;
    TEST_CHECK(!decide(&in), "no context ever received this boot refuses (unknown is not safe)");

    in = safe_baseline();
    in.context_lock_available = false;
    TEST_CHECK(!decide(&in), "could not confirm context freshness (lock contended) refuses");

    in = safe_baseline();
    in.context_valid = false;
    TEST_CHECK(!decide(&in), "a decoded-but-invalid context frame refuses");

    in = safe_baseline();
    in.context_age_ms = in.context_max_age_ms;
    TEST_CHECK(!decide(&in), "context exactly at the max-age boundary refuses (>=, not >)");

    in = safe_baseline();
    in.context_age_ms = in.context_max_age_ms - 1u;
    TEST_CHECK(decide(&in), "context just under the max-age boundary is accepted");

    // Check 3: the ESP-reported "active heat owner" bits, each individually.
    in = safe_baseline();
    in.context_flags = TEST_FLAG_HEAT_REQUESTED;
    TEST_CHECK(!decide(&in), "HEAT_REQUESTED set refuses");

    in = safe_baseline();
    in.context_flags = TEST_FLAG_PROFILE_RUNNING;
    TEST_CHECK(!decide(&in), "PROFILE_RUNNING set refuses");

    in = safe_baseline();
    in.context_flags = TEST_FLAG_HEAT_OWNER_ACTIVE;
    TEST_CHECK(!decide(&in), "HEAT_OWNER_ACTIVE set refuses -- this is the N1 fix itself: covers "
                             "autotune/CT-sweep, a PAUSED firing, and danger-mode heat, none of "
                             "which HEAT_REQUESTED/PROFILE_RUNNING alone would catch");

    in = safe_baseline();
    in.context_flags = (uint8_t)(0xFFu & ~(uint8_t)(TEST_FLAG_HEAT_REQUESTED |
                                                     TEST_FLAG_PROFILE_RUNNING |
                                                     TEST_FLAG_HEAT_OWNER_ACTIVE));
    TEST_CHECK(decide(&in), "an unrelated flag bit set (not one of the three unsafe ones) is accepted");

    // Additional Pico-local heuristics.
    in = safe_baseline();
    in.relay_on_continuous = true;
    TEST_CHECK(!decide(&in), "s_relay_on_continuous refuses even with every ESP-reported signal clean");

    in = safe_baseline();
    in.any_current_present = true;
    TEST_CHECK(!decide(&in), "current-sense presence refuses even with every ESP-reported signal clean");

    // NULL input: fail closed, never crash.
    TEST_CHECK(!link_task_tc_type_gate_decide(NULL, TEST_FLAG_HEAT_REQUESTED,
                                               TEST_FLAG_PROFILE_RUNNING,
                                               TEST_FLAG_HEAT_OWNER_ACTIVE),
               "a NULL input pointer refuses rather than crashing");
}
