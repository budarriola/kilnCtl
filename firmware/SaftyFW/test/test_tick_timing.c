// test_tick_timing.c -- host tests for src/tasks/tick_timing.c/.h, the
// 2026-08-27 audit's item 1 (measured dt_s) and item 2 (snapshot freshness)
// pure helpers. Mirrors test_clock_health.c's own structure/style (that file
// covers the sibling stalled-clock detector these two functions are wired
// alongside in safety_core.c).
#include <math.h>

#include "test_common.h"
#include "../src/tasks/tick_timing.h"

// Real values from safety_core.c, duplicated here (not #included -- that
// file pulls FreeRTOS/pico-sdk) so these tests exercise the SAME numbers
// production actually uses, not arbitrary stand-ins.
#define NOMINAL_DT_S 0.1f  /* SAFTYFW_PERIOD_SAFETY_CORE_MS = 100ms */
#define MIN_DT_S     0.05f /* NOMINAL_DT_S * 0.5 */
#define MAX_DT_S     1.0f  /* NOMINAL_DT_S * 10.0 */

// --- tick_dt_compute_s -------------------------------------------------

// The first tick after boot has no previous timestamp -- must fall back to
// nominal_dt_s exactly, not compute a garbage diff against an unset 0.
static void test_first_tick_falls_back_to_nominal(void)
{
    float dt = tick_dt_compute_s(100000u, 0u, false, false, NOMINAL_DT_S, MIN_DT_S, MAX_DT_S);
    TEST_CHECK(dt == NOMINAL_DT_S,
               "the first tick (have_prev_now_ms == false) must return nominal_dt_s exactly");
}

// A clock the caller has already declared stalled must never be trusted for
// a measurement, regardless of what the raw timestamps say.
static void test_stalled_clock_falls_back_to_nominal(void)
{
    // now_ms == prev_now_ms would measure 0s anyway, so also prove the
    // fallback wins even when the raw numbers suggest a large gap -- a
    // stalled clock's now_ms could be frozen far ahead of a stale prev if
    // the caller had not already reset have_prev_now_ms (belt and
    // suspenders: this function must not trust clock_stalled == true even
    // if the caller somehow still passed have_prev_now_ms == true).
    float dt = tick_dt_compute_s(999999u, 100u, true, true, NOMINAL_DT_S, MIN_DT_S, MAX_DT_S);
    TEST_CHECK(dt == NOMINAL_DT_S,
               "clock_stalled == true must return nominal_dt_s exactly, ignoring the raw diff");
}

// GREEN case, item 1's own motivating example: a tick that genuinely took
// 150ms (not the compile-time-constant 100ms) must report 150ms, not
// silently collapse back to the constant -- this is the whole point of the
// fix. Proven here, then broken deliberately at the production call site as
// this task's mandatory negative test (see the coordinator's own build/test
// transcript, not reproducible in a pure-function host test -- an elongated
// dt_s is exactly what this function is FOR, so "no clamp fires" is the
// green path, not a gap in coverage).
static void test_genuine_elongation_is_measured_not_hidden(void)
{
    float dt = tick_dt_compute_s(150u, 0u, true, false, NOMINAL_DT_S, MIN_DT_S, MAX_DT_S);
    TEST_CHECK(fabsf(dt - 0.15f) < 0.0001f,
               "a genuine 150ms gap must be reported as 0.15s, not clamped back to nominal -- "
               "this is the exact scheduling-pressure scenario the fix exists for");
}

// A dt below the lower clamp must be raised to MIN_DT_S, not passed through
// or clamped to nominal.
static void test_below_floor_clamps_to_min(void)
{
    // 10ms real gap, well under MIN_DT_S (50ms).
    float dt = tick_dt_compute_s(10u, 0u, true, false, NOMINAL_DT_S, MIN_DT_S, MAX_DT_S);
    TEST_CHECK(dt == MIN_DT_S, "a measured dt below min_dt_s must clamp UP to min_dt_s exactly");
}

// A dt above the upper clamp must be lowered to MAX_DT_S -- the nuisance-
// trip-storm protection: a single catastrophic stall must not inject its
// full raw duration.
static void test_above_ceiling_clamps_to_max(void)
{
    // 30 real seconds in one tick (a pathological stall) -- must clamp down
    // to MAX_DT_S (1.0s), not pass through as 30.0s.
    float dt = tick_dt_compute_s(30000u, 0u, true, false, NOMINAL_DT_S, MIN_DT_S, MAX_DT_S);
    TEST_CHECK(dt == MAX_DT_S, "a measured dt above max_dt_s must clamp DOWN to max_dt_s exactly");
}

// Wraparound: now_ms < prev_now_ms numerically, from the same clock wrapping
// past UINT32_MAX -- unsigned subtraction must still produce the correct
// small positive elapsed time, same idiom safety_core.c's context_valid/
// reboot_grace_active already rely on.
static void test_wraparound_measures_correctly(void)
{
    uint32_t prev = 0xFFFFFFF0u; // 16ms before the wrap
    uint32_t now  = 20u;         // 20ms after the wrap -> 36ms real elapsed
    float dt = tick_dt_compute_s(now, prev, true, false, NOMINAL_DT_S, MIN_DT_S, MAX_DT_S);
    // 36ms is below MIN_DT_S (50ms), so this also exercises the floor clamp
    // on a wraparound-computed value -- both facts checked together.
    TEST_CHECK(dt == MIN_DT_S,
               "a wraparound elapsed time (36ms) must be measured correctly (not as a huge "
               "unsigned underflow) and still clamp exactly like a non-wrapped 36ms gap would");
}

