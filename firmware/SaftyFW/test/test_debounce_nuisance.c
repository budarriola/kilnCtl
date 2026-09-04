// Closes GUARD_TEST_MATRIX.md section 1's last two open host-coverable
// rows -- S6a ("mainFault glitching for 100ms" must not trip) and S7
// ("30ms of contact bounce" must not trip) -- against the real, extracted
// debounce logic (src/debounce_policy.c, moved verbatim out of
// discrete_task.c's static debounce_update(); see debounce_policy.h for the
// full extraction history). A sibling file to test_guard_nuisance.c/
// test_debounce_policy.c rather than an addition to either: it needs its
// own constants mirroring discrete_task.c's real window/period values,
// which belong together rather than folded into either existing file's
// scope.
//
// The window/sample-count arithmetic below is copied from discrete_task.c's
// own SAFTYFW_ESTOP_DEBOUNCE_MS/SAFTYFW_MAIN_FAULT_DEBOUNCE_MS/
// SAFTYFW_DEBOUNCE_SAMPLES() (task_priorities.h's SAFTYFW_PERIOD_DISCRETE_
// TASK_MS = 10ms), so a change to either the windows or the task period in
// discrete_task.c will silently desync from this file unless it is kept in
// sync by hand -- flagged here rather than #include-ing discrete_task.c
// (which cannot be host-built, see debounce_policy.h) or task_priorities.h
// (a FreeRTOS-adjacent header not part of this test's link set).
#include "test_common.h"
#include "../src/debounce_policy.h"

#define TEST_DISCRETE_TASK_PERIOD_MS      10u  // discrete_task.c: SAFTYFW_PERIOD_DISCRETE_TASK_MS
#define TEST_ESTOP_DEBOUNCE_MS             50u  // discrete_task.c: SAFTYFW_ESTOP_DEBOUNCE_MS (S7)
#define TEST_MAIN_FAULT_DEBOUNCE_MS       200u  // discrete_task.c: SAFTYFW_MAIN_FAULT_DEBOUNCE_MS (S6a)
#define TEST_DEBOUNCE_SAMPLES(window_ms) \
    (((window_ms) + TEST_DISCRETE_TASK_PERIOD_MS - 1u) / TEST_DISCRETE_TASK_PERIOD_MS)

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

    uint32_t n = TEST_DEBOUNCE_SAMPLES(TEST_MAIN_FAULT_DEBOUNCE_MS); /* 20 samples @ 200ms/10ms */
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

    uint32_t n = TEST_DEBOUNCE_SAMPLES(TEST_MAIN_FAULT_DEBOUNCE_MS);
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

    uint32_t n = TEST_DEBOUNCE_SAMPLES(TEST_ESTOP_DEBOUNCE_MS); /* 5 samples @ 50ms/10ms */
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

    uint32_t n = TEST_DEBOUNCE_SAMPLES(TEST_ESTOP_DEBOUNCE_MS);
    /* 60ms sustained -- comfortably past the 50ms window. */
    bool final_published = false;
    bool ever_published = run_glitch_episode(60u, 0u, n, &final_published);
    TEST_CHECK(ever_published, "an E-stop held for 60ms (more than the 50ms window) must be detected");
    TEST_CHECK(final_published, "and it must still read asserted at the end of the sustained hold");
}

void run_test_debounce_nuisance(void)
{
    test_s6a_100ms_main_fault_glitch_does_not_trip();
    test_s6a_sustained_main_fault_is_detected();
    test_s7_30ms_estop_bounce_does_not_trip();
    test_s7_sustained_estop_press_is_detected();
}
