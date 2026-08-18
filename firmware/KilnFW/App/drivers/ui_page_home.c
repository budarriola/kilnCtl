#include "ui_page_home.h"

#include <math.h>
#include <stdio.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "profile_executor.h"
#include "profiles_http.h"
#include "run_state.h"
#include "ui_theme.h"
#include "wifi_status_ui.h"
#include "zones_http.h"

// TODO.md 10.3's real main/status page -- replaces the pre-10.2 placeholder
// (see git history / TODO.md 10.1's status update for what used to live
// here). Per TODO.md 10.1a, every number on this page comes from the SAME
// plain-C getters dashboard_http.c's HTTP handlers call
// (dashboard_get_status(), profile_executor_get_status()) and every button
// calls the SAME action functions the web dashboard's POST handlers call
// (profile_executor_run()/_halt()) -- nothing here reimplements a read or
// write against kiln_io/MAX31856/profile_executor a second time.
//
// Built this pass: per-zone temp + heater on/off (zones_config_get_thermo_count()
// zones, legacy zone_index == MAX31856 channel mapping, same as
// dashboard_http.c's status_get_handler()), profile name/state, segment
// elapsed + dwell-remaining time as text AND a progress bar, Start/Stop, and
// "Configuration"/"Temperature" nav buttons to the new stub pages.
//
// Also built this pass (previously deferred, TODO.md 10.3's status note):
// - Zone names: zones_config_get_name() (new getter, zones_http.c/.h) reads
//   the same zone_cfg_t::name the JSON API already exposed but nothing
//   plain-C could read -- "Zone N" is now only the fallback for a zone that
//   has never been given a name, not the permanent label.
// - The desired-vs-actual temperature graph: a real lv_chart, plotted
//   against profile_executor_get_history()/_get_history_count() (TODO.md
//   section 0's ring buffer, the same data GET /api/history.csv streams).
//   One combined chart, not one per zone -- see build_graph_card()'s comment
//   for why (the ring buffer itself is single-series, not per-zone).
// - A profile picker: an lv_dropdown populated via profiles_http_get()
//   (the same read-only accessor profile_executor.c already uses, looped
//   over the 8 fixed NVS slots -- no new profiles_http.c getter needed, that
//   loop already exists as profiles_list_get_handler()'s pattern for
//   GET /api/profiles, just not previously exposed to a non-HTTP caller in
//   a form this page could reuse directly). Start now uses the picker's
//   selection when the operator has touched it; falls back to the previous
//   boot-state logic otherwise -- see start_btn_cb()'s comment.
//
// 2026-08-18: status bar WiFi/IP/mDNS readout (s_status_label, see
// wifi_status_ui_get_text(), wifi_status_ui.c) -- connection state, station
// IP, and kiln.local reachability, all via wifi_prov.h getters
// wifi_provision_http.c's status_get_handler() already calls plus the mdns
// component's own mdns_hostname_get() (TODO.md 10.1a's shared-backend rule).
// TODO.md 10.9 relocated the actual formatter into wifi_status_ui.c/.h so
// ui_page_network.c (new this pass) could call the same implementation
// instead of a second copy of the same switch statement.

static const char *TAG = "ui_page_home";

/* Refresh cadence for the live numbers on this page. 1 Hz matches
 * PROFILE_EXECUTOR_TICK_MS (profile_executor.h) -- no point refreshing
 * faster than the control loop that produces the numbers changes them. */
#define UI_PAGE_HOME_REFRESH_MS 1000

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name_label;
    lv_obj_t *temp_label;
    lv_obj_t *heat_label;
} zone_widgets_t;

static zone_widgets_t s_zone[MAX31856_CHANNEL_COUNT];
static uint8_t s_zone_count; /* zones_config_get_thermo_count() at build time */

static lv_obj_t *s_profile_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_stop_btn;

