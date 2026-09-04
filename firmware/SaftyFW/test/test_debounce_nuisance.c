// Closes GUARD_TEST_MATRIX.md section 1's last two open host-coverable
// rows -- S6a ("mainFault glitching for 100ms" must not trip) and S7
// ("30ms of contact bounce" must not trip) -- against the real, extracted
// debounce logic (src/debounce_policy.c, moved verbatim out of
// discrete_task.c's static debounce_update(); see debounce_policy.h for the
// full extraction history). A sibling file to test_guard_nuisance.c/
// test_debounce_policy.c rather than an addition to either: it needs its
// own window/sample-count arithmetic, which belongs together rather than
// folded into either existing file's scope.
//
// The window/period constants themselves (SAFTYFW_ESTOP_DEBOUNCE_MS/
// SAFTYFW_MAIN_FAULT_DEBOUNCE_MS/SAFTYFW_DEBOUNCE_SAMPLES()) are #included
// directly from debounce_policy.h -- the same source of truth
// discrete_task.c itself now uses (2026-09-04) -- rather than hand-copied
// here, so a window/period change cannot silently desync this file from the
// real firmware. Only the task period is still a local constant
// (task_priorities.h's SAFTYFW_PERIOD_DISCRETE_TASK_MS is a FreeRTOS-adjacent
// header not part of this test's link set, see debounce_policy.h), kept in
// one place below and named so a mismatch against the firmware is easy to
// spot.
#include "test_common.h"
#include "../src/debounce_policy.h"

#define TEST_DISCRETE_TASK_PERIOD_MS      10u  // task_priorities.h: SAFTYFW_PERIOD_DISCRETE_TASK_MS
#define TEST_DEBOUNCE_SAMPLES(window_ms) \
    SAFTYFW_DEBOUNCE_SAMPLES((window_ms), TEST_DISCRETE_TASK_PERIOD_MS)

// Feeds `ms` worth of raw=asserted samples (at the 10ms task period), then
// `settle_ms` worth of raw=healthy samples, and returns whether `published`
// was ever true at any point during the whole run. This models one glitch
// or bounce episode followed by the line returning healthy and staying
// there -- exactly S6a/S7's nuisance scenario, not a sustained fault.
static bool run_glitch_episode(uint32_t glitch_ms, uint32_t settle_ms, uint32_t n_samples,
                                bool *out_final_published)
{
    debounce_policy_state_t db = { .published = false };
    bool ever_published = false;

    uint32_t glitch_samples = glitch_ms / TEST_DISCRETE_TASK_PERIOD_MS;
    for (uint32_t i = 0; i < glitch_samples; i++) {
        if (debounce_policy_update(&db, true, n_samples)) {
            ever_published = true;
        }
    }
    uint32_t settle_samples = settle_ms / TEST_DISCRETE_TASK_PERIOD_MS;
    for (uint32_t i = 0; i < settle_samples; i++) {
        if (debounce_policy_update(&db, false, n_samples)) {
            ever_published = true;
        }
    }

    if (out_final_published) {
        *out_final_published = db.published;
    }
    return ever_published;
}

static void test_s6a_100ms_main_fault_glitch_does_not_trip(void)
{
    TEST_SECTION("S6a -- a 100ms mainFault glitch must NOT trip (GUARD_TEST_MATRIX.md section 1)");

    uint32_t n = TEST_DEBOUNCE_SAMPLES(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS); /* 20 samples @ 200ms/10ms */
    TEST_CHECK(n == 20u, "sanity: mainFault's 200ms window at a 10ms task period is 20 samples");

    bool final_published = true;
    bool ever_published = run_glitch_episode(100u, 500u, n, &final_published);
    TEST_CHECK(!ever_published, "a 100ms mainFault glitch (10 of the 20 required samples) must never "
                                 "reach 'published asserted' at any point during the episode");
    TEST_CHECK(!final_published, "and mainFault must read healthy again once the glitch clears");
}

static void test_s6a_sustained_main_fault_is_detected(void)
{
    TEST_SECTION("S6a -- POSITIVE CONTROL: a genuinely sustained mainFault (>=200ms) IS detected");

    uint32_t n = TEST_DEBOUNCE_SAMPLES(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS);
    /* 210ms sustained -- comfortably past the 200ms window, unlike the
     * glitch case above. */
    bool final_published = false;
    bool ever_published = run_glitch_episode(210u, 0u, n, &final_published);
    TEST_CHECK(ever_published, "a mainFault held for 210ms (more than the 200ms window) must be "
                                "detected -- a debounce that never asserts at all would wrongly pass "
                                "the glitch-rejection test above, so this control must fail loud if "
                                "the debounce is broken that way");
    TEST_CHECK(final_published, "and it must still read asserted at the end of the sustained hold");
}

