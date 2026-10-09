#include "ui_page_home_graph.h"

#include <math.h>
#include <stdint.h>
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
    /* 2026-09-28 owner request: never show less than
     * UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP degrees of vertical span. Widen the
     * raw data range symmetrically around its own midpoint FIRST, before the
     * usual 10% pad/degenerate-guard/freezing-floor logic below runs on top
     * of it -- see header comment. data_lo keeps the REAL data minimum for
     * the freezing-floor decision below: testing the widened lo instead
     * would stop flooring any above-freezing trace within 2.5 degrees of
     * the floor (e.g. flat 1 C data widened to -1.5..3.5). */
    const float data_lo = lo;
    if (hi - lo < UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP) {
        float mid = (lo + hi) * 0.5f;
        lo = mid - UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP * 0.5f;
        hi = mid + UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP * 0.5f;
    }

    float range = hi - lo;
    if (range < 1.0f) {
        range = 1.0f; /* degenerate/all-same-value guard -- see header comment */
    }
    float pad_c = range * 0.1f;
    int32_t axis_lo = (int32_t)lroundf(lo - pad_c);
    int32_t axis_hi = (int32_t)lroundf(hi + pad_c);

    int32_t floor_i = (int32_t)lroundf(floor_disp);
    if (axis_lo < floor_i && data_lo >= floor_i) {
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
    /* Final integer-level floor: the freezing-floor clamp just above only
     * ever RAISES axis_lo, which can narrow the span back under
     * UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP even though the pre-clamp float math
     * already widened it to that span (e.g. a near-freezing flat reading
     * whose widened low edge dips just below the floor and gets clamped back
     * up). Re-widen upward -- never re-lower axis_lo, which would undo the
     * floor clamp this guard runs after -- so the 5-degree minimum holds on
     * the actual returned integers in every case, not just the common one. */
    if (axis_hi - axis_lo < (int32_t)UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP) {
        axis_hi = axis_lo + (int32_t)UI_PAGE_HOME_GRAPH_MIN_SPAN_DISP;
    }
    *out_axis_lo = axis_lo;
    *out_axis_hi = axis_hi;
}

void ui_page_home_legend_visibility(bool has_span, bool has_actual_multi, bool has_planned,
                                     bool *out_show_actual, bool *out_show_plan)
{
    *out_show_actual = has_span && has_actual_multi;
    *out_show_plan = has_span && has_planned;
}

bool ui_page_home_axis_ratchet_should_reset(bool active, bool prev_active, uint32_t elapsed_s,
                                             uint32_t prev_elapsed_s)
{
    if (!active) {
        return false;
    }
    if (!prev_active) {
        return true; /* ordinary IDLE -> active start */
    }
    /* Both ticks active: total_elapsed_s is set to 0 exactly once, at
     * profile_executor_run(), and never decreases again until the next run
     * -- see this function's header comment in ui_page_home_graph.h. A drop
     * between two active ticks means a new run started in between, whether
     * or not an IDLE tick was ever observed. */
    return elapsed_s < prev_elapsed_s;
}

static int32_t quantize_floor_i32(int32_t v, int32_t step)
{
    int32_t r = v % step;
    if (r < 0) {
        r += step;
    }
    return v - r;
}

static int32_t quantize_ceil_i32(int32_t v, int32_t step)
{
    int32_t f = quantize_floor_i32(v, step);
    return (f == v) ? v : f + step;
}