/* WiFi/IP/mDNS status readout, added to the status bar this pass. Text comes
 * from wifi_status_ui_get_text() (wifi_status_ui.c) -- see that module's
 * header comment for the underlying getters and the TODO.md 10.1a
 * shared-backend rule. */
static lv_obj_t *s_status_label;

/* Graph card (TODO.md 10.3, previously a labeled placeholder -- see this
 * file's header comment). Windowed to the most recent
 * UI_PAGE_HOME_CHART_POINTS ring-buffer samples rather than the whole
 * HISTORY_MAX_SAMPLES (2880, 24h) history: at 30s/sample this window is 30
 * minutes, which is what actually fits legibly on a 480px-wide panel, and
 * profile_executor_get_history() is already paged for exactly this kind of
 * "give me a slice, not the whole buffer" caller. Series data is handed to
 * LVGL via lv_chart_set_series_ext_y_array() -- these two arrays ARE the
 * chart's backing store, not a scratch copy, so they must outlive the chart
 * (static, matching every other widget on this page's "built once, page
 * never torn down" lifetime -- kiln_ui.h's header comment). */
#define UI_PAGE_HOME_CHART_POINTS 60
static lv_obj_t *s_chart;
static lv_obj_t *s_chart_zone_label;
static lv_chart_series_t *s_chart_actual_series;
static lv_chart_series_t *s_chart_desired_series;
static int32_t s_chart_actual_pts[UI_PAGE_HOME_CHART_POINTS];
static int32_t s_chart_desired_pts[UI_PAGE_HOME_CHART_POINTS];
/* Last profile_executor_get_history_count() this page redrew the chart at --
 * TODO.md 10.3's "only refresh the chart when the buffer's sample count has
 * actually advanced" rule, since the ring buffer only gains a new sample
 * every HISTORY_SAMPLE_PERIOD_S (30s) and redrawing an unchanged chart every
 * UI_PAGE_HOME_REFRESH_MS (1s) tick would be pure wasted LVGL render work. */
static size_t s_chart_last_count;

/* Profile picker (TODO.md 10.3). Holds the operator's dropdown selection as
 * a slot id (0..PROFILES_MAX_COUNT-1) plus whether they have actually made
 * one this session -- see start_btn_cb()'s comment for why "has the operator
 * touched the dropdown" matters and isn't the same question as "what does
 * the dropdown currently show" (it always shows *something* once populated,
 * even before a deliberate selection). */
static lv_obj_t *s_profile_picker;
static uint8_t s_picker_profile_ids[PROFILES_MAX_COUNT]; /* dropdown option index -> slot id */
static uint8_t s_picker_option_count;
static bool s_picker_touched;

static lv_color_t zone_accent(uint8_t zone_index)
{
    switch (zone_index % 5) {
    case 0: return UI_THEME_ACCENT_1;
    case 1: return UI_THEME_ACCENT_2;
    case 2: return UI_THEME_ACCENT_3;
    case 3: return UI_THEME_ACCENT_4;
    default: return UI_THEME_ACCENT_5;
    }
}

/* mm:ss for anything under an hour (this page's numbers are segment-scale,
 * not multi-day), hh:mm:ss beyond that -- matches main_page.html's
 * fmtDuration() shape closely enough for web/LCD parity (TODO.md 10.5)
 * without pulling in the exact same JS-derived format. */
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

static const char *exec_state_label(profile_exec_state_t s)
{
    switch (s) {
    case PROFILE_EXEC_IDLE: return "Idle";
    case PROFILE_EXEC_RUNNING: return "Running";
    case PROFILE_EXEC_PAUSED: return "Paused";
    case PROFILE_EXEC_DONE: return "Done";
    case PROFILE_EXEC_FAULTED: return "Faulted";
    default: return "Unknown";
    }
}

/* TODO.md 10.3's profile picker -- marks that the operator has deliberately
 * chosen a slot, distinct from the dropdown merely showing option 0 because
 * that's what an untouched lv_dropdown always displays. Only LV_EVENT_VALUE_CHANGED
 * fires this, never the initial populate, so s_picker_touched staying false
 * really does mean "operator hasn't chosen" -- see start_btn_cb(). */
