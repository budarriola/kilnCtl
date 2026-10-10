/* Planned-curve chart drawing helpers for the home page (ui_page_home.c) --
 * split out 2026-09-04, ROADMAP.md M15's 1500-line item. plan_lookup()'s
 * piecewise-linear evaluation, the dashed-planned-line and Y-tick-mark LVGL
 * draw-event hooks, and the Y/X-tick-label and legend layout helpers that
 * ui_page_home_refresh.c calls every tick. See ui_page_home_internal.h for
 * the shared statics/prototypes this file reaches across the split.
 */
#include "ui_page_home_internal.h"

float ui_home_plan_lookup(const profile_plan_point_t *pts, size_t n, float t)
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

/* LV_EVENT_DRAW_TASK_ADDED hook on s_ui_home_chart -- the only way to get a dashed
 * series line out of LVGL 9.5's lv_chart (grepped this tree's own
 * lv_chart.c/.h first: draw_series_line() builds one lv_draw_line_dsc_t per
 * series from LV_PART_ITEMS style properties and calls lv_draw_line() with
 * it -- there is no per-series "dashed" flag or style selector to reach any
 * other way). lv_draw_line_dsc_t itself DOES support dashing natively
 * (dash_width/dash_gap fields, draw/lv_draw_line.h) -- LVGL's line drawing
 * primitive can dash, the chart widget just never exposes it -- so this
 * intercepts each line draw task after the chart builds it and, for the one
 * belonging to the planned-profile series (identified by its already-applied
 * color, set from s_ui_home_chart_planned_series's own color at lv_chart_add_series()
 * time below), turns on the same 6-on/4-off dash main_page.html's
 * `ctx.setLineDash([6, 4])` uses. Every other draw task (the actual-series
 * line, any bullets, the card background/border) passes through untouched. */
void ui_home_chart_draw_event_cb(lv_event_t *e)
{
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_line_dsc_t *line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (line_dsc == NULL) {
        return; /* not a line task (rect/label/etc) -- nothing to dash */
    }
    lv_color_t plan_color = lv_color_hex(UI_PAGE_HOME_PLAN_COLOR_HEX);
    if (!lv_color_eq(line_dsc->color, plan_color)) {
        return; /* the actual-series line (or anything else), leave solid */
    }
    line_dsc->dash_width = UI_PAGE_HOME_PLAN_DASH_WIDTH_PX;
    line_dsc->dash_gap = UI_PAGE_HOME_PLAN_DASH_GAP_PX;
}

/* LV_EVENT_DRAW_POST hook on s_ui_home_chart -- draws the UI_PAGE_HOME_Y_TICK_COUNT
 * (6) short tick marks that
 * accompany s_ui_home_chart_y_tick_labels[], matching main_page.html's drawYAxis()
 * (`moveTo(padL - 3, vy) -> lineTo(padL, vy)`, a 3px stroke just outside the
 * plot in the border color). This chart's own pad_all is only 2px, so a
 * literal "3px further left" would sit past the card's own edge and risk
 * being clipped by the parent flex row; each tick is instead drawn 4px INTO
 * the plot's own left edge, in the same muted color as the labels (this
 * theme has no separate border token to mirror the web's g.border vs.
 * g.muted split -- see ui_home_chart_set_y_ticks()'s comment). Draw-event hook
 * chosen over 11 more lv_obj children for the same reason
 * ui_home_chart_draw_event_cb() (the dashed planned-line hook, above) uses one: no
 * per-tick lv_obj allocation, and it fires every render pass so it can never
 * drift out of sync with the label positions even if a stray extra layout
 * pass runs between ui_home_refresh_cb() ticks. */
