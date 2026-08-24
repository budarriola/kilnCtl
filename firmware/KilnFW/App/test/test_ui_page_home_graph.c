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
}