static void profile_picker_changed_cb(lv_event_t *e)
{
    (void)e;
    s_picker_touched = true;
}

static void start_btn_cb(lv_event_t *e)
{
    (void)e;

    /* Same action function dashboard_http.c's POST /api/profile_exec/start
     * handler calls (profile_exec_start_post_handler()) -- TODO.md 10.1a.
     * The only thing specific to this page is how it picks WHICH profile_id
     * to pass.
     *
     * TODO.md 10.3: prefer the operator's explicit picker selection over the
     * previous-pass "whatever's already known this boot" fallback below --
     * an operator who deliberately picked profile 3 from the dropdown almost
     * certainly wants profile 3, not the last thing that happened to run.
     * The fallback stays for the case the picker was never touched (e.g. an
     * operator who wants to just re-press Start on whatever DONE/FAULTED and
     * hasn't opened the dropdown), documented as a real, intentional
     * behavior split rather than the "gap, not a placeholder" this file's
     * previous-pass comment described -- that gap is now closed for the
     * common case (operator uses the picker) and only the fallback's own
     * pre-existing limits (nothing known this boot, no picker touch either)
     * remain. */
    bool have_id = false;
    uint8_t id = 0;
    if (s_picker_touched && s_picker_option_count > 0) {
        uint32_t sel = lv_dropdown_get_selected(s_profile_picker);
        if (sel < s_picker_option_count) {
            id = s_picker_profile_ids[sel];
            have_id = true;
        }
    }

    if (!have_id) {
        profile_exec_status_t st;
        profile_executor_get_status(&st);
        if (st.state != PROFILE_EXEC_IDLE) {
            id = st.profile_id;
            have_id = true;
        }
    }
    if (!have_id) {
        run_state_record_t rec;
        if (run_state_get_boot_record(&rec)) {
            id = rec.profile_id;
            have_id = true;
        }
    }
    if (!have_id) {
        ESP_LOGW(TAG, "Start pressed with no known profile id -- picker untouched/empty "
                      "and nothing has run this boot (TODO.md 10.3)");
        return;
    }

    char err_msg[64] = "";
    if (!profile_executor_run(id, err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "profile_executor_run(%u) refused: %s", id, err_msg);
    }
}

static void stop_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Same action function dashboard_http.c's POST /api/profile_exec/stop
     * handler calls -- TODO.md 10.1a. */
    profile_executor_halt();
}

static void config_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("config");
}

static void temperature_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("temperature");
}

static lv_obj_t *build_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    /* TODO.md 10.4's touch hit-area helper (ui_theme.c) -- this is a sparse
     * button row, not a dense grid, so compact_layout=false. Forces an
     * immediate layout pass first: ui_theme_apply_touch_area() reads back
     * lv_obj_get_width/height(), which flex_grow leaves unresolved until
     * layout actually runs. */
    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);

    return btn;
}

