#include "ui_page_history.h"

#include <math.h>
#include <stdio.h>

#include "MAX31856.h"
#include "dashboard_http.h"
#include "profile_executor.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"
#include "zones_http.h"

// See ui_page_history.h for why this page exists (moved off ui_page_home.c
// in the 2026-08-18 no-scroll rewrite). Contents/behavior are otherwise
// unchanged from the chart card ui_page_home.c used to build -- ONE
// combined chart, not one per zone (profile_executor.h's history ring
// buffer is itself single-series, "scoped to a single representative zone"
// -- see profile_executor.h's HISTORY_SAMPLE_PERIOD_S comment), windowed to
// the most recent UI_PAGE_HISTORY_CHART_POINTS ring-buffer samples. Chart
// height trimmed from 140px to 110px versus the pre-move version, purely to
// leave a comfortable margin against this page's own content budget (480x320
// landscape) alongside its label/legend -- the point count and windowing
// logic are unchanged. The in-content Back button that used to share this
// budget moved into the shared top bar (ui_topbar.c) in the 2026-08-21
// icon-topbar pass, freeing UI_THEME_MIN_TOUCH_TARGET_PX (72px) + the 4px
// row gap back to content.
//
// 2026-08-21: this page used to show a dead-end "Temperature history -- no
// run yet" label with an empty chart while idle. Per the same user request
// that put a compact chart back on ui_page_home.c ("i always want to see
// the graph... current temps should just show as dots and should stay on
// the left side of the graph until the profile is started"), idle now shows
// the SAME single-dot-pinned-at-index-0 behaviour ui_page_home.c uses
// (rewritten every refresh tick, not appended, so it never marches or
// accumulates), on this page's existing actual series -- s_chart_desired_pts
// stays LV_CHART_POINT_NONE throughout idle since there is no desired value
// without a running profile. The label text changes from the old dead-end
// message to "Current -- <zone>" so it still says something true instead of
// announcing a lack of history.
//
// 2026-08-21, PROGRESS BAR, part 1 (superseded by part 2 below the same day
// once the backend landed -- kept for history): same user request that added
// a progress bar under ui_page_home.c's chart. Landed first as a stopgap
// showing per-SEGMENT elapsed/remaining (profile_exec_status_t's
// segment_elapsed_s/dwell_remaining_s/dwelling) because the whole-profile
// duration accessor a parallel pass was adding to profile_executor.c/
// profile_feasibility.c/dashboard_http.c was not yet present anywhere under
// firmware/ when this was first written.
//
// 2026-08-21, part 2 (the backend landed later the same day -- CURRENT
// behaviour, mirrors ui_page_home.c's identical part 2 exactly): the bar now
// shows WHOLE-FIRING elapsed/remaining (profile_exec_status_t's
// total_elapsed_s and profile_feasibility_plan_curve()'s returned total),
// with the same honesty rules -- unknown total hides the bar and shows
// elapsed only; a known total always labels remaining "(estimate)" since
// ramp-lock overrun is never corrected for. The per-segment line part 1
// showed was dropped from the bar for the same "doesn't fit next to a
// whole-firing line" reason ui_page_home.c's copy gives, though this page
// has more spare height overall -- the progress_row itself is still just
// one label + one bar, same shape as before.
//
// 2026-08-21, PLANNED CURVE (also part 2, same landing): this page's chart
// gains a THIRD series -- the planned-ahead setpoint from
// profile_feasibility_plan_curve() (ACCENT_2), alongside the existing
// Actual (ACCENT_1) and the recorded, trailing Desired (ACCENT_3) this page
// already had. Unlike ui_page_home.c (which dropped trailing-Desired for
// space at 70px), this page keeps all three at 110px with legend room to
// label a third swatch -- genuinely useful here: Actual vs. recorded-Desired
// shows how the real run tracked its own (possibly ramp-lock-delayed)
// setpoint, while Planned shows what the schedule says should happen with
// no delay, so the gap between recorded-Desired and Planned IS the
// ramp-lock/overrun story. All three are now sampled at the SAME per-bucket
// time (0..the planned curve's horizon) instead of the old "last N
// ring-buffer samples" trailing window -- Actual and recorded-Desired come
// from the SAME single-entry profile_executor_get_history() read per bucket
// (one ring entry carries both actual_c and desired_c, so no extra reads),
// Planned from plan_lookup() over the curve's own points. Both Actual and
// recorded-Desired stop at "now" (real time > st.total_elapsed_s means no
// sample exists yet); Planned keeps going -- see ui_page_home.c's identical
// header comment (part 2) for the full reasoning, not repeated here.

