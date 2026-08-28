// Host tests for App/drivers/dram_margin.h -- the boot-time internal-DRAM
// low-water alarm wired into main.c's heap_stage() (2026-08-24 investigation,
// commit 750dc33's bench log: largest=7680/dram_free=12483 at "uart_bridges_1",
// close enough to the documented failure zone below to be worth an explicit,
// provable check rather than a human eyeballing a boot log every time).
//
// dram_margin_check() is a pure, header-only inline function -- no ESP-IDF,
// no I/O -- so it is included directly here, same as any other pure-decision
// module this project host-tests (boot_button.c's boot_button_step(),
// boot_guard.c's next_boot_count(), etc.).
#include "test_common.h"

#include "../drivers/dram_margin.h"

static void test_both_healthy_does_not_trip(void)
{
    dram_margin_result_t r = dram_margin_check(31744, 47139);
    TEST_CHECK(!r.tripped, "healthy largest+free (executor+autotune stage figures): must not trip");
    TEST_CHECK(!r.largest_low, "healthy largest: largest_low false");
    TEST_CHECK(!r.free_low, "healthy free: free_low false");
}

static void test_largest_alone_trips_it(void)
{
    // Exactly the documented failure's largest figure minus one, with a
    // free total nowhere near its own alarm -- proves the largest check
    // trips independently of the free-total check, not only when both sag
    // together.
    dram_margin_result_t r = dram_margin_check(KILN_DRAM_LARGEST_ALARM_BYTES - 1, 100000);
    TEST_CHECK(r.tripped, "largest one byte under its alarm: must trip");
    TEST_CHECK(r.largest_low, "largest_low set");
    TEST_CHECK(!r.free_low, "free comfortably clear: free_low NOT set");
}

static void test_free_alone_trips_it(void)
{
    // Mirror of the above: a huge largest block (can't happen with a small
    // free total in real life, but the function must not assume the two are
    // correlated -- it is handed whatever heap_caps_get_*() actually
    // returned) with free under its own alarm.
    dram_margin_result_t r = dram_margin_check(50000, KILN_DRAM_FREE_ALARM_BYTES - 1);
    TEST_CHECK(r.tripped, "free one byte under its alarm: must trip");
    TEST_CHECK(!r.largest_low, "largest comfortably clear: largest_low NOT set");
    TEST_CHECK(r.free_low, "free_low set");
}

static void test_exact_threshold_is_not_yet_alarmed(void)
{
    // Boundary check: the alarm fires BELOW the documented failure figures,
    // not AT them -- values equal to the threshold read as still-healthy.
    // This pins the "<" in dram_margin_check() against an accidental "<="
    // flip either direction.
    dram_margin_result_t r =
        dram_margin_check(KILN_DRAM_LARGEST_ALARM_BYTES, KILN_DRAM_FREE_ALARM_BYTES);
    TEST_CHECK(!r.tripped, "exactly at both alarm thresholds: not yet tripped");
}

static void test_current_bench_figures_do_trip(void)
{
    // THIS is the negative test that matters most: the actual bench numbers
    // from this investigation (2026-08-24, commit 750dc33, "uart_bridges_1"
    // stage: largest=7680, dram_free=12483) must trip the alarm. Temporarily
    // widening either constant in dram_margin.h past 7680/12483 makes this
    // assertion fail -- proving the check is not vacuous, i.e. it can and
    // does fail against a value it is meant to catch.
    // NOTE: 12483 was the uart_bridges_1 reading, once believed to be the
    // trough. It is not -- app_main_done reaches 11415. Kept as a test case
    // because it is a real measured point that must still trip on `largest`,
    // but see KILN_DRAM_FREE_KNOWN_BYTES for the actual minimum.
    dram_margin_result_t r = dram_margin_check(7680, 12483);
    TEST_CHECK(r.tripped, "2026-08-24 bench figures (largest=7680, free=12483): must trip the alarm");
    TEST_CHECK(r.largest_low, "largest=7680 is below the 8704 alarm: largest_low set");
    TEST_CHECK(!r.free_low, "free=12483 is above the 11903 alarm: free_low NOT set at THIS stage");
}