static void build_zone_row(lv_obj_t *parent, uint8_t zone_index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, zone_accent(zone_index), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_color(name, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char name_buf[24];
    /* zones_config_get_name() (new getter, TODO.md 10.3) -- "Zone N" is now
     * only the fallback for a zone that returns false (out of range, should
     * never trip here) or a real but empty name (a configured zone the
     * operator has never named). */
    char cfg_name[16];
    if (zones_config_get_name(zone_index, cfg_name, sizeof(cfg_name)) && cfg_name[0] != '\0') {
        snprintf(name_buf, sizeof(name_buf), "%s", cfg_name);
    } else {
        snprintf(name_buf, sizeof(name_buf), "Zone %u", (unsigned)zone_index);
    }
    lv_label_set_text(name, name_buf);

    lv_obj_t *temp = lv_label_create(row);
    lv_obj_set_style_text_color(temp, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(temp, "-- C");

    lv_obj_t *heat = lv_label_create(row);
    lv_obj_set_style_text_color(heat, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(heat, "unknown");

    s_zone[zone_index].row = row;
    s_zone[zone_index].name_label = name;
    s_zone[zone_index].temp_label = temp;
    s_zone[zone_index].heat_label = heat;
}

/* Small "swatch + text" legend entry -- lv_chart has no built-in legend
 * widget, and a single-color lv_label can't recolor part of its own text, so
 * a tiny colored lv_obj square next to a plain label is the straightforward
 * way to say "this line is Actual, that one is Desired" without pulling in
 * LVGL's recolor-markup feature for two words. */
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

/* TODO.md 10.3's desired-vs-actual temperature graph. ONE combined chart,
 * not one per zone: profile_executor.h's history ring buffer is itself
 * single-series ("scoped to a single representative zone" -- the
 * lowest-indexed active zone in the run's zone_mask, see
 * profile_executor.h's HISTORY_SAMPLE_PERIOD_S comment), not per-zone data,
 * so a per-zone chart would just be N-1 empty charts and one real one. If
 * the backend ever grows real per-zone history this should become per-zone
 * too, but there's nothing to plot per-zone today.
 *
 * Data is handed to the chart via lv_chart_set_series_ext_y_array() against
 * the s_chart_*_pts arrays -- see those statics' comment for why they're
 * static and why that's safe. */
static void build_graph_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(card, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, UI_THEME_PADDING_PX / 2, 0);

    s_chart_zone_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_chart_zone_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_chart_zone_label, "Temperature history -- no run yet");

    s_chart = lv_chart_create(card);
    lv_obj_set_width(s_chart, lv_pct(100));
    lv_obj_set_height(s_chart, 140);
    lv_obj_set_style_bg_color(s_chart, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 3, 5);
    /* Point count must be set before the ext-array assignment below --
     * lv_chart_set_series_ext_y_array() (lv_chart.c) just swaps the series'
     * y_points pointer to whatever's handed in and trusts it's sized for
     * the chart's current point count; setting point_count afterward would
     * leave that trust broken. */
    lv_chart_set_point_count(s_chart, UI_PAGE_HOME_CHART_POINTS);

    s_chart_actual_series = lv_chart_add_series(s_chart, UI_THEME_ACCENT_1, LV_CHART_AXIS_PRIMARY_Y);
    s_chart_desired_series = lv_chart_add_series(s_chart, UI_THEME_ACCENT_3, LV_CHART_AXIS_PRIMARY_Y);
    for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
        s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(s_chart, s_chart_actual_series, s_chart_actual_pts);
    lv_chart_set_series_ext_y_array(s_chart, s_chart_desired_series, s_chart_desired_pts);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100); /* placeholder until real data sets it */
    s_chart_last_count = (size_t)-1; /* force the first refresh_cb() tick to actually populate the chart */

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
}

/* Repaints the graph card built above. Called every refresh_cb() tick, but
 * the actual chart repaint is gated on profile_executor_get_history_count()
 * having advanced -- TODO.md 10.3's "the ring buffer only samples every 30s
 * ... don't re-read/redraw the whole chart every second" rule. The zone
 * label is cheap enough to update unconditionally. */
