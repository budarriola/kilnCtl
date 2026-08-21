#include "ui_page_home.h"

#include <stdio.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "ui_page_config.h"
#include "profile_executor.h"
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
// 2026-08-18, NO-SCROLL REWRITE (hard requirement: LCD pages must never
// require scrolling -- ILI9488.h's DISPLAY_WIDTH/HEIGHT plus Kconfig's
// default startup rotation (1) put this codebase's actual runtime canvas at
// 480x320 landscape, matching ui_theme.h's own "budgeted against the
// 480x320 landscape panel" comment; the physical panel is natively
// 320x480, but nothing on this page renders in that orientation with the
// shipped default). Everything that previously lived in one long
// scrollable flex column has been re-budgeted against a hard content height
// of roughly 320 - (2*UI_THEME_PADDING_PX outer pad) - UI_THEME_STATUS_BAR_HEIGHT_PX
// - (bar-to-content gap) =~ 264px, computed against real UI_THEME_PADDING_PX /
// UI_THEME_STATUS_BAR_HEIGHT_PX constants and LV_FONT_DEFAULT's real
// montserrat_14 line height (~20px with LVGL's default line spacing), not
// guessed. No real hardware was available to visually confirm the result --
// treat the numbers below as "computed to leave a real double-digit-pixel
// margin against a real budget," not as pixel-verified.
//
// What stayed on THIS page (the file's own prior framing: "kiln-process data
// the operator watches while firing"):
//   - Zone rows: name, temperature, heater on/off -- the core "glance while
//     firing" case.
//   - A single-line run-state summary (profile name + state) plus a
//     time/progress line and a slim progress bar -- trimmed from the
//     previous 4-widget profile card to fit the budget.
//   - Start/Stop/Menu action row (Menu replaces the old separate
//     Configuration+Temperature nav row -- see below).
//
// What MOVED OFF this page, each reachable via kiln_ui_show() same as any
// other secondary page (TODO.md 10.1's page-manager pattern), because they
// could not fit the 264px budget alongside the zones/status/action content
// above without silently re-introducing scrolling:
//   - AP-join QR card (TODO.md 10.9) -- REMOVED outright, not moved.
//     ui_page_network.c already shows the identical QR
//     (WIFI:T:WPA;S:...;P:...;; payload, same gating) on its own AP-mode
//     section; this page's copy was a duplicate per 10.9's own audit note,
//     not unique content, so deleting it (rather than relocating it to a
//     third place) is the honest fix.
//   - "Safety Processor" card (ROADMAP.md M6, added 2026-08-18) -- moved to
//     the new ui_page_safety.c/.h, reachable from ui_page_config.c's
//     "Safety Processor" nav item.
//   - Desired-vs-actual temperature chart (TODO.md 10.3) -- moved to the new
//     ui_page_history.c/.h, reachable from ui_page_config.c's "Temperature
//     History" nav item.
//   - Profile picker dropdown -- REMOVED outright, not moved. Start now
//     always uses the same fallback chain start_btn_cb() already had for
//     "picker untouched": current non-idle profile, else the last boot
//     record. An operator who wants to explicitly pick a *different* saved
//     profile before starting still has to use the web dashboard's picker
//     (main_page.html) -- a real, documented capability loss versus the
//     picker this page briefly had, traded for the hard no-scroll
//     requirement. See TODO.md 10.3's status note for this trade-off.
//
// Content-container scrolling is explicitly disabled
// (LV_OBJ_FLAG_SCROLLABLE cleared on both `scr` and `content` in
// ui_page_home_build()) now that the content is sized to fit -- if a future
// change re-overflows this page, LVGL will clip the overflow instead of
// silently turning scrollable again, which is a visible bug report waiting
// to happen rather than a silent regression.

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

static lv_obj_t *s_state_label;   /* "<profile> -- <state>" single line */
static lv_obj_t *s_time_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_stop_btn;

/* WiFi/IP/mDNS status readout, in the status bar. Text comes from
 * wifi_status_ui_get_text() (wifi_status_ui.c) -- see that module's header
 * comment for the underlying getters and the TODO.md 10.1a shared-backend
 * rule. */
static lv_obj_t *s_status_label;

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
     *
     * No picker on this page anymore (see this file's header comment for
     * why) -- always the fallback chain: whatever's currently known this
     * boot (non-idle profile_id), else the last boot record. An operator
     * who wants a *different* profile than either of those has to use the
     * web dashboard's picker. */
    bool have_id = false;
    uint8_t id = 0;

    profile_exec_status_t st;
    profile_executor_get_status(&st);
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
        ESP_LOGW(TAG, "Start pressed with no known profile id -- nothing has run this boot "
                      "and no picker on this page (TODO.md 10.3's no-scroll rewrite)");
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