void ui_home_chart_y_tick_draw_event_cb(lv_event_t *e)
{
    if (!s_ui_home_chart_y_ticks_visible || s_ui_home_chart_axis_hi <= s_ui_home_chart_axis_lo) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t content;
    lv_obj_get_content_coords(s_ui_home_chart, &content);
    int32_t height = content.y2 - content.y1;
    if (height <= 0) {
        return;
    }
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = UI_THEME_COLOR_TEXT_SECONDARY;
    dsc.width = 1;
    dsc.opa = LV_OPA_70;
    for (int k = 0; k < UI_PAGE_HOME_Y_TICK_COUNT; k++) {
        float frac = (float)k / (float)(UI_PAGE_HOME_Y_TICK_COUNT - 1);
        int32_t y = content.y2 - (int32_t)lroundf(frac * (float)height);
        dsc.p1 = (lv_point_precise_t){ content.x1, y };
        dsc.p2 = (lv_point_precise_t){ content.x1 + 4, y };
        lv_draw_line(layer, &dsc);
    }
}

/* Rewrites all UI_PAGE_HOME_Y_TICK_COUNT (6) Y-tick labels' text and position from axis_lo/axis_hi --
 * the SAME two values just passed to lv_chart_set_axis_range() at each call
 * site below, never read back from the chart, for the reason this file's
 * header comment on s_ui_home_chart_y_tick_labels gives. Also stashes them into
 * s_ui_home_chart_axis_lo/hi for ui_home_chart_y_tick_draw_event_cb() above. Static array
 * of lv_obj*, so this only ever calls lv_label_set_text()/lv_obj_set_pos()
 * on objects built once in ui_page_home_build() -- no allocation here, per
 * this task's "never allocate inside the refresh callback" requirement. */
