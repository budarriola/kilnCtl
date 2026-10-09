// Host tests for profile_feasibility_plan_curve() (App/drivers/control/profile_feasibility.c)
// -- the duration model backing /api/profile_exec's total_planned_s/
// elapsed_s/remaining_s and GET /api/profile_plan's polyline.
//
// Pure math, no zones_http.h getters involved (unlike profile_feasibility_
// segment()), so no stubs are needed here.
#include <string.h>

#include "test_common.h"

#include "../drivers/control/profile_feasibility.h"
#include "../drivers/http/profiles_http.h"

static profile_segment_t seg(float target_c, float ramp_c_per_hr, uint32_t dwell_min)
{
    profile_segment_t s;
    s.target_c = target_c;
    s.ramp_c_per_hr = ramp_c_per_hr;
    s.dwell_min = dwell_min;
    return s;
}

// A single ramp+dwell segment: 20C -> 1000C at 500C/hr (distance 980C, so
// 980/500 h = 7056s ramp), then a 30-minute dwell (1800s).
// Hand-checkable: 7056 + 1800 = 8856s.
static void test_plain_ramp_and_dwell(void)
{
    TEST_SECTION("plain ramp+dwell -- known total");
    profile_segment_t segs[1] = { seg(1000.0f, 500.0f, 30) };

    profile_plan_point_t points[8];
    size_t count = 0;
    int64_t total = profile_feasibility_plan_curve(segs, 1, 20.0f, points, 8, &count);

    TEST_CHECK(total == 8856, "plain ramp+dwell total should be 8856s (7056s ramp + 30min dwell)");
    TEST_CHECK(count == 3, "plain ramp+dwell should emit 3 points (start, ramp-end/dwell-start, dwell-end)");
    if (count == 3) {
        TEST_CHECK_NEAR(points[0].t, 0.0, 0.01, "point 0 t");
        TEST_CHECK_NEAR(points[0].c, 20.0, 0.01, "point 0 c (start_c)");
        TEST_CHECK_NEAR(points[1].t, 7056.0, 0.01, "point 1 t (ramp end, 980C @ 500C/hr)");
        TEST_CHECK_NEAR(points[1].c, 1000.0, 0.01, "point 1 c (segment target)");
        TEST_CHECK_NEAR(points[2].t, 8856.0, 0.01, "point 2 t (dwell end, +30min)");
        TEST_CHECK_NEAR(points[2].c, 1000.0, 0.01, "point 2 c (holds target through the dwell)");
    }
}

// A zero-ramp ("no rate constraint") segment must NOT silently contribute 0
// seconds as if that were a known answer -- it must make the whole total
// unknown (-1), the honesty rule this function exists to enforce.
static void test_zero_ramp_is_unknown_not_zero(void)
{
    TEST_SECTION("zero-ramp segment -- unknown total, not zero");
    profile_segment_t segs[1] = { seg(500.0f, 0.0f, 10) };

    int64_t total = profile_feasibility_plan_curve(segs, 1, 20.0f, NULL, 0, NULL);
    TEST_CHECK(total == -1, "a ramp_c_per_hr<=0 segment must report -1 (unknown), never 0");

    // Also true for a negative rate (same "no constraint" encoding).
    profile_segment_t segs_neg[1] = { seg(500.0f, -5.0f, 0) };
    int64_t total_neg = profile_feasibility_plan_curve(segs_neg, 1, 20.0f, NULL, 0, NULL);
    TEST_CHECK(total_neg == -1, "a negative ramp_c_per_hr segment must also report -1 (unknown)");
}

// Multi-segment profile: one rate-limited ramp+dwell, then a second
// zero-ramp segment -- the known first segment must not mask the second
// segment's unknown duration; the *whole profile's* total must be unknown,
// but points before the unknown segment must still carry correct times.
static void test_multi_segment_mixed_known_and_unknown(void)
{
    TEST_SECTION("multi-segment -- one unknown segment makes the whole total unknown");
    profile_segment_t segs[2] = {
        seg(520.0f, 300.0f, 0),  // 20 -> 520C at 300C/hr = exactly 6000s, no dwell
        seg(900.0f, 0.0f, 15),   // no rate constraint, then a 15-minute dwell
    };

    profile_plan_point_t points[8];
    size_t count = 0;
    int64_t total = profile_feasibility_plan_curve(segs, 2, 20.0f, points, 8, &count);

    TEST_CHECK(total == -1, "one unknown-duration segment must make the profile total -1");
    // Points: [start@0,20] [seg0 ramp end@6000,520] [seg1 step@6000,900] [seg1 dwell end@6900,900]
    TEST_CHECK(count == 4, "mixed profile should still emit all 4 points despite unknown total");
    if (count == 4) {
        TEST_CHECK_NEAR(points[1].t, 6000.0, 0.01, "segment 0's known ramp end lands at 6000s");
        TEST_CHECK_NEAR(points[1].c, 520.0, 0.01, "segment 0's target");
        TEST_CHECK_NEAR(points[2].t, 6000.0, 0.01, "segment 1's zero-ramp step is zero-width (plotted at its start time)");
        TEST_CHECK_NEAR(points[2].c, 900.0, 0.01, "segment 1's target");
        TEST_CHECK_NEAR(points[3].t, 6900.0, 0.01, "segment 1's dwell end, 15 min later");
    }
}

// A fully rate-limited multi-segment profile (all known) sums correctly --
// guards against a per-segment bug that only shows up once totals compound
// across more than one segment.
static void test_multi_segment_all_known_sums_correctly(void)
{
    TEST_SECTION("multi-segment -- all-known totals sum correctly");
    profile_segment_t segs[3] = {
        seg(220.0f, 200.0f, 0),   // 20 -> 220 @200C/hr = 1h = 3600s
        seg(220.0f, 100.0f, 60),  // no distance to travel (already at 220), + 60min dwell = 3600s
        seg(1000.0f, 780.0f, 0),  // 220 -> 1000 @780C/hr = 1h = 3600s
    };

    int64_t total = profile_feasibility_plan_curve(segs, 3, 20.0f, NULL, 0, NULL);
    TEST_CHECK(total == 10800, "3 segments of 3600s each should sum to 10800s");
}

void run_test_profile_plan_curve(void)
{
    test_plain_ramp_and_dwell();
    test_zero_ramp_is_unknown_not_zero();
    test_multi_segment_mixed_known_and_unknown();
    test_multi_segment_all_known_sums_correctly();
}