static void menu_nav_cb(lv_event_t *e)
{
    (void)e;
    /* Single "Menu" button replaces the old separate Configuration and
     * Temperature nav buttons -- see this file's header comment on the
     * budget this page is fit to. Temperature (manual relay control) is now
     * reached via ui_page_config.c's hub, one tap further than before.
     *
     * Rewind the hub first: it is built once and keeps its paging position,
     * so without this, Menu drops you on whichever hub page you were last on.
     * Back from a sub-page deliberately still returns to the page you left
     * from -- only Menu means "take me to the top of the menu". */
    ui_page_config_reset_to_first_page();
    kiln_ui_show("config");
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

/* Compact zone row -- pad_all trimmed to UI_THEME_PADDING_PX/4 (2px, vs. the
 * standard 8px) specifically to fit up to MAX31856_CHANNEL_COUNT (3) of
 * these plus the status card and action row inside this page's ~264px
 * content budget (see this file's header comment for that number's
 * derivation). Everything else about the row (accent border, name/temp/heat
 * labels) is unchanged from the pre-rewrite version. */
static void build_zone_row(lv_obj_t *parent, uint8_t zone_index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_set_style_pad_left(row, UI_THEME_PADDING_PX, 0); /* room for the accent border */
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, zone_accent(zone_index), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_color(name, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char name_buf[24];
    /* zones_config_get_name() -- "Zone N" is only the fallback for a zone
     * that returns false (out of range, should never trip here) or a real
     * but empty name (a configured zone the operator has never named). */
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

    /* Same plain-C getters dashboard_http.c's GET /api/status and
     * GET /api/profile_exec handlers call -- TODO.md 10.1a's shared-backend
     * rule, not a reimplementation. */
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

    /* Single-line "<profile> -- <state>" summary -- replaces the previous
     * two separate labels (profile name, state) to save a text line's worth
     * of height (see this file's header comment on the budget). */
    char state_buf[64];
    if (st.state == PROFILE_EXEC_IDLE) {
        snprintf(state_buf, sizeof(state_buf), "No profile running");
    } else {
        snprintf(state_buf, sizeof(state_buf), "%s -- %s", st.profile_name[0] ? st.profile_name : "(unnamed)",
                 exec_state_label(st.state));
    }
    lv_label_set_text(s_state_label, state_buf);

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
         * -Werror=format-truncation caught this statically. */
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
}

lv_obj_t *ui_page_home_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    /* Hard no-scroll requirement -- see this file's header comment. Content
     * below is sized to fit; if it ever overflows again this clips instead
     * of silently becoming scrollable. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar (TODO.md 10.2's persistent top bar -- just a title here,
     * global chrome shared across pages is still explicitly deferred per
     * kiln_ui.h's header comment). */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "kilnCtl");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* WiFi/IP/mDNS status readout -- right side of the same status bar. */
    s_status_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_status_label, "WiFi: --");
    lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Content area -- deliberately NOT scrollable (see this file's header
     * comment for the ~264px budget this is sized against). */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

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

    /* Compact run-state card -- one summary line, one time/progress line, a
     * slim progress bar. Trimmed from the previous 4-widget card (separate
     * profile-name and state labels, a full-height picker) specifically to
     * fit this page's budget -- see this file's header comment. */
    lv_obj_t *state_card = lv_obj_create(content);
    lv_obj_set_width(state_card, lv_pct(100));
    lv_obj_set_height(state_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(state_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(state_card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(state_card, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(state_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(state_card, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(state_card, LV_OBJ_FLAG_SCROLLABLE);

    s_state_label = lv_label_create(state_card);
    lv_obj_set_style_text_color(s_state_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_state_label, "No profile running");

    s_time_label = lv_label_create(state_card);
    lv_obj_set_style_text_color(s_time_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_time_label, "--");

    s_progress_bar = lv_bar_create(state_card);
    lv_obj_set_width(s_progress_bar, lv_pct(100));
    lv_obj_set_height(s_progress_bar, 10);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, UI_THEME_ACCENT_3, LV_PART_INDICATOR);

    /* Start/Stop/Menu -- one row (was previously two: Start/Stop, then a
     * separate Configuration/Temperature nav row). Menu replaces both old
     * nav buttons; Temperature (manual relay control) is now one tap
     * further away, via ui_page_config.c's hub -- see menu_nav_cb(). */
    lv_obj_t *action_row = lv_obj_create(content);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(action_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);
    s_start_btn = build_button(action_row, "Start", UI_THEME_ACCENT_4, start_btn_cb);
    s_stop_btn = build_button(action_row, "Stop", UI_THEME_ACCENT_5, stop_btn_cb);
    build_button(action_row, "Menu", UI_THEME_COLOR_CARD, menu_nav_cb);

    /* Pages are never torn down (kiln_ui.h's header comment), so a timer
     * created once here and never deleted matches that lifetime. */
    lv_timer_create(refresh_cb, UI_PAGE_HOME_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