static void refresh_graph(const profile_exec_status_t *st)
{
    if (st->state == PROFILE_EXEC_IDLE && profile_executor_get_history_count() == 0) {
        lv_label_set_text(s_chart_zone_label, "Temperature history -- no run yet");
    } else {
        uint8_t zi = 0;
        for (; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (st->zone_mask & (1u << zi)) break;
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
        /* A new run just reset the buffer (profile_executor_run() clears it
         * at start) -- blank the chart rather than leaving the previous
         * run's stale trace on screen. */
        for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
            s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
        }
        lv_chart_refresh(s_chart);
        return;
    }

    size_t n = count < UI_PAGE_HOME_CHART_POINTS ? count : UI_PAGE_HOME_CHART_POINTS;
    size_t start = count - n; /* most-recent-N window, not the whole buffer */
    profile_history_entry_t batch[UI_PAGE_HOME_CHART_POINTS];
    size_t got = profile_executor_get_history(batch, start, n);
    size_t pad = UI_PAGE_HOME_CHART_POINTS - got; /* leading empty slots while the run is younger than the window */
    for (size_t i = 0; i < pad; i++) {
        s_chart_actual_pts[i] = LV_CHART_POINT_NONE;
        s_chart_desired_pts[i] = LV_CHART_POINT_NONE;
    }

    bool have_range = false;
    float lo = 0.0f, hi = 0.0f;
    for (size_t i = 0; i < got; i++) {
        float a = batch[i].actual_c;
        float d = batch[i].desired_c;
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
        /* Dynamic range rather than a fixed guessed ceiling (this codebase's
         * ui_theme.h is explicit about not wanting more unverified guessed
         * constants) -- padded 10% on each side so the plotted lines don't
         * touch the chart's top/bottom edge. */
        float range = hi - lo;
        if (range < 1.0f) range = 1.0f;
        float pad_c = range * 0.1f;
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lroundf(lo - pad_c),
                                (int32_t)lroundf(hi + pad_c));
    }
    lv_chart_refresh(s_chart);
}

/* TODO.md 10.3's profile picker. Loops the fixed PROFILES_MAX_COUNT (8) NVS
 * slots through profiles_http_get() -- the same read-only accessor
 * profile_executor.c already calls, and the same one profiles_http.c's own
 * GET /api/profiles handler (profiles_list_get_handler()) loops over its
 * internal storage to build; looping the PUBLIC getter here instead of
 * reaching into profiles_http.c internals is what keeps this a second
 * caller of an existing seam rather than a reimplementation (TODO.md
 * 10.1a's rule, applied to a getter that already existed here instead of
 * one this page had to add). */