static void test_bench_figures_are_not_a_regression(void)
{
    // The standing-vs-regression split (main.c's heap_stage()). The bench
    // figures trip the failure-zone alarm -- covered above -- but must NOT
    // report a regression, because they ARE the measured baseline. If they
    // did, every boot would log DRAM REGRESSION and the line would stop
    // meaning anything, which is the exact failure the split exists to avoid.
    dram_margin_result_t r = dram_margin_check(KILN_DRAM_LARGEST_KNOWN_BYTES,
                                               KILN_DRAM_FREE_KNOWN_BYTES);
    // 2026-08-27: the trough climbed clear of the failure zone (13824/22123
    // against alarms of 8704/11903), so this no longer trips. That is the
    // point of the change, not a weakened test -- the regression half below
    // is what this case exists to pin, and it still holds.
    TEST_CHECK(!r.tripped, "the known trough is now ABOVE the documented failure zone");
    TEST_CHECK(!r.regressed, "the known trough is NOT a regression against itself");
    TEST_CHECK(!r.largest_regressed, "largest exactly at the known trough: not a regression");
    TEST_CHECK(!r.free_regressed, "free exactly at the known trough: not a regression");
}

static void test_within_slack_is_not_a_regression(void)
{
    // Boot-to-boot jitter must NOT report a regression, or the line flaps and
    // becomes as worthless as one that fires every boot. Exactly at the slack
    // boundary, both axes.
    dram_margin_result_t r =
        dram_margin_check(KILN_DRAM_LARGEST_KNOWN_BYTES - KILN_DRAM_REGRESSION_SLACK_BYTES,
                          KILN_DRAM_FREE_KNOWN_BYTES - KILN_DRAM_REGRESSION_SLACK_BYTES);
    TEST_CHECK(!r.regressed, "exactly one slack below the trough: still not a regression");
    TEST_CHECK(!r.largest_regressed, "largest at the slack boundary: not a regression");
    TEST_CHECK(!r.free_regressed, "free at the slack boundary: not a regression");
}

static void test_beyond_slack_is_a_regression(void)
{
    // The check must catch a build that uses more internal DRAM than any
    // measured before it -- that is the whole point of the second threshold.
    // One byte past the slack boundary on each axis independently, so neither
    // condition can be carried by the other.
    dram_margin_result_t r = dram_margin_check(
        KILN_DRAM_LARGEST_KNOWN_BYTES - KILN_DRAM_REGRESSION_SLACK_BYTES - 1,
        KILN_DRAM_FREE_KNOWN_BYTES);
    TEST_CHECK(r.regressed, "largest one byte past the slack boundary: regression");
    TEST_CHECK(r.largest_regressed, "largest_regressed set");
    TEST_CHECK(!r.free_regressed, "free unchanged: free_regressed NOT set");

    dram_margin_result_t r2 = dram_margin_check(
        KILN_DRAM_LARGEST_KNOWN_BYTES,
        KILN_DRAM_FREE_KNOWN_BYTES - KILN_DRAM_REGRESSION_SLACK_BYTES - 1);
    TEST_CHECK(r2.regressed, "free one byte past the slack boundary: regression");
    TEST_CHECK(r2.free_regressed, "free_regressed set");
    TEST_CHECK(!r2.largest_regressed, "largest unchanged: largest_regressed NOT set");
}

static void test_slack_cannot_be_widened_into_uselessness(void)
{
    // The slack-boundary tests above are expressed in terms of
    // KILN_DRAM_REGRESSION_SLACK_BYTES, which means widening that constant
    // moves their expectations with it -- they would keep passing while the
    // regression check quietly stopped detecting anything. (Observed: setting
    // the slack to 9999 left the whole suite green.) So these assertions use
    // ABSOLUTE byte counts instead, and are the ones that actually constrain
    // the constant.
    TEST_CHECK(KILN_DRAM_REGRESSION_SLACK_BYTES <= 1024,
               "regression slack stays <=1KB -- wider than this and real growth hides in it");

    // A build that loses 2KB of internal DRAM against the measured trough is
    // a regression by any reasonable definition, whatever the slack says.
    dram_margin_result_t r = dram_margin_check(KILN_DRAM_LARGEST_KNOWN_BYTES,
                                               KILN_DRAM_FREE_KNOWN_BYTES - 2048);
    TEST_CHECK(r.regressed, "2KB worse than the measured trough: regression, absolutely");
    TEST_CHECK(r.free_regressed, "free_regressed set at 2KB worse");

    // And a genuinely starved board must report both states at once.
    dram_margin_result_t r2 = dram_margin_check(2048, 4096);
    TEST_CHECK(r2.tripped, "a starved board trips the failure-zone alarm");
    TEST_CHECK(r2.regressed, "a starved board also reports a regression");
}

