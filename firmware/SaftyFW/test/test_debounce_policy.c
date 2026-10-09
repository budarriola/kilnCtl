// Host tests for debounce_policy.h -- the pure consecutive-sample debounce
// extracted from discrete_task.c's static debounce_update() (2026-09-04,
// closing GUARD_TEST_MATRIX.md section 1's S6a/S7 gap; see debounce_policy.h
// for the full extraction rationale).
//
// These tests exercise the generic debounce mechanism directly, in sample
// counts (this module has no notion of time). test/test_guard_nuisance.c's
// test_s6a_/test_s7_ functions layer discrete_task.c's real ms->sample-count
// conversion on top and assert the *named* GUARD_TEST_MATRIX.md scenarios
// (a 100ms mainFault glitch, a 30ms E-stop bounce); this file proves the
// underlying mechanism is correct in general.
#include "test_common.h"
#include "../src/debounce_policy.h"

static void test_glitch_shorter_than_window_does_not_publish(void)
{
    TEST_SECTION("debounce_policy -- a run of raw=true shorter than n_samples never publishes true");

    debounce_policy_state_t db = { .published = false };
    const uint32_t n = 5;

    /* 4 consecutive true samples, one short of the 5 required, then back to
     * false. published must stay false the whole time. */
    for (int i = 0; i < 4; i++) {
        bool out = debounce_policy_update(&db, true, n);
        TEST_CHECK(!out, "a streak of true samples shorter than n_samples must not publish true yet");
    }
    bool out = debounce_policy_update(&db, false, n);
    TEST_CHECK(!out, "reverting to false before reaching n_samples must never have published true");
}

static void test_sustained_assertion_is_detected(void)
{
    TEST_SECTION("debounce_policy -- POSITIVE CONTROL: a genuinely sustained raw=true IS published true");

    debounce_policy_state_t db = { .published = false };
    const uint32_t n = 5;

    bool out = false;
    for (int i = 0; i < 5; i++) {
        out = debounce_policy_update(&db, true, n);
    }
    TEST_CHECK(out, "n_samples consecutive true readings must publish true -- a debounce that never "
                    "asserts at all would wrongly pass the glitch-rejection test above");
}

static void test_streak_resets_on_disagreement(void)
{
    TEST_SECTION("debounce_policy -- a single disagreeing sample resets the streak against the new value");

    debounce_policy_state_t db = { .published = false };
    const uint32_t n = 5;

    for (int i = 0; i < 4; i++) {
        debounce_policy_update(&db, true, n);
    }
    /* One false sample here should NOT let a later run of true "borrow"
     * the 4 samples already accumulated. */
    debounce_policy_update(&db, false, n);
    bool out = false;
    for (int i = 0; i < 4; i++) {
        out = debounce_policy_update(&db, true, n);
    }
    TEST_CHECK(!out, "4 more true samples after a reset is still one short of n_samples=5 -- "
                     "must not have published yet (streak did not carry over the interruption)");
    out = debounce_policy_update(&db, true, n);
    TEST_CHECK(out, "the 5th consecutive true sample after the reset does publish");
}

static void test_published_value_holds_until_a_new_streak_completes(void)
{
    TEST_SECTION("debounce_policy -- published stays at the old value through a short-lived opposite streak");

    debounce_policy_state_t db = { .published = false };
    const uint32_t n = 3;
    for (int i = 0; i < 3; i++) {
        debounce_policy_update(&db, true, n);
    }
    /* published is now true. A 2-sample false glitch (one short of n=3)
     * must not flip it back. */
    bool out = debounce_policy_update(&db, false, n);
    TEST_CHECK(out, "a single false sample must not immediately un-publish a settled true");
    out = debounce_policy_update(&db, false, n);
    TEST_CHECK(out, "two consecutive false samples, still short of n_samples=3, must not un-publish either");
    out = debounce_policy_update(&db, false, n);
    TEST_CHECK(!out, "the 3rd consecutive false sample does complete the streak and un-publish");
}

void run_test_debounce_policy(void)
{
    test_glitch_shorter_than_window_does_not_publish();
    test_sustained_assertion_is_detected();
    test_streak_resets_on_disagreement();
    test_published_value_holds_until_a_new_streak_completes();
}