static void build_profile_picker(lv_obj_t *parent)
{
    char options[PROFILES_MAX_COUNT * (PROFILE_NAME_MAX_LEN + 8)];
    size_t o = 0;
    options[0] = '\0';
    s_picker_option_count = 0;

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        profile_t p;
        if (!profiles_http_get(id, &p)) {
            continue; /* unused slot */
        }
        int n = snprintf(options + o, sizeof(options) - o, "%s%s", s_picker_option_count > 0 ? "\n" : "",
                         p.name[0] ? p.name : "(unnamed)");
        if (n < 0 || (size_t)n >= sizeof(options) - o) {
            break; /* out of room -- leaves the options already appended intact */
        }
        o += (size_t)n;
        s_picker_profile_ids[s_picker_option_count] = id;
        s_picker_option_count++;
    }

    s_profile_picker = lv_dropdown_create(parent);
    lv_obj_set_width(s_profile_picker, lv_pct(100));
    if (s_picker_option_count > 0) {
        lv_dropdown_set_options(s_profile_picker, options);
    } else {
        lv_dropdown_set_options(s_profile_picker, "No saved profiles");
        lv_obj_add_state(s_profile_picker, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(s_profile_picker, profile_picker_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_update_layout(s_profile_picker);
    ui_theme_apply_touch_area(s_profile_picker, false);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* wifi_status_ui.h -- TODO.md 10.9 factored this formatter out of this
     * file into its own shared module so ui_page_network.c can call the
     * exact same "human-readable WiFi state" text instead of a second copy
     * of the switch statement that used to live here. */
    char status_buf[64];
    wifi_status_ui_get_text(status_buf, sizeof(status_buf));
    lv_label_set_text(s_status_label, status_buf);

    /* Both calls below are the exact same plain-C getters
     * dashboard_http.c's GET /api/status and GET /api/profile_exec handlers
     * call -- TODO.md 10.1a's shared-backend rule, not a reimplementation. */
    dashboard_status_t ds;
    dashboard_get_status(&ds);
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    for (uint8_t zi = 0; zi < s_zone_count; zi++) {
        char buf[24];
        const dashboard_channel_status_t *ch = NULL;
        for (size_t i = 0; i < ds.channel_count; i++) {
            /* Legacy zone_index == MAX31856 channel mapping, same as
             * dashboard_http.c (zones_config_apply_cal()'s scope note). */
            if (ds.channels[i].channel == zi) {
                ch = &ds.channels[i];
                break;
            }
        }
        if (ch && ch->valid) {
            snprintf(buf, sizeof(buf), "%.1f C", (double)ch->temp_c);
        } else {
            snprintf(buf, sizeof(buf), "-- C");
        }
        lv_label_set_text(s_zone[zi].temp_label, buf);

        uint8_t relay_mask = 0;
        bool heat_on = false;
        bool have_mask = zones_config_get_relay_mask(zi, &relay_mask);
        if (have_mask && ds.io_ready) {
            for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
                if ((relay_mask & (1u << r)) && ds.relay_on[r]) {
                    heat_on = true;
                    break;
                }
            }
        }
        if (!ds.io_ready) {
            lv_label_set_text(s_zone[zi].heat_label, "no relay board");
        } else {
            lv_label_set_text(s_zone[zi].heat_label, heat_on ? "HEATING" : "off");
        }
        lv_obj_set_style_text_color(s_zone[zi].heat_label,
                                     heat_on ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }

    if (st.state == PROFILE_EXEC_IDLE) {
        lv_label_set_text(s_profile_label, "No profile running");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s", st.profile_name[0] ? st.profile_name : "(unnamed)");
        lv_label_set_text(s_profile_label, buf);
    }
    lv_label_set_text(s_state_label, exec_state_label(st.state));

    /* Elapsed/remaining are per-SEGMENT, not whole-profile totals -- that is
     * all profile_executor_get_status() computes (see profile_executor.h),
     * and it is exactly what main_page.html already shows for GET
     * /api/profile_exec ("Into the segment" / "Dwell remaining"), so this
     * matches section 2's web dashboard rather than inventing a
     * whole-profile total the backend doesn't track. */
    char elapsed_buf[16];
    format_duration(st.segment_elapsed_s, elapsed_buf, sizeof(elapsed_buf));
    if (st.dwelling) {
        char remaining_buf[16];
        format_duration(st.dwell_remaining_s, remaining_buf, sizeof(remaining_buf));
        /* 64, not 48: "Elapsed " + up to 15 bytes of elapsed_buf + " / Remaining "
         * + up to 15 bytes of remaining_buf can reach 51 bytes plus the NUL --
         * -Werror=format-truncation caught this statically (GCC bounds %s by
         * the source buffer's declared size, not format_duration()'s actual
         * output, which is much shorter in practice but not something GCC can
         * prove). Found building 2026-08-18. */
        char buf[64];
        snprintf(buf, sizeof(buf), "Elapsed %s / Remaining %s", elapsed_buf, remaining_buf);
        lv_label_set_text(s_time_label, buf);
        uint32_t total = st.segment_elapsed_s + st.dwell_remaining_s;
        int32_t pct = total > 0 ? (int32_t)((uint64_t)st.segment_elapsed_s * 100u / total) : 0;
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
    } else if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Elapsed %s (ramping)", elapsed_buf);
        lv_label_set_text(s_time_label, buf);
        /* No total ramp duration is tracked anywhere in the backend --
         * see the header comment above. 0 rather than a fabricated
         * percentage. */
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    } else {
        lv_label_set_text(s_time_label, "--");
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }

    refresh_graph(&st);
}

lv_obj_t *ui_page_home_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX, 0);

    /* Status bar (TODO.md 10.2's persistent top bar -- just a title here,
     * global chrome shared across pages is still explicitly deferred per
     * kiln_ui.h's header comment). */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "kilnCtl");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* WiFi/IP/mDNS status readout (see wifi_status_text()'s comment) --
     * right side of the same status bar, kept to one line at this bar's
     * UI_THEME_STATUS_BAR_HEIGHT_PX height. */
    s_status_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_status_label, "WiFi: --");
    lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Scrollable content area -- 480x320 is tight for zones + profile/time +
     * graph placeholder + buttons all at once, and this page's exact widget
     * sizing has never been checked against the real panel (no hardware this
     * pass, same caveat as ui_theme.h). Scrolling is the safe fallback
     * rather than guessing pixel-perfect fixed heights. */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX, 0);

    /* Zones (TODO.md 10.3: "each configured zone, its current temperature,
     * and its heater on/off status"). Widgets built for however many zones
     * are configured right now; a config change mid-session (no live editor
     * on this page yet anyway) is not re-observed until the next boot. */
    s_zone_count = zones_config_get_thermo_count();
    if (s_zone_count > MAX31856_CHANNEL_COUNT) {
        s_zone_count = MAX31856_CHANNEL_COUNT; /* defensive; should never trip */
    }
    if (s_zone_count == 0) {
        lv_obj_t *none = lv_label_create(content);
        lv_obj_set_style_text_color(none, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(none, "No zones configured");
    } else {
        for (uint8_t zi = 0; zi < s_zone_count; zi++) {
            build_zone_row(content, zi);
        }
    }

    /* Profile / run state card. */
    lv_obj_t *profile_card = lv_obj_create(content);
    lv_obj_set_width(profile_card, lv_pct(100));
    lv_obj_set_height(profile_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(profile_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(profile_card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(profile_card, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(profile_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(profile_card, UI_THEME_PADDING_PX / 2, 0);

    s_profile_label = lv_label_create(profile_card);
    lv_obj_set_style_text_color(s_profile_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_profile_label, "No profile running");

    s_state_label = lv_label_create(profile_card);
    lv_obj_set_style_text_color(s_state_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_state_label, "Idle");

    s_time_label = lv_label_create(profile_card);
    lv_obj_set_style_text_color(s_time_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_time_label, "--");

    s_progress_bar = lv_bar_create(profile_card);
    lv_obj_set_width(s_progress_bar, lv_pct(100));
    lv_obj_set_height(s_progress_bar, 16);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, UI_THEME_ACCENT_3, LV_PART_INDICATOR);

    /* Profile picker (TODO.md 10.3) -- lives in the profile/run-state card,
     * right above Start/Stop, since it's what Start now consults first
     * (start_btn_cb()'s comment). */
    lv_obj_t *picker_label = lv_label_create(profile_card);
    lv_obj_set_style_text_color(picker_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(picker_label, "Profile to start:");
    build_profile_picker(profile_card);

    /* Desired-vs-actual temperature graph (TODO.md 10.3, previously a
     * labeled placeholder -- see build_graph_card()'s comment for why this
     * is one combined chart rather than one per zone). */
    build_graph_card(content);

    /* Start/Stop. */
    lv_obj_t *action_row = lv_obj_create(content);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(action_row, UI_THEME_PADDING_PX, 0);
    s_start_btn = build_button(action_row, "Start", UI_THEME_ACCENT_4, start_btn_cb);
    s_stop_btn = build_button(action_row, "Stop", UI_THEME_ACCENT_5, stop_btn_cb);

    /* Nav. */
    lv_obj_t *nav_row = lv_obj_create(content);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(nav_row, UI_THEME_PADDING_PX, 0);
    build_button(nav_row, "Configuration", UI_THEME_COLOR_CARD, config_nav_cb);
    build_button(nav_row, "Temperature", UI_THEME_COLOR_CARD, temperature_nav_cb);

    /* Pages are never torn down (kiln_ui.h's header comment), so a timer
     * created once here and never deleted matches that lifetime -- it keeps
     * refreshing this page's widgets even while another page is shown, which
     * is cheap (one dashboard_get_status()/profile_executor_get_status()
     * call a second) and means the numbers are already current the instant
     * kiln_ui_show("home") is called again. */
    lv_timer_create(refresh_cb, UI_PAGE_HOME_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