void ui_home_chart_set_y_ticks(int32_t axis_lo, int32_t axis_hi, unit_pref_t unit)
{
    s_ui_home_chart_axis_lo = axis_lo;
    s_ui_home_chart_axis_hi = axis_hi;
    s_ui_home_chart_y_ticks_visible = (axis_hi > axis_lo);
    if (!s_ui_home_chart_y_ticks_visible) {
        for (int k = 0; k < UI_PAGE_HOME_Y_TICK_COUNT; k++) {
            lv_obj_add_flag(s_ui_home_chart_y_tick_labels[k], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Layout must be resolved before content coords mean anything -- same
     * guard ui_page_home_build()'s status-label width computation uses for
     * the analogous "page built detached" problem. Cheap to call every tick
     * (LVGL no-ops an already-clean layout). */
    lv_obj_update_layout(s_ui_home_chart);
    lv_area_t content;
    lv_obj_get_content_coords(s_ui_home_chart, &content);
    lv_area_t chart_coords;
    lv_obj_get_coords(s_ui_home_chart, &chart_coords);
    /* L29: lv_obj_set_pos() is relative to the parent's CONTENT area, so
     * content-local coordinates start at 0 (adding the pad again put every
     * label 2 px right/down). pad_top only bounds the top label. */
    int32_t pad_top = content.y1 - chart_coords.y1;
    int32_t content_top_local = 0;
    int32_t height = content.y2 - content.y1;

    for (int k = 0; k < UI_PAGE_HOME_Y_TICK_COUNT; k++) {
        float frac = (float)k / (float)(UI_PAGE_HOME_Y_TICK_COUNT - 1);
        int32_t value = (int32_t)lroundf((float)axis_lo + frac * (float)(axis_hi - axis_lo));

        char buf[16];
        snprintf(buf, sizeof(buf), "%d%s", (int)value, unit_pref_suffix(unit));
        lv_obj_t *label = s_ui_home_chart_y_tick_labels[k];
        lv_label_set_text(label, buf);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);

        /* k=0 is the BOTTOM tick (axis_lo), k=10 the TOP (axis_hi) -- same
         * orientation as main_page.html's vi loop (vy computed from a yTemp()
         * that maps minV to the bottom). content_top_local + height*(1-frac)
         * places it accordingly; the label's own font-scaled half-height is
         * subtracted so the text is vertically centred ON the tick rather
         * than hanging below it. */
        int32_t y_local = content_top_local + (int32_t)lroundf((1.0f - frac) * (float)height);
        int32_t y_pos = y_local - 5;
        if (y_pos < -pad_top) {
            y_pos = -pad_top; /* keep the top label's first pixel row inside the chart */
        }
        lv_obj_set_pos(label, 0, y_pos);
    }
}

void ui_home_chart_hide_y_ticks(void)
{
    /* axis_hi left <= axis_lo (0,0) so ui_home_chart_y_tick_draw_event_cb() also
     * skips drawing -- one flag, not two independent "don't draw" states to
     * keep in sync. */
    ui_home_chart_set_y_ticks(0, 0, UNIT_PREF_CELSIUS);
}

/* Rewrites/positions the four bottom time-axis ticks (or hides all four when
 * has_span is false) -- s_ui_home_chart_x_tick_labels' own comment explains why this
 * replaced the old single top-right span string. Values come from
 * ui_page_home_x_ticks()/ui_page_home_format_mmss() (ui_page_home_graph.c),
 * same host-tested M:SS shape the old label used, just laid out as four
 * separate positions instead of one pipe-joined string.
 *
 * Horizontal placement mirrors ui_home_chart_set_y_ticks()'s vertical placement: each
 * label's CENTER lands at its fractional x position across the content area,
 * then is clamped so no label's box crosses the plot's own left/right edges
 * (a raw center-anchor would let tick 0 hang half off the left edge and tick
 * 3 half off the right, since a text label has real width unlike a
 * zero-width tick mark). The left clamp additionally reserves
 * UI_PAGE_HOME_X_TICK_LEFT_MARGIN_PX so tick 0 ("0:00") never sits under the
 * Y-axis's own bottom tick label (s_ui_home_chart_y_tick_labels[0], anchored at
 * content x=2) -- the two would otherwise overlap in the plot's bottom-left
 * corner, which is exactly the "labels must not overlap" legibility
 * requirement this page is built under. Each label gets the same semi-opaque
 * background chip the old single label used (not the Y ticks' plain-text
 * style) because these sit low in the plot, right where the actual/planned
 * trace lines are often passing through near the end of a run. */
#define UI_PAGE_HOME_X_TICK_LEFT_MARGIN_PX 24

void ui_home_chart_set_x_ticks(float horizon_s, bool has_span)
{
    if (!has_span) {
        for (int k = 0; k < UI_PAGE_HOME_X_TICK_COUNT; k++) {
            lv_obj_add_flag(s_ui_home_chart_x_tick_labels[k], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    float ticks[4];
    ui_page_home_x_ticks(horizon_s, ticks);

    lv_obj_update_layout(s_ui_home_chart);
    lv_area_t content;
    lv_obj_get_content_coords(s_ui_home_chart, &content);
    lv_area_t chart_coords;
    lv_obj_get_coords(s_ui_home_chart, &chart_coords);
    /* L29: content-local, see ui_home_chart_set_y_ticks(). */
    int32_t content_left_local = 0;
    int32_t content_bottom_local = content.y2 - content.y1;
    int32_t width = content.x2 - content.x1;
    if (width <= 0) {
        for (int k = 0; k < UI_PAGE_HOME_X_TICK_COUNT; k++) {
            lv_obj_add_flag(s_ui_home_chart_x_tick_labels[k], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    int32_t left_bound = content_left_local + UI_PAGE_HOME_X_TICK_LEFT_MARGIN_PX;
    int32_t right_bound = content_left_local + width - 2;

    for (int k = 0; k < UI_PAGE_HOME_X_TICK_COUNT; k++) {
        char buf[16];
        ui_page_home_format_mmss((uint32_t)lroundf(ticks[k]), buf, sizeof(buf));
        lv_obj_t *label = s_ui_home_chart_x_tick_labels[k];
        lv_label_set_text(label, buf);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(label);
        int32_t label_w = lv_obj_get_width(label);
        int32_t label_h = lv_obj_get_height(label);

        float frac = (float)k / (float)(UI_PAGE_HOME_X_TICK_COUNT - 1);
        int32_t x_center = content_left_local + (int32_t)lroundf(frac * (float)width);
        int32_t x_local = x_center - label_w / 2;
        if (x_local < left_bound) {
            x_local = left_bound;
        }
        if (x_local + label_w > right_bound) {
            x_local = right_bound - label_w;
        }
        lv_obj_set_pos(label, x_local, content_bottom_local - label_h);
    }
}

/* Shows/hides and repositions the 0-2 legend rows built in
 * ui_page_home_build() -- per-row visibility comes from the host-tested
 * ui_page_home_legend_visibility() (ui_page_home_graph.c), never decided
 * here. Must be called AFTER ui_home_chart_set_x_ticks() for the same horizon_s/
 * has_span this tick: the legend anchors itself just ABOVE the bottom
 * tick-label row (s_ui_home_chart_x_tick_labels[0]'s own height, measured after that
 * call has given it real text) rather than sharing the bottom-right corner
 * with tick 3 (the rightmost, full-span tick, which ui_home_chart_set_x_ticks()'s
 * own right-clamp also parks in that same corner) -- the two would otherwise
 * overlap right where the actual/planned trace lines are often passing
 * through near the end of a run. Rows stack bottom-up so the LAST visible
 * row (row 1, "Plan", when both are shown) sits closest to the tick row and
 * row 0 ("Actual") sits above it. */
void ui_home_chart_set_legend(bool has_span, bool has_actual_multi, bool has_planned)
{
    bool show_actual, show_plan;
    ui_page_home_legend_visibility(has_span, has_actual_multi, has_planned, &show_actual, &show_plan);
    bool row_visible[UI_PAGE_HOME_LEGEND_ROWS] = { show_actual, show_plan };
    bool any_visible = show_actual || show_plan;
    if (!any_visible) {
        for (int k = 0; k < UI_PAGE_HOME_LEGEND_ROWS; k++) {
            lv_obj_add_flag(s_ui_home_chart_legend_row[k], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    for (int k = 0; k < UI_PAGE_HOME_LEGEND_ROWS; k++) {
        if (row_visible[k]) {
            lv_obj_remove_flag(s_ui_home_chart_legend_row[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui_home_chart_legend_row[k], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_update_layout(s_ui_home_chart);
    lv_area_t content;
    lv_obj_get_content_coords(s_ui_home_chart, &content);
    lv_area_t chart_coords;
    lv_obj_get_coords(s_ui_home_chart, &chart_coords);
    /* L29: content-local, see ui_home_chart_set_y_ticks(). */
    int32_t content_left_local = 0;
    int32_t content_bottom_local = content.y2 - content.y1;
    int32_t width = content.x2 - content.x1;
    if (width <= 0) {
        return;
    }
    int32_t right_bound = content_left_local + width - 2;

    /* Tick row's own rendered height -- valid to measure here because
     * ui_home_chart_set_x_ticks() has already run this tick and (when has_span) left
     * real text/a resolved layout on s_ui_home_chart_x_tick_labels[0]. A 2px buffer
     * keeps the legend from touching the tick text baseline-to-baseline. */
    int32_t tick_row_h = (int32_t)lv_obj_get_height(s_ui_home_chart_x_tick_labels[0]);
    int32_t legend_bottom_local = content_bottom_local - tick_row_h - 2;

    int32_t y = legend_bottom_local;
    for (int k = UI_PAGE_HOME_LEGEND_ROWS - 1; k >= 0; k--) {
        if (!row_visible[k]) {
            continue; /* not the old "hide the fixed 0-2 count's tail" case --
                       * row 0 (Actual) can now be hidden while row 1 (Plan)
                       * stays visible, see ui_page_home_legend_visibility()'s
                       * header comment. */
        }
        lv_obj_t *row = s_ui_home_chart_legend_row[k];
        int32_t row_h = lv_obj_get_height(row);
        int32_t row_w = lv_obj_get_width(row);
        y -= row_h;
        int32_t x = right_bound - row_w;
        if (x < content_left_local) {
            x = content_left_local; /* never run off the left edge if the plot is very narrow */
        }
        lv_obj_set_pos(row, x, y);
    }
}

