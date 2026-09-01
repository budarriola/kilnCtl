// Host tests for ui_page_home_graph.c -- the pure geometry/formatting seam
// factored out of ui_page_home.c's refresh_cb() so the LCD profile chart's
// match to main_page.html's web chart (M:SS x-axis ticks, 0..peak y-axis,
// current-position bucket index) can be checked without LVGL/esp_log stubs.
#include "test_common.h"
#include "../drivers/ui_page_home_graph.h"

#include <math.h>
#include <string.h>

void run_test_ui_page_home_graph(void)
{
    TEST_SECTION("ui_page_home_graph: format_mmss");
    {
        char buf[16];
        ui_page_home_format_mmss(0, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "0:00") == 0, "0s -> 0:00");

        ui_page_home_format_mmss(59, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "0:59") == 0, "59s -> 0:59");

        ui_page_home_format_mmss(60, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1:00") == 0, "60s -> 1:00");

        // Screenshot's own tick values (owner-supplied reference): total
        // elapsed MINUTES, never rolled over to hours -- 125:59 is 125
        // minutes 59 seconds, not 2:05:59.
        ui_page_home_format_mmss(125u * 60u + 59u, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "125:59") == 0, "125m59s -> 125:59 (no hour rollover)");

        ui_page_home_format_mmss(377u * 60u + 57u, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "377:57") == 0, "377m57s -> 377:57");
    }

    TEST_SECTION("ui_page_home_graph: x_ticks");
    {
        float t[4];
        ui_page_home_x_ticks(0.0f, t);
        TEST_CHECK_NEAR(t[0], 0.0f, 0.001f, "horizon 0: tick0");
        TEST_CHECK_NEAR(t[3], 0.0f, 0.001f, "horizon 0: tick3");

        ui_page_home_x_ticks(300.0f, t);
        TEST_CHECK_NEAR(t[0], 0.0f, 0.001f, "horizon 300: tick0 == 0");
        TEST_CHECK_NEAR(t[1], 100.0f, 0.001f, "horizon 300: tick1 == h/3");
        TEST_CHECK_NEAR(t[2], 200.0f, 0.001f, "horizon 300: tick2 == 2h/3");
        TEST_CHECK_NEAR(t[3], 300.0f, 0.001f, "horizon 300: tick3 == h");

        // Negative horizon (should never happen post-guard, but the function
        // must not hand back negative tick times) clamps to all-zero.
        ui_page_home_x_ticks(-5.0f, t);
        TEST_CHECK_NEAR(t[3], 0.0f, 0.001f, "negative horizon clamps to 0");
    }

    TEST_SECTION("ui_page_home_graph: plan_peak_c");
    {
        TEST_CHECK(isnan(ui_page_home_plan_peak_c(NULL, 0)), "empty list -> NAN");

        profile_plan_point_t pts[4] = {
            {0.0f, 20.0f},
            {600.0f, 1170.0f}, // the screenshot's own peak value
            {1200.0f, 1170.0f},
            {1800.0f, 400.0f}, // controlled cool-down below peak
        };
        float peak = ui_page_home_plan_peak_c(pts, 4);
        TEST_CHECK_NEAR(peak, 1170.0f, 0.001f, "peak is the max c, not the last point");

        profile_plan_point_t single[1] = {{0.0f, 55.5f}};
        TEST_CHECK_NEAR(ui_page_home_plan_peak_c(single, 1), 55.5f, 0.001f, "single point is its own peak");
    }

    TEST_SECTION("ui_page_home_graph: now_bucket_index");
    {
        // 10 buckets over a 900s horizon (matches this page's own
        // UI_PAGE_HOME_CHART_POINTS-style bucket math, t_i = i*h/(n-1)).
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 0.0f, 10) == 0, "elapsed 0 -> bucket 0");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 900.0f, 10) == 9, "elapsed == horizon -> last bucket");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 450.0f, 10) == 4 ||
                       ui_page_home_now_bucket_index(900.0f, 450.0f, 10) == 5,
                   "elapsed == half horizon -> a middle bucket");
        // Overrun (elapsed past the plotted horizon, e.g. a stale read)
        // clamps to the last bucket rather than extrapolating past it.
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 5000.0f, 10) == 9, "overrun elapsed clamps to last bucket");
        // Degenerate inputs return the single safe index instead of
        // dividing by zero.
        TEST_CHECK(ui_page_home_now_bucket_index(0.0f, 10.0f, 10) == 0, "zero horizon -> 0");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 10.0f, 1) == 0, "point_count<2 -> 0");
        TEST_CHECK(ui_page_home_now_bucket_index(900.0f, 10.0f, 0) == 0, "point_count==0 -> 0");
    }

    TEST_SECTION("ui_page_home_graph: build_x_label");
    {
        char buf[64];

        // has_span == false (idle, no history / a plan that came back empty):
        // no label written, buffer left untouched, function reports "no
        // label" -- this is the case that must stay hidden on the LCD.
        memset(buf, 0x7A, sizeof(buf));
        bool wrote = ui_page_home_build_x_label(300.0f, false, buf, sizeof(buf));
        TEST_CHECK(!wrote, "has_span=false -> no label produced");
        TEST_CHECK(buf[0] == (char)0x7A, "has_span=false -> out buffer untouched");

        // has_span == true, matches the running-profile case (0..horizon_s,
        // four M:SS ticks) -- same numbers ui_page_home_x_ticks() itself
        // returns for horizon 300.
        wrote = ui_page_home_build_x_label(300.0f, true, buf, sizeof(buf));
        TEST_CHECK(wrote, "has_span=true -> label produced");
        TEST_CHECK(strcmp(buf, "0:00|1:40|3:20|5:00") == 0, "horizon 300 -> 0:00|1:40|3:20|5:00");

        // has_span == true also covers idle-with-history: same shape, driven
        // by whatever the caller's own (different) horizon_s span is --
        // proves the function does not special-case a "running" value.
        wrote = ui_page_home_build_x_label(90.0f, true, buf, sizeof(buf));
        TEST_CHECK(wrote, "history span -> label produced");
        TEST_CHECK(strcmp(buf, "0:00|0:30|1:00|1:30") == 0, "horizon 90 -> 0:00|0:30|1:00|1:30");

        // A too-small out buffer must never overflow -- snprintf-backed
        // truncation, same discipline as ui_page_home_format_mmss().
        char tiny[6];
        wrote = ui_page_home_build_x_label(300.0f, true, tiny, sizeof(tiny));
        TEST_CHECK(wrote, "truncated buffer still reports a label was produced");
        TEST_CHECK(strlen(tiny) == sizeof(tiny) - 1, "truncated buffer is NUL-terminated within its capacity");
    }

    TEST_SECTION("ui_page_home_graph: x_ticks/format_mmss over real-run horizons");
    {
        // Tonight's 3-zone firing: ~27 minutes total. Ticks must land at
        // 0, 9:00, 18:00, 27:00 and each format cleanly as mm:ss (no hour
        // rollover -- see ui_page_home_format_mmss()'s own header comment).
        float t[4];
        float horizon_27min = 27.0f * 60.0f;
        ui_page_home_x_ticks(horizon_27min, t);
        TEST_CHECK_NEAR(t[1], 9.0f * 60.0f, 0.01f, "27min horizon: tick1 == 9:00");
        TEST_CHECK_NEAR(t[2], 18.0f * 60.0f, 0.01f, "27min horizon: tick2 == 18:00");
        TEST_CHECK_NEAR(t[3], 27.0f * 60.0f, 0.01f, "27min horizon: tick3 == 27:00");
        char buf[16];
        ui_page_home_format_mmss((uint32_t)t[3], buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "27:00") == 0, "27min horizon: full-span tick formats as 27:00");

        // Autotune's worst-case 4-hour budget: ticks at 0, 80:00, 160:00,
        // 240:00 -- large enough that a naive hh:mm choice would need to
        // differ from the 27-minute case's mm:ss, which is exactly why this
        // page's ticks stay unit-of-minutes (M:SS) at every horizon rather
        // than switching formats and forcing the label-width/tick-count
        // logic to handle two shapes.
        float horizon_4h = 4.0f * 3600.0f;
        ui_page_home_x_ticks(horizon_4h, t);
        TEST_CHECK_NEAR(t[3], 240.0f * 60.0f, 0.01f, "4h horizon: full span == 240 minutes");
        ui_page_home_format_mmss((uint32_t)t[3], buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "240:00") == 0, "4h horizon: full-span tick formats as 240:00");
    }

    TEST_SECTION("ui_page_home_graph: y_axis_range");
    {
        int32_t lo, hi;

        // Tonight's firing spans ~36-63 C -- a real, few-tens-of-degrees
        // range. 10% pad on each side, freezing floor (0 C) does not apply
        // (36 C is well above it).
        ui_page_home_y_axis_range(36.0f, 63.0f, 0.0f, &lo, &hi);
        TEST_CHECK(lo < 36 && hi > 63, "36-63C range: padded on both sides");
        TEST_CHECK_NEAR((float)lo, 36.0f - (63.0f - 36.0f) * 0.1f, 1.0f, "36-63C range: ~10% pad on the low side");
        TEST_CHECK_NEAR((float)hi, 63.0f + (63.0f - 36.0f) * 0.1f, 1.0f, "36-63C range: ~10% pad on the high side");

        // A span of a few degrees (e.g. an idle bench sitting at 31-33C) --
        // still must pad sensibly and never collapse lo==hi.
        ui_page_home_y_axis_range(31.0f, 33.0f, 0.0f, &lo, &hi);
        TEST_CHECK(hi > lo, "small few-degree range: axis_hi > axis_lo");
        TEST_CHECK(lo <= 31 && hi >= 33, "small few-degree range: still spans the real data");

        // Degenerate all-same-value span (lo == hi) -- e.g. exactly one
        // sample recorded so far, or a stuck sensor. THE load-bearing
        // assertion: axis_hi must come back strictly greater than axis_lo,
        // or lv_chart_set_axis_range()'s zero-height axis divides by zero
        // one call downstream (see ui_page_home_y_axis_range()'s own header
        // comment in ui_page_home_graph.h for exactly where). Verified this
        // is not vacuous by removing the `if (range < 1.0f) range = 1.0f;`
        // guard in ui_page_home_graph.c and re-running: with the guard
        // removed, this exact call returns axis_lo == axis_hi == 45 and this
        // assertion fails -- restored afterward.
        ui_page_home_y_axis_range(45.0f, 45.0f, 0.0f, &lo, &hi);
        TEST_CHECK(hi > lo, "degenerate all-same-value span (lo==hi==45): axis_hi > axis_lo, guard held");

        // Freezing floor: a real sub-zero low must NOT be clamped up (the
        // excursion has to stay visible), but padding that merely DIPS below
        // freezing on an otherwise-above-freezing range must be raised to
        // the floor.
        ui_page_home_y_axis_range(-5.0f, 10.0f, 0.0f, &lo, &hi);
        TEST_CHECK(lo < 0, "genuine sub-zero low (-5C): axis_lo stays unclamped below freezing");

        ui_page_home_y_axis_range(1.0f, 5.0f, 0.0f, &lo, &hi);
        TEST_CHECK(lo >= 0, "above-freezing data whose padding alone dips below 0: axis_lo floored at freezing");
    }
}
