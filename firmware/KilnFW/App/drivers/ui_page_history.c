#include "ui_page_history.h"

#include <math.h>
#include <stdio.h>

#include "MAX31856.h"
#include "profile_executor.h"
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

static const char *TAG __attribute__((unused)) = "ui_page_history";

#define UI_PAGE_HISTORY_REFRESH_MS 1000
#define UI_PAGE_HISTORY_CHART_POINTS 60
#define UI_PAGE_HISTORY_CHART_HEIGHT_PX 110

static lv_obj_t *s_chart;
static lv_obj_t *s_chart_zone_label;
static lv_chart_series_t *s_chart_actual_series;
static lv_chart_series_t *s_chart_desired_series;
/* These arrays ARE the chart's backing store (lv_chart_set_series_ext_y_array()),
 * not a scratch copy, so they must outlive the chart -- static, matching
 * every other widget on this page's "built once, page never torn down"
 * lifetime (kiln_ui.h's header comment). */
static int32_t s_chart_actual_pts[UI_PAGE_HISTORY_CHART_POINTS];
static int32_t s_chart_desired_pts[UI_PAGE_HISTORY_CHART_POINTS];
static size_t s_chart_last_count;

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

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    profile_exec_status_t st;
    profile_executor_get_status(&st);

    if (st.state == PROFILE_EXEC_IDLE && profile_executor_get_history_count() == 0) {
        lv_label_set_text(s_chart_zone_label, "Temperature history -- no run yet");
    } else {
        uint8_t zi = 0;
        for (; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (st.zone_mask & (1u << zi)) break;
        }
        char cfg_name[16];
        char zone_buf[24];
        if (zones_config_get_name(zi, cfg_name, sizeof(cfg_name)) && cfg_name[0] != '\0') {
            snprintf(zone_buf, sizeof(zone_buf), "%s", cfg_name);
        } else {
            snprintf(zone_buf, sizeof(zone_buf), "Zone %u", (unsigned)zi);
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "Actual vs Desired -- %s", zone_buf);
        lv_label_set_text(s_chart_zone_label, buf);
    }

    size_t count = profile_executor_get_history_count();
    if (count == s_chart_last_count) {
        return; /* buffer hasn't gained a new sample since the last repaint */
    }
    s_chart_last_count = count;

    if (count == 0) {
        for (uint32_t i = 0; i < UI_PAGE_HISTORY_CHART_POINTS; i++) {
            s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
        }
        lv_chart_refresh(s_chart);
        return;
    }

    size_t n = count < UI_PAGE_HISTORY_CHART_POINTS ? count : UI_PAGE_HISTORY_CHART_POINTS;
    size_t start = count - n;
    profile_history_entry_t batch[UI_PAGE_HISTORY_CHART_POINTS];
    size_t got = profile_executor_get_history(batch, start, n);
    size_t pad = UI_PAGE_HISTORY_CHART_POINTS - got;
    for (size_t i = 0; i < pad; i++) {
        s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
    }

    /* ROADMAP.md 2026-08-21 shared unit preference. batch[i].actual_c/
     * desired_c are live/target temperature READINGS (absolute, not rates),
     * straight from profile_executor.c's history ring buffer -- that buffer
     * itself is unaffected (still Celsius on the wire/in RAM, per
     * unit_pref.h's display-only rule); only this chart's rendering of it
     * converts, same boundary point as every other renderer in this pass. */
    unit_pref_t unit = unit_pref_get();
    bool have_range = false;
    float lo = 0.0f, hi = 0.0f;
    for (size_t i = 0; i < got; i++) {
        float a = unit_pref_convert(batch[i].actual_c, unit, UNIT_PREF_KIND_ABSOLUTE);
        float d = unit_pref_convert(batch[i].desired_c, unit, UNIT_PREF_KIND_ABSOLUTE);
        s_chart_actual_pts[pad + i] = isnan(a) ? LV_CHART_POINT_NONE : (int32_t)lroundf(a);
        s_chart_desired_pts[pad + i] = isnan(d) ? LV_CHART_POINT_NONE : (int32_t)lroundf(d);
        if (!isnan(a)) {
            if (!have_range) { lo = hi = a; have_range = true; } else { if (a < lo) lo = a; if (a > hi) hi = a; }
        }
        if (!isnan(d)) {
            if (!have_range) { lo = hi = d; have_range = true; } else { if (d < lo) lo = d; if (d > hi) hi = d; }
        }
    }
    if (have_range) {
        float range = hi - lo;
        if (range < 1.0f) range = 1.0f;
        float pad_c = range * 0.1f;
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lroundf(lo - pad_c),
                                (int32_t)lroundf(hi + pad_c));
    }
    lv_chart_refresh(s_chart);
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
    lv_label_set_text(s_chart_zone_label, "Temperature history -- no run yet");

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
    for (uint32_t i = 0; i < UI_PAGE_HISTORY_CHART_POINTS; i++) {
        s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(s_chart, s_chart_actual_series, s_chart_actual_pts);
    lv_chart_set_series_ext_y_array(s_chart, s_chart_desired_series, s_chart_desired_pts);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_chart_last_count = (size_t)-1;

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

    ui_topbar_raise(&tb);

    lv_timer_create(refresh_cb, UI_PAGE_HISTORY_REFRESH_MS, NULL);
    refresh_cb(NULL);

    return scr;
}