static const char *TAG __attribute__((unused)) = "ui_page_history";

#define UI_PAGE_HISTORY_REFRESH_MS 1000
#define UI_PAGE_HISTORY_CHART_POINTS 60
#define UI_PAGE_HISTORY_CHART_HEIGHT_PX 110

static lv_obj_t *s_chart;
static lv_obj_t *s_chart_zone_label;
static lv_chart_series_t *s_chart_actual_series;
static lv_chart_series_t *s_chart_desired_series;   /* recorded, trailing setpoint (unchanged) */
static lv_chart_series_t *s_chart_planned_series;   /* planned-ahead setpoint, 2026-08-21 part 2 */
/* These arrays ARE the chart's backing store (lv_chart_set_series_ext_y_array()),
 * not a scratch copy, so they must outlive the chart -- static, matching
 * every other widget on this page's "built once, page never torn down"
 * lifetime (kiln_ui.h's header comment). s_chart_planned_pts is a NEW
 * allocation (2026-08-21 part 2): UI_PAGE_HISTORY_CHART_POINTS (60) * 4
 * bytes = 240 bytes, bringing this page's three chart arrays to 720 bytes
 * total (was 480 for two) -- still small next to the ~4167-byte internal-
 * DRAM headroom this codebase runs under, not moved to PSRAM. SEPARATE
 * arrays from ui_page_home.c's own chart arrays, same non-aliasing rule that
 * page's arrays document. */
static int32_t s_chart_actual_pts[UI_PAGE_HISTORY_CHART_POINTS];
static int32_t s_chart_desired_pts[UI_PAGE_HISTORY_CHART_POINTS];
static int32_t s_chart_planned_pts[UI_PAGE_HISTORY_CHART_POINTS];

/* Progress bar under the chart, 2026-08-21 -- see this file's header
 * comment ("PROGRESS BAR"). */
static lv_obj_t *s_time_label;
static lv_obj_t *s_progress_bar;

/* mm:ss for anything under an hour, hh:mm:ss beyond that -- same shape as
 * ui_page_home.c's format_duration() (not shared between the two files, but
 * kept identical on purpose so the two pages never disagree on how a
 * duration reads). */
