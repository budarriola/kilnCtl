#include "ui_page_home_graph.h"

#include <math.h>
#include <stdio.h>

void ui_page_home_format_mmss(uint32_t seconds, char *out, size_t out_cap)
{
    uint32_t m = seconds / 60u;
    uint32_t s = seconds % 60u;
    snprintf(out, out_cap, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}

void ui_page_home_x_ticks(float horizon_s, float out_ticks[4])
{
    if (horizon_s < 0.0f) {
        horizon_s = 0.0f;
    }
    out_ticks[0] = 0.0f;
    out_ticks[1] = horizon_s / 3.0f;
    out_ticks[2] = horizon_s * 2.0f / 3.0f;
    out_ticks[3] = horizon_s;
}

float ui_page_home_plan_peak_c(const profile_plan_point_t *pts, size_t n)
{
    if (n == 0) {
        return NAN;
    }
    float peak = pts[0].c;
    for (size_t i = 1; i < n; i++) {
        if (pts[i].c > peak) {
            peak = pts[i].c;
        }
    }
    return peak;
}

size_t ui_page_home_now_bucket_index(float horizon_s, float elapsed_s, size_t point_count)
{
    if (point_count < 2 || horizon_s <= 0.0f) {
        return 0;
    }
    if (elapsed_s < 0.0f) {
        elapsed_s = 0.0f;
    }
    if (elapsed_s > horizon_s) {
        elapsed_s = horizon_s;
    }
    /* Inverse of refresh_cb()'s t_i = i * horizon_s / (point_count-1). */
    float f = elapsed_s * (float)(point_count - 1) / horizon_s;
    long idx = lroundf(f);
    if (idx < 0) {
        idx = 0;
    }
    if ((size_t)idx > point_count - 1) {
        idx = (long)(point_count - 1);
    }
    return (size_t)idx;
}

bool ui_page_home_build_x_label(float horizon_s, bool has_span, char *out, size_t out_cap)
{
    if (!has_span) {
        return false;
    }
    float ticks[4];
    ui_page_home_x_ticks(horizon_s, ticks);
    char t1_buf[16], t2_buf[16], t3_buf[16];
    ui_page_home_format_mmss((uint32_t)lroundf(ticks[1]), t1_buf, sizeof(t1_buf));
    ui_page_home_format_mmss((uint32_t)lroundf(ticks[2]), t2_buf, sizeof(t2_buf));
    ui_page_home_format_mmss((uint32_t)lroundf(ticks[3]), t3_buf, sizeof(t3_buf));
    snprintf(out, out_cap, "0:00|%s|%s|%s", t1_buf, t2_buf, t3_buf);
    return true;
}

void ui_page_home_y_axis_range(float lo, float hi, float floor_disp, int32_t *out_axis_lo, int32_t *out_axis_hi)
{
    float range = hi - lo;
    if (range < 1.0f) {
        range = 1.0f; /* degenerate/all-same-value guard -- see header comment */
    }
    float pad_c = range * 0.1f;
    int32_t axis_lo = (int32_t)lroundf(lo - pad_c);
    int32_t axis_hi = (int32_t)lroundf(hi + pad_c);

    int32_t floor_i = (int32_t)lroundf(floor_disp);
    if (axis_lo < floor_i && lo >= floor_i) {
        axis_lo = floor_i;
    }
    /* Second-stage guard: the float-side `range < 1.0f` floor above keeps
     * pad_c from being zero, but does NOT guarantee axis_lo/axis_hi round to
     * DIFFERENT integers -- e.g. lo=hi=45, range floored to 1.0, pad_c=0.1,
     * so lo-pad_c=44.9 and hi+pad_c=45.1 both round (lroundf) to the SAME
     * integer 45. Caught by this repo's negative-test discipline: a first
     * pass here had only the float guard and still failed the degenerate
     * lo==hi==45 test in test_ui_page_home_graph.c. Same integer-level
     * "axis_hi <= axis_lo -> axis_lo + 1" bump this file's caller already
     * uses for the 0..peak axis case (ui_page_home.c's state_active
     * branch). */
    if (axis_hi <= axis_lo) {
        axis_hi = axis_lo + 1;
    }
    *out_axis_lo = axis_lo;
    *out_axis_hi = axis_hi;
}