void ui_page_home_active_y_axis_range(float lo, float hi, float floor_disp, bool have_held,
                                       int32_t held_lo, int32_t held_hi, int32_t *out_axis_lo,
                                       int32_t *out_axis_hi)
{
    int32_t raw_lo, raw_hi;
    ui_page_home_y_axis_range(lo, hi, floor_disp, &raw_lo, &raw_hi);

    /* Quantize outward to a round UI_PAGE_HOME_AXIS_QUANT_STEP_DISP-unit
     * step -- see this function's header comment in ui_page_home_graph.h for
     * why: a raw padded range recomputed every ~1s tick from a live trace
     * drifts by fractions of a degree as new samples land, which after
     * lroundf() flips the displayed integer bound back and forth on
     * consecutive refreshes. Flooring the low bound and ceiling the high
     * bound (never the reverse -- that would shrink the visible span below
     * the real data) means a tick that does not cross a step boundary always
     * quantizes to the exact same pair of integers as the tick before it. */
    /* No `q_hi <= q_lo` guard here (a prior version of this function had
     * one): ui_page_home_y_axis_range() above already guarantees raw_hi >
     * raw_lo (its own callers-facing contract, enforced by its own
     * axis_hi<=axis_lo bump). floor_i32(raw_lo) <= raw_lo < raw_hi <=
     * ceil_i32(raw_hi) therefore holds transitively with the middle
     * inequality strict, so floor_i32(raw_lo) < ceil_i32(raw_hi) always --
     * q_hi > q_lo is unreachable to violate and a defensive bump here would
     * be untested, unreachable dead code (this repo's negative-test rule:
     * every guard must be provable to fire, or it does not belong). See
     * test_ui_page_home_graph.c's "quantize never collapses" case, which
     * sweeps lo/hi pairs spanning every possible quantize-boundary alignment
     * and asserts q_hi > q_lo holds without this guard's help. */
    int32_t q_lo = quantize_floor_i32(raw_lo, UI_PAGE_HOME_AXIS_QUANT_STEP_DISP);
    int32_t q_hi = quantize_ceil_i32(raw_hi, UI_PAGE_HOME_AXIS_QUANT_STEP_DISP);

    /* Only-widen ratchet: once a bound has been shown this run, it never
     * moves back inward. Combined with quantization above this is what keeps
     * the axis from visibly breathing in and out while a firing is under
     * way -- the range can grow (a real overshoot, or the plan curve simply
     * having more of itself revealed) but a momentary dip in the accumulated
     * lo/hi (e.g. the actual trace briefly reads a hair cooler than a prior
     * sample) can never yank the axis back in. have_held is false only on
     * the first tick of a run (the caller resets its held state exactly
     * once, at the state_active false->true transition) -- see
     * ui_page_home.c's own call site comment for that reset. */
    if (have_held) {
        if (q_lo > held_lo) {
            q_lo = held_lo;
        }
        if (q_hi < held_hi) {
            q_hi = held_hi;
        }
    }

    *out_axis_lo = q_lo;
    *out_axis_hi = q_hi;
}

uint32_t ui_page_home_lag_notice_tick(bool ramp_lock_held, uint32_t prev_consecutive_ticks)
{
    if (!ramp_lock_held) {
        return 0u;
    }
    if (prev_consecutive_ticks >= UINT32_MAX) {
        return UINT32_MAX; /* saturate, never wrap */
    }
    return prev_consecutive_ticks + 1u;
}

bool ui_page_home_lag_notice_should_show(uint32_t consecutive_ticks)
{
    return consecutive_ticks >= UI_PAGE_HOME_LAG_NOTICE_DEBOUNCE_TICKS;
}

bool ui_page_home_lag_notice_active(bool have_rich_zone_data, bool any_zone_sustained,
                                     uint32_t debounced_ticks)
{
    if (have_rich_zone_data) {
        return any_zone_sustained;
    }
    return ui_page_home_lag_notice_should_show(debounced_ticks);
}

size_t ui_page_home_lagging_zone_indices(uint8_t mask, uint8_t max_zones, uint8_t *out_indices,
                                          size_t out_cap)
{
    size_t n = 0;
    for (uint8_t zi = 0; zi < max_zones && zi < 8u; zi++) {
        if (!(mask & (1u << zi))) {
            continue;
        }
        if (n >= out_cap) {
            break;
        }
        out_indices[n++] = zi;
    }
    return n;
}

float ui_page_home_active_horizon_s(float plan_horizon_s, float elapsed_s)
{
    float h = 1.0f;
    if (plan_horizon_s > h) {
        h = plan_horizon_s;
    }
    if (elapsed_s > h) {
        h = elapsed_s;
    }
    if (!(h <= UI_PAGE_HOME_GRAPH_MAX_HORIZON_S)) {
        h = UI_PAGE_HOME_GRAPH_MAX_HORIZON_S;
    }
    return h;
}

float ui_page_home_history_tolerance_s(float horizon_s, size_t point_count, float sample_period_s)
{
    float tol = sample_period_s;
    if (point_count > 1 && horizon_s > 0.0f) {
        float half_bucket = horizon_s / (float)(point_count - 1) * 0.5f;
        if (half_bucket > tol) {
            tol = half_bucket;
        }
    }
    return tol;
}