static void format_duration(uint32_t seconds, char *out, size_t out_cap)
{
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    if (h > 0) {
        snprintf(out, out_cap, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(out, out_cap, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
    }
}

static void build_chart_legend_item(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(item, 4, 0);

    lv_obj_t *swatch = lv_obj_create(item);
    lv_obj_set_size(swatch, 12, 12);
    lv_obj_set_style_bg_color(swatch, color, 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(swatch, 0, 0);
    lv_obj_set_style_radius(swatch, 2, 0);

    lv_obj_t *label = lv_label_create(item);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(label, text);
}

/* 2026-08-21 owner request: "the charts should never show below freezing
 * temp." Mirrors ui_page_home.c's identical freezing_point_disp() (not
 * shared between the two files, same "mirrored not shared" reasoning as this
 * file's format_duration()/plan_lookup()) -- the clamp is on the AXIS, never
 * on the DATA: a genuine sub-zero/fault/disconnected-probe reading must still
 * be plotted and visible as an out-of-range excursion, so both call sites
 * below only raise their axis_lo to this floor when the real plotted minimum
 * is itself still at-or-above it (see each call site's own comment). */
static float freezing_point_disp(unit_pref_t unit)
{
    return (unit == UNIT_PREF_FAHRENHEIT) ? 32.0f : 0.0f;
}

static void refresh_progress(void);

/* Piecewise-linear sample of a profile_feasibility_plan_curve() point list --
 * identical to ui_page_home.c's plan_lookup() (not shared between the two
 * files, same reasoning as format_duration() above). See that file's
 * comment for the full rationale. */
static float plan_lookup(const profile_plan_point_t *pts, size_t n, float t)
{
    if (n == 0) {
        return NAN;
    }
    if (t <= pts[0].t) {
        return pts[0].c;
    }
    for (size_t i = 1; i < n; i++) {
        if (t <= pts[i].t) {
            float t0 = pts[i - 1].t, t1 = pts[i].t;
            float c0 = pts[i - 1].c, c1 = pts[i].c;
            if (t1 <= t0) {
                return c1;
            }
            float f = (t - t0) / (t1 - t0);
            return c0 + f * (c1 - c0);
        }
    }
    return pts[n - 1].c;
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Progress bar first, unconditionally -- the chart-repaint logic below
     * has early returns (idle single-dot path, "no new sample yet" path)
     * that must not skip the time/progress update too. */
    refresh_progress();

    profile_exec_status_t st;
    profile_executor_get_status(&st);

    bool idle_no_history = (st.state == PROFILE_EXEC_IDLE && profile_executor_get_history_count() == 0);

    /* Representative zone -- st.zone_mask is only meaningful once a run has
     * actually started (profile_exec_status_t's own scope note), so while
     * idle there is no mask to read yet; fall back to zone 0, the same
     * "first configured zone" convention ui_page_home.c's idle dot uses. */
    uint8_t zi = 0;
    if (!idle_no_history) {
        for (; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (st.zone_mask & (1u << zi)) break;
        }
    }
    char cfg_name[16];
    char zone_buf[24];
    if (zones_config_get_name(zi, cfg_name, sizeof(cfg_name)) && cfg_name[0] != '\0') {
        snprintf(zone_buf, sizeof(zone_buf), "%s", cfg_name);
    } else {
        snprintf(zone_buf, sizeof(zone_buf), "Zone %u", (unsigned)zi);
    }
    char buf[48];
    if (idle_no_history) {
        snprintf(buf, sizeof(buf), "Current -- %s", zone_buf);
    } else {
        snprintf(buf, sizeof(buf), "Actual vs Desired -- %s", zone_buf);
    }
    lv_label_set_text(s_chart_zone_label, buf);

    if (idle_no_history) {
        /* Single dot pinned at index 0 -- see this file's header comment
         * ("2026-08-21") and ui_page_home.c's matching idle-dot block.
         * Rewritten every tick, not appended, so it never marches across
         * the plot or accumulates a trail. */
        for (uint32_t i = 1; i < UI_PAGE_HISTORY_CHART_POINTS; i++) {
            s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
            s_chart_planned_pts[i] = LV_CHART_POINT_NONE;
        }
        dashboard_status_t ds;
        dashboard_get_status(&ds);
        float val = NAN;
        for (size_t i = 0; i < ds.channel_count; i++) {
            if (ds.channels[i].channel == zi) {
                if (ds.channels[i].valid && !ds.channels[i].stale) {
                    val = unit_pref_convert(ds.channels[i].temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE);
                }
                break;
            }
        }
        /* No desired/planned value without a running profile. */
        s_chart_desired_pts[0] = LV_CHART_POINT_NONE;
        s_chart_planned_pts[0] = LV_CHART_POINT_NONE;
        if (!isnan(val)) {
            int32_t v = (int32_t)lroundf(val);
            s_chart_actual_pts[0] = v;
            /* Freezing floor -- only raise axis_lo when the real point (v)
             * is itself at or above freezing; if v itself is below freezing
             * (a genuine sub-zero/fault reading), axis_lo is left at v-10
             * unclamped so the excursion stays visible instead of being
             * clamped off the bottom of the plot. */
            int32_t floor_i = (int32_t)lroundf(freezing_point_disp(ds.temp_unit));
            int32_t axis_lo = v - 10;
            int32_t axis_hi = v + 10;
            if (axis_lo < floor_i && v >= floor_i) {
                axis_lo = floor_i;
            }
            lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
        } else {
            s_chart_actual_pts[0] = LV_CHART_POINT_NONE;
        }
        lv_chart_refresh(s_chart);
        return;
    }

    /* Running/paused/done/faulted: segments[]/run_start_c/total_elapsed_s
     * are all meaningful once state != IDLE -- see ui_page_home.c's
     * identical block (part 2 of its header comment) for the full
     * reasoning this mirrors exactly, including why this is recomputed
     * every tick (not gated on a new ring-buffer sample) and why the actual/
     * recorded-desired lookup is a per-bucket single-entry read rather than
     * a bulk copy of the (up to HISTORY_MAX_SAMPLES=2880-entry) ring. */
    profile_plan_point_t plan_pts[1 + 2 * PROFILE_MAX_SEGMENTS];
    size_t plan_n = 0;
    int64_t total_planned_s = profile_feasibility_plan_curve(st.segments, st.segment_count, st.run_start_c,
                                                              plan_pts, sizeof(plan_pts) / sizeof(plan_pts[0]),
                                                              &plan_n);
    (void)total_planned_s; /* only the sign matters to refresh_progress(), which recomputes its own copy */
    float horizon_s = (plan_n > 0) ? plan_pts[plan_n - 1].t : 1.0f;
    if (horizon_s < 1.0f) {
        horizon_s = 1.0f;
    }

    size_t count = profile_executor_get_history_count();
    unit_pref_t unit = unit_pref_get();
    bool have_range = false;
    float lo = 0.0f, hi = 0.0f;
    for (uint32_t i = 0; i < UI_PAGE_HISTORY_CHART_POINTS; i++) {
        float t_i = (UI_PAGE_HISTORY_CHART_POINTS > 1)
                        ? (float)i * horizon_s / (float)(UI_PAGE_HISTORY_CHART_POINTS - 1)
                        : 0.0f;

        float planned_c = plan_lookup(plan_pts, plan_n, t_i);
        float planned_disp = unit_pref_convert(planned_c, unit, UNIT_PREF_KIND_ABSOLUTE);
        s_chart_planned_pts[i] = isnan(planned_disp) ? LV_CHART_POINT_NONE : (int32_t)lroundf(planned_disp);
        if (!isnan(planned_disp)) {
            if (!have_range) { lo = hi = planned_disp; have_range = true; }
            else { if (planned_disp < lo) lo = planned_disp; if (planned_disp > hi) hi = planned_disp; }
        }

        bool have_sample = false;
        float actual_c = NAN, desired_c = NAN;
        if (t_i <= (float)st.total_elapsed_s + (float)HISTORY_SAMPLE_PERIOD_S / 2.0f) {
            if (count > 0) {
                size_t idx = (size_t)lroundf(t_i / (float)HISTORY_SAMPLE_PERIOD_S);
                if (idx >= count) idx = count - 1;
                profile_history_entry_t entry;
                if (profile_executor_get_history(&entry, idx, 1) == 1) {
                    actual_c = entry.actual_c;
                    desired_c = entry.desired_c;
                    have_sample = true;
                }
            } else if (i == 0) {
                actual_c = st.run_start_c;
                desired_c = st.run_start_c; /* segment 0's own ramp starts here too */
                have_sample = true;
            }
        }
        if (have_sample) {
            float a = unit_pref_convert(actual_c, unit, UNIT_PREF_KIND_ABSOLUTE);
            float d = unit_pref_convert(desired_c, unit, UNIT_PREF_KIND_ABSOLUTE);
            s_chart_actual_pts[i] = isnan(a) ? LV_CHART_POINT_NONE : (int32_t)lroundf(a);
            s_chart_desired_pts[i] = isnan(d) ? LV_CHART_POINT_NONE : (int32_t)lroundf(d);
            if (!isnan(a)) {
                if (!have_range) { lo = hi = a; have_range = true; }
                else { if (a < lo) lo = a; if (a > hi) hi = a; }
            }
            if (!isnan(d)) {
                if (!have_range) { lo = hi = d; have_range = true; }
                else { if (d < lo) lo = d; if (d > hi) hi = d; }
            }
        } else {
            s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
        }
    }
    if (have_range) {
        float range = hi - lo;
        if (range < 1.0f) range = 1.0f;
        float pad_c = range * 0.1f;
        int32_t axis_lo = (int32_t)lroundf(lo - pad_c);
        int32_t axis_hi = (int32_t)lroundf(hi + pad_c);
        /* Freezing floor -- same guard as the idle-dot branch above: only
         * raise axis_lo when the real data minimum (lo, pre-padding) is
         * itself at or above freezing. If `lo` itself is below freezing (a
         * genuine sub-zero actual/desired/planned point), axis_lo is left
         * unclamped so that point stays plotted and visible rather than
         * being clipped off the bottom. */
        int32_t floor_i = (int32_t)lroundf(freezing_point_disp(unit));
        if (axis_lo < floor_i && lo >= floor_i) {
            axis_lo = floor_i;
        }
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
    }
    lv_chart_refresh(s_chart);
}

/* Progress bar under the chart -- WHOLE-FIRING elapsed/remaining, see this
 * file's header comment ("PROGRESS BAR, part 2"). Identical logic/wording to
 * ui_page_home.c's copy in its refresh_cb() -- kept as a straight duplicate
 * rather than a shared helper since these are two independent, never-torn-
 * down pages with their own static widget handles, same pattern as this
 * file's format_duration() above. Calls profile_feasibility_plan_curve()
 * a SECOND time here (refresh_cb()'s chart block already called it once this
 * tick) rather than threading the chart block's result through -- this
 * function has no access to refresh_cb()'s locals and duplicating a cheap
 * (<=25-point) pure-math call is simpler than plumbing a second output
 * parameter through refresh_cb() for one caller. */
static void refresh_progress(void)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    int64_t total_planned_s = -1;
    if (st.state != PROFILE_EXEC_IDLE) {
        total_planned_s = profile_feasibility_plan_curve(st.segments, st.segment_count, st.run_start_c,
                                                          NULL, 0, NULL);
    }

    char elapsed_buf[16];
    format_duration(st.total_elapsed_s, elapsed_buf, sizeof(elapsed_buf));
    if (st.state == PROFILE_EXEC_IDLE) {
        lv_label_set_text(s_time_label, "--");
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
    } else if (total_planned_s < 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Elapsed %s (total unknown)", elapsed_buf);
        lv_label_set_text(s_time_label, buf);
        /* HIDE the bar outright (not an opacity trick, not a fixed 0/100
         * value) -- see ui_page_home.c's identical branch for why either of
         * those would still misread as "empty"/"full." */
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        uint32_t total = (uint32_t)total_planned_s;
        uint32_t elapsed = st.total_elapsed_s;
        uint32_t remaining = (elapsed < total) ? (total - elapsed) : 0;
        char remaining_buf[16];
        format_duration(remaining, remaining_buf, sizeof(remaining_buf));
        char buf[80];
        snprintf(buf, sizeof(buf), "Elapsed %s / Remaining %s (estimate)", elapsed_buf, remaining_buf);
        lv_label_set_text(s_time_label, buf);
        int32_t pct = total > 0 ? (int32_t)((uint64_t)elapsed * 100u / total) : 100;
        if (pct > 100) pct = 100;
        lv_obj_remove_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
    }
}

lv_obj_t *ui_page_history_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Temperature History",
        .back_page = "config",
        .show_home = true,
    }, &tb);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(content);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(card, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_chart_zone_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_chart_zone_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_chart_zone_label, "Current -- Zone 0");

    s_chart = lv_chart_create(card);
    lv_obj_set_width(s_chart, lv_pct(100));
    lv_obj_set_height(s_chart, UI_PAGE_HISTORY_CHART_HEIGHT_PX);
    lv_obj_set_style_bg_color(s_chart, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 3, 5);
    lv_chart_set_point_count(s_chart, UI_PAGE_HISTORY_CHART_POINTS);

    s_chart_actual_series = lv_chart_add_series(s_chart, UI_THEME_ACCENT_1, LV_CHART_AXIS_PRIMARY_Y);
    s_chart_desired_series = lv_chart_add_series(s_chart, UI_THEME_ACCENT_3, LV_CHART_AXIS_PRIMARY_Y);
    s_chart_planned_series = lv_chart_add_series(s_chart, UI_THEME_ACCENT_2, LV_CHART_AXIS_PRIMARY_Y);
    for (uint32_t i = 0; i < UI_PAGE_HISTORY_CHART_POINTS; i++) {
        s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
        s_chart_planned_pts[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(s_chart, s_chart_actual_series, s_chart_actual_pts);
    lv_chart_set_series_ext_y_array(s_chart, s_chart_desired_series, s_chart_desired_pts);
    lv_chart_set_series_ext_y_array(s_chart, s_chart_planned_series, s_chart_planned_pts);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

    lv_obj_t *legend = lv_obj_create(card);
    lv_obj_set_width(legend, lv_pct(100));
    lv_obj_set_height(legend, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(legend, 0, 0);
    lv_obj_set_style_pad_all(legend, 0, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(legend, UI_THEME_PADDING_PX, 0);
    build_chart_legend_item(legend, "Actual", UI_THEME_ACCENT_1);
    build_chart_legend_item(legend, "Desired", UI_THEME_ACCENT_3);
    build_chart_legend_item(legend, "Planned", UI_THEME_ACCENT_2);

    /* Progress row -- see this file's header comment ("PROGRESS BAR").
     * This page has ample spare height (its only content is this one card),
     * so unlike ui_page_home.c's tightly-budgeted copy, no arithmetic is
     * needed here to prove it fits -- card is LV_SIZE_CONTENT and `content`
     * is a flex column with room to spare below it. */
    lv_obj_t *progress_row = lv_obj_create(card);
    lv_obj_set_width(progress_row, lv_pct(100));
    lv_obj_set_height(progress_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(progress_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_row, 0, 0);
    lv_obj_set_style_pad_all(progress_row, 0, 0);
    lv_obj_set_flex_flow(progress_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(progress_row, 4, 0);
    lv_obj_remove_flag(progress_row, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(progress_row);
    lv_obj_set_style_text_color(s_time_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_time_label, "--");

    s_progress_bar = lv_bar_create(progress_row);
    lv_obj_set_width(s_progress_bar, lv_pct(100));
    lv_obj_set_height(s_progress_bar, 10);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, UI_THEME_ACCENT_3, LV_PART_INDICATOR);

    ui_topbar_raise(&tb);

    lv_timer_create(refresh_cb, UI_PAGE_HISTORY_REFRESH_MS, NULL);
    refresh_cb(NULL);

    return scr;
}