static void test_s7_30ms_estop_bounce_does_not_trip(void)
{
    TEST_SECTION("S7 -- 30ms of E-stop contact bounce must NOT trip (GUARD_TEST_MATRIX.md section 1)");

    uint32_t n = TEST_DEBOUNCE_SAMPLES(SAFTYFW_ESTOP_DEBOUNCE_MS); /* 5 samples @ 50ms/10ms */
    TEST_CHECK(n == 5u, "sanity: E-stop's 50ms window at a 10ms task period is 5 samples");

    bool final_published = true;
    bool ever_published = run_glitch_episode(30u, 200u, n, &final_published);
    TEST_CHECK(!ever_published, "a 30ms E-stop bounce (3 of the 5 required samples) must never reach "
                                 "'published asserted' at any point during the episode");
    TEST_CHECK(!final_published, "and E-stop must read healthy again once the bounce settles");
}

static void test_s7_sustained_estop_press_is_detected(void)
{
    TEST_SECTION("S7 -- POSITIVE CONTROL: a genuine sustained E-stop press (>=50ms) IS detected");

    uint32_t n = TEST_DEBOUNCE_SAMPLES(SAFTYFW_ESTOP_DEBOUNCE_MS);
    /* 60ms sustained -- comfortably past the 50ms window. */
    bool final_published = false;
    bool ever_published = run_glitch_episode(60u, 0u, n, &final_published);
    TEST_CHECK(ever_published, "an E-stop held for 60ms (more than the 50ms window) must be detected");
    TEST_CHECK(final_published, "and it must still read asserted at the end of the sustained hold");
}

// The ms-domain boundary itself, exactly. test_debounce_policy.c already
// proves "publishes at exactly n samples, not n-1" in the sample-count
// domain; nothing before this exercised that same boundary in the ms domain
// a reader of the S6a/S7 nuisance tests above would actually care about --
// 210ms/60ms and 100ms/30ms both sit comfortably clear of it. These four
// cases pin the edge exactly, using the real millisecond windows
// (SAFTYFW_MAIN_FAULT_DEBOUNCE_MS/SAFTYFW_ESTOP_DEBOUNCE_MS) rather than a
// sample count chosen to demonstrate the arithmetic.
static void test_s6a_exact_200ms_boundary(void)
{
    TEST_SECTION("S6a -- exact ms-domain boundary: 200ms publishes, 190ms does not "
                 "(GUARD_TEST_MATRIX.md section 1)");

    uint32_t n = TEST_DEBOUNCE_SAMPLES(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS);

    bool final_published_at_boundary = false;
    bool ever_published_at_boundary =
        run_glitch_episode(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS, 0u, n, &final_published_at_boundary);
    TEST_CHECK(ever_published_at_boundary,
               "mainFault held for exactly 200ms (the real SAFTYFW_MAIN_FAULT_DEBOUNCE_MS) must publish");
    TEST_CHECK(final_published_at_boundary, "and read asserted at the end of exactly 200ms");

    bool final_published_short = true;
    bool ever_published_short = run_glitch_episode(SAFTYFW_MAIN_FAULT_DEBOUNCE_MS - TEST_DISCRETE_TASK_PERIOD_MS,
                                                     500u, n, &final_published_short);
    TEST_CHECK(!ever_published_short,
               "mainFault held for one sample short of 200ms (190ms) must never publish");
    TEST_CHECK(!final_published_short, "and must read healthy once it settles");
}

static void test_s7_exact_50ms_boundary(void)
{
    TEST_SECTION("S7 -- exact ms-domain boundary: 50ms publishes, 40ms does not "
                 "(GUARD_TEST_MATRIX.md section 1)");

    uint32_t n = TEST_DEBOUNCE_SAMPLES(SAFTYFW_ESTOP_DEBOUNCE_MS);

    bool final_published_at_boundary = false;
    bool ever_published_at_boundary =
        run_glitch_episode(SAFTYFW_ESTOP_DEBOUNCE_MS, 0u, n, &final_published_at_boundary);
    TEST_CHECK(ever_published_at_boundary,
               "E-stop held for exactly 50ms (the real SAFTYFW_ESTOP_DEBOUNCE_MS) must publish");
    TEST_CHECK(final_published_at_boundary, "and read asserted at the end of exactly 50ms");

    bool final_published_short = true;
    bool ever_published_short = run_glitch_episode(SAFTYFW_ESTOP_DEBOUNCE_MS - TEST_DISCRETE_TASK_PERIOD_MS,
                                                     200u, n, &final_published_short);
    TEST_CHECK(!ever_published_short,
               "E-stop held for one sample short of 50ms (40ms) must never publish");
    TEST_CHECK(!final_published_short, "and must read healthy once it settles");
}

void run_test_debounce_nuisance(void)
{
    test_s6a_100ms_main_fault_glitch_does_not_trip();
    test_s6a_sustained_main_fault_is_detected();
    test_s6a_exact_200ms_boundary();
    test_s7_30ms_estop_bounce_does_not_trip();
    test_s7_sustained_estop_press_is_detected();
    test_s7_exact_50ms_boundary();
}