static void test_measured_trough_clears_the_documented_failure_figure(void)
{
    // The original form of this case pinned the finding that corrected the
    // investigation's framing: the real end-of-boot trough (app_main_done)
    // was BELOW the 11903 figure from the one real failure, not above it as
    // the uart_bridges_1 reading suggested. That framing still matters --
    // app_main_done is still the stage to measure, and quoting
    // uart_bridges_1 is still the mistake to avoid (the first draft of the
    // 2026-08-27 baseline update made exactly that error and used 23111
    // instead of 22123).
    //
    // What changed is the answer: moving four task stacks off internal DRAM
    // lifted the trough clear of both alarm figures. So this now pins the
    // opposite fact, and pins the FLOOR as the thing standing between the
    // trough and that failure zone.
    TEST_CHECK(KILN_DRAM_FREE_KNOWN_BYTES > KILN_DRAM_FREE_ALARM_BYTES,
               "the measured end-of-boot trough now clears the documented failure zone");
    TEST_CHECK(KILN_DRAM_FREE_KNOWN_BYTES >= KILN_DRAM_FREE_FLOOR_BYTES,
               "the measured trough is at or above the owner's 20 kB operating floor");
    TEST_CHECK(KILN_DRAM_FREE_FLOOR_BYTES > KILN_DRAM_FREE_ALARM_BYTES,
               "the floor must sit ABOVE the evidence-based failure figure -- it is the early "
               "warning for it, and below it the warning would arrive too late to be one");
    dram_margin_result_t r =
        dram_margin_check(KILN_DRAM_LARGEST_KNOWN_BYTES, KILN_DRAM_FREE_KNOWN_BYTES);
    TEST_CHECK(!r.free_low, "free_low clear at the measured trough");
    TEST_CHECK(!r.largest_low, "largest_low clear at the measured trough");
    TEST_CHECK(!r.below_floor, "the measured trough is not below the floor");
}

static void test_floor_catches_a_dip_the_failure_alarm_would_miss(void)
{
    // The floor's whole reason to exist: a free figure between the failure
    // figure and the floor is invisible to the failure alarm but is exactly
    // the state worth reporting -- the margin has been spent while there is
    // still room to act.
    dram_margin_result_t r = dram_margin_check(KILN_DRAM_LARGEST_KNOWN_BYTES,
                                               KILN_DRAM_FREE_FLOOR_BYTES - 1u);
    TEST_CHECK(r.below_floor, "one byte under the floor reports below_floor");
    TEST_CHECK(!r.free_low, "...while still clear of the documented failure figure");
    dram_margin_result_t ok = dram_margin_check(KILN_DRAM_LARGEST_KNOWN_BYTES,
                                                KILN_DRAM_FREE_FLOOR_BYTES);
    TEST_CHECK(!ok.below_floor, "exactly at the floor is not below it");
}

static void test_healthy_figures_are_neither(void)
{
    // A board with real headroom reports neither state -- so the alarm cannot
    // be "always on" in some other build, only in this one.
    dram_margin_result_t r = dram_margin_check(40000, 60000);
    TEST_CHECK(!r.tripped, "healthy figures: no failure-zone alarm");
    TEST_CHECK(!r.regressed, "healthy figures: no regression");
}

void run_test_dram_margin(void)
{
    TEST_SECTION("dram_margin_check");
    test_both_healthy_does_not_trip();
    test_largest_alone_trips_it();
    test_free_alone_trips_it();
    test_exact_threshold_is_not_yet_alarmed();
    test_current_bench_figures_do_trip();
    test_bench_figures_are_not_a_regression();
    test_within_slack_is_not_a_regression();
    test_beyond_slack_is_a_regression();
    test_slack_cannot_be_widened_into_uselessness();
    test_measured_trough_clears_the_documented_failure_figure();
    test_floor_catches_a_dip_the_failure_alarm_would_miss();
    test_healthy_figures_are_neither();
}
