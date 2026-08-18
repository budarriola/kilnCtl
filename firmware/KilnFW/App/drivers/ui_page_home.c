#include "ui_page_home.h"

#include <stdio.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "profile_executor.h"
#include "run_state.h"
#include "ui_theme.h"
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
// Deliberately NOT built this pass (see this file's bottom section and
// TODO.md 10.3's status note): the desired-vs-actual temperature graph --
// left as a labeled placeholder card. It needs a real LVGL chart/canvas
// approach against profile_executor_get_history() (the same ring buffer
// GET /api/history.csv streams) and is large enough to be its own pass, not
// something to half-build here.
//
// Also honest about a real gap: there is no profile-picker widget on this
// page (that belongs on a real Configuration/profile-select page, still a
// stub -- ui_page_config.c). The Start button therefore starts whichever
// profile_id is already known -- the currently DONE/FAULTED run's id if the
// executor has one this boot, else the last boot's run_state record -- and
// simply logs a warning and does nothing if neither exists. This is a real
// limitation, not a placeholder pretending to work; TODO.md 10.3 notes it.

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

static void start_btn_cb(lv_event_t *e)
{
    (void)e;

    /* Same action function dashboard_http.c's POST /api/profile_exec/start
     * handler calls (profile_exec_start_post_handler()) -- TODO.md 10.1a.
     * The only thing specific to this page is how it picks WHICH profile_id
     * to pass, since there is no profile-picker widget here yet -- see this
     * file's header comment. */
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    bool have_id = false;
    uint8_t id = 0;
    if (st.state != PROFILE_EXEC_IDLE) {
        id = st.profile_id;
        have_id = true;
    }
    if (!have_id) {
        run_state_record_t rec;
        if (run_state_get_boot_record(&rec)) {
            id = rec.profile_id;
            have_id = true;
        }
    }
    if (!have_id) {
        ESP_LOGW(TAG, "Start pressed with no known profile id -- no profile-picker on this page yet "
                      "(TODO.md 10.3), and nothing has run this boot");
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
    char name_buf[16];
    /* No zone-name getter is exposed by zones_http.h today (only config
     * storage internal to zones_http.c has one) -- "Zone N" until that gap
     * is closed. */
    snprintf(name_buf, sizeof(name_buf), "Zone %u", (unsigned)zone_index);
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

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

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
        char buf[48];
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
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSPARENT, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "kilnCtl");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* Scrollable content area -- 480x320 is tight for zones + profile/time +
     * graph placeholder + buttons all at once, and this page's exact widget
     * sizing has never been checked against the real panel (no hardware this
     * pass, same caveat as ui_theme.h). Scrolling is the safe fallback
     * rather than guessing pixel-perfect fixed heights. */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSPARENT, 0);
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

    /* Graph placeholder -- explicitly out of scope this pass (TODO.md
     * 10.3's status note). Needs a real LVGL chart/canvas against
     * profile_executor_get_history(), not built here. */
    lv_obj_t *graph_card = lv_obj_create(content);
    lv_obj_set_width(graph_card, lv_pct(100));
    lv_obj_set_height(graph_card, 60);
    lv_obj_set_style_bg_color(graph_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(graph_card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_t *graph_label = lv_label_create(graph_card);
    lv_obj_set_style_text_color(graph_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(graph_label, "Temperature graph -- not built yet (TODO.md 10.3)");
    lv_obj_set_style_text_align(graph_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(graph_label);

    /* Start/Stop. */
    lv_obj_t *action_row = lv_obj_create(content);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSPARENT, 0);
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
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSPARENT, 0);
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