static void run_tick_dt_compute_s_tests(void)
{
    test_first_tick_falls_back_to_nominal();
    test_stalled_clock_falls_back_to_nominal();
    test_genuine_elongation_is_measured_not_hidden();
    test_below_floor_clamps_to_min();
    test_above_ceiling_clamps_to_max();
    test_wraparound_measures_correctly();
}

// --- snapshot_is_fresh ---------------------------------------------------

// A snapshot published on THIS exact tick (age 0) must read as fresh.
static void test_zero_age_is_fresh(void)
{
    TEST_CHECK(snapshot_is_fresh(1000u, 1000u, 2000u),
               "a snapshot with age 0 must be fresh");
}

// A normal, slightly-late tick (well within max_age_ms) must still be
// accepted -- the mandatory "ordinary jitter is not mistaken for staleness"
// direction.
static void test_slightly_late_snapshot_is_still_fresh(void)
{
    // Published 300ms ago against a 2000ms budget -- an entirely ordinary
    // gap (a couple of missed thermo/current publish cycles), must not read
    // as stale.
    TEST_CHECK(snapshot_is_fresh(1300u, 1000u, 2000u),
               "a snapshot 300ms old against a 2000ms budget is ordinary jitter, must read fresh");
}

// A snapshot older than max_age_ms must be rejected -- the mandatory "a
// genuinely stale snapshot is caught" direction.
static void test_snapshot_older_than_max_age_is_stale(void)
{
    TEST_CHECK(!snapshot_is_fresh(3001u, 1000u, 2000u),
               "a snapshot 2001ms old against a 2000ms budget must read as stale");
}

// Exact boundary: age == max_age_ms must read as stale (strict '<', matching
// context_valid's own "age_ms < CONTEXT_MAX_AGE_MS" convention in
// safety_core.c -- age exactly equal to the budget has already used the
// whole allowance, not one tick of margin left).
static void test_age_exactly_at_boundary_is_stale(void)
{
    TEST_CHECK(!snapshot_is_fresh(3000u, 1000u, 2000u),
               "age exactly equal to max_age_ms must read as stale, matching context_valid's "
               "own strict '<' boundary convention");
}

// A never-published snapshot (timestamp_ms == 0, thermo_task.c/current_
// sense.c's own zero-init default) must read as stale against any real
// now_ms, with no special-casing needed.
static void test_never_published_timestamp_is_stale(void)
{
    TEST_CHECK(!snapshot_is_fresh(50000u, 0u, 2000u),
               "timestamp_ms == 0 (never published) must read as stale against a real now_ms, "
               "with no special-case needed beyond the plain age comparison");
}

// Wraparound: now_ms has wrapped past timestamp_ms numerically.
static void test_snapshot_freshness_wraparound(void)
{
    uint32_t timestamp = 0xFFFFFFF0u; // 16ms before the wrap
    uint32_t now        = 20u;        // 20ms after -> 36ms real age
    TEST_CHECK(snapshot_is_fresh(now, timestamp, 2000u),
               "a wraparound age (36ms) must be measured correctly and read as fresh against a "
               "2000ms budget, not misread as an enormous unsigned underflow age");
}

static void run_snapshot_is_fresh_tests(void)
{
    test_zero_age_is_fresh();
    test_slightly_late_snapshot_is_still_fresh();
    test_snapshot_older_than_max_age_is_stale();
    test_age_exactly_at_boundary_is_stale();
    test_never_published_timestamp_is_stale();
    test_snapshot_freshness_wraparound();
}

static void test_reboot_grace_expires_once_no_wrap_rearm(void)
{
    bool ev = false;
    uint32_t ea = 0u;
    TEST_CHECK(!reboot_grace_evaluate(false, 0u, 100u, 5000u, &ev, &ea),
               "never announced -> no grace");
    TEST_CHECK(reboot_grace_evaluate(true, 1000u, 2000u, 5000u, &ev, &ea),
               "inside window -> grace active");
    TEST_CHECK(!reboot_grace_evaluate(true, 1000u, 6000u, 5000u, &ev, &ea),
               "age == window -> expired");
    // Tick wraps ~49.7 days later: raw (now - announced) is small again.
    TEST_CHECK(!reboot_grace_evaluate(true, 1000u, 1000u + 100u, 5000u, &ev, &ea),
               "after expiry a wrapped small age must NOT re-arm the grace window (F7)");
    TEST_CHECK(reboot_grace_evaluate(true, 9000u, 9100u, 5000u, &ev, &ea),
               "a genuinely new announcement is honoured after an expiry");
}

void run_test_tick_timing(void)
{
    TEST_SECTION("tick_dt_compute_s -- 2026-08-27 audit item 1, measured (not compile-time-"
                  "constant) dt_s");
    run_tick_dt_compute_s_tests();

    TEST_SECTION("snapshot_is_fresh -- 2026-08-27 audit item 2, consumer-side staleness for "
                  "thermo/current snapshots");
    run_snapshot_is_fresh_tests();

    TEST_SECTION("reboot_grace_evaluate -- guard review F7, wrap-safe expire-once");
    test_reboot_grace_expires_once_no_wrap_rearm();
}
