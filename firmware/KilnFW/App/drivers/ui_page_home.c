#include "ui_page_home.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "ui_confirm.h"
#include "ui_page_config.h"
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
//   - A single merged Start/Stop action button, colour/label following run
//     state (2026-08-20: was Start+Stop+Menu; Menu moved to a gear icon in
//     the status bar and Start/Stop merged into one button -- see
//     fire_btn_cb() and the status-bar build in ui_page_home_build()).
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
static lv_obj_t *s_fire_btn;      /* merged Start/Stop button */
static lv_obj_t *s_fire_btn_label;
static lv_obj_t *s_gear_btn;       /* Menu, now a gear icon top-right of the status bar */

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

/* Shared by do_start() and the confirmation dialog builder below -- both need
 * the exact same fallback chain (whatever's currently known this boot
 * (non-idle profile_id), else the last boot record), and the dialog has to
 * name the SAME profile the button is actually about to start, not a second
 * guess at it. Returns false (out_id untouched) if neither source has one. */
static bool resolve_start_profile_id(uint8_t *out_id)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state != PROFILE_EXEC_IDLE) {
        *out_id = st.profile_id;
        return true;
    }
    run_state_record_t rec;
    if (run_state_get_boot_record(&rec)) {
        *out_id = rec.profile_id;
        return true;
    }
    return false;
}

static void do_start(void)
{
    /* Same action function dashboard_http.c's POST /api/profile_exec/start
     * handler calls (profile_exec_start_post_handler()) -- TODO.md 10.1a.
     *
     * No picker on this page anymore (see this file's header comment for
     * why) -- always the fallback chain: whatever's currently known this
     * boot (non-idle profile_id), else the last boot record. An operator
     * who wants a *different* profile than either of those has to use the
     * web dashboard's picker. */
    uint8_t id = 0;
    if (!resolve_start_profile_id(&id)) {
        ESP_LOGW(TAG, "Start pressed with no known profile id -- nothing has run this boot "
                      "and no picker on this page (TODO.md 10.3's no-scroll rewrite)");
        return;
    }

    char err_msg[64] = "";
    if (!profile_executor_run(id, err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "profile_executor_run(%u) refused: %s", id, err_msg);
    }
}

static void do_stop(void)
{
    /* Same action function dashboard_http.c's POST /api/profile_exec/stop
     * handler calls -- TODO.md 10.1a. profile_executor_halt() itself is the
     * only gate on a stop today -- no confirmation dialog exists anywhere in
     * this call path (see this file's header/report note: flagged back to
     * the requester rather than silently added here). */
    profile_executor_halt();
}

/* ---- Start/Stop confirmation overlay --------------------------------
 *
 * Both actions were firing immediately with no confirmation anywhere in this
 * call path (do_start()/do_stop()'s own header comments used to flag this
 * back to the requester rather than silently adding one). Starting energises
 * heaters for hours; stopping mid-firing aborts a load. Both now go through
 * a modal confirm/cancel step first, via the shared ui_confirm.c helper
 * (factored out of what used to be a hand-rolled lv_msgbox pair here, so the
 * new Profiles-hub detail page's START action -- see ui_page_profile_detail.c
 * -- reuses the exact same dialog instead of a second copy-pasted
 * implementation). See ui_confirm.h for the FLEX TRAP / cancel-safe-by-
 * default rationale that used to live in this comment. */

static void confirm_start_yes_cb(void *user_data)
{
    (void)user_data;
    do_start();
}

static void confirm_stop_yes_cb(void *user_data)
{
    (void)user_data;
    do_stop();
}

static void show_start_confirm(void)
{
    uint8_t id = 0;
    bool have_id = resolve_start_profile_id(&id);

    profile_t prof;
    bool have_prof = have_id && profiles_http_get(id, &prof);

    char body[256];
    if (have_prof) {
        /* Zones this profile drives -- cheap here (profiles_http_get() is a
         * plain NVS-backed struct copy, same call profile_executor.c itself
         * uses to run the profile, not a second read path), unlike trying to
         * derive it from profile_exec_status_t, which only carries a
         * meaningful zone_mask once the run has actually started. */
        char zones_buf[96];
        size_t zlen = 0;
        zones_buf[0] = '\0';
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && zlen < sizeof(zones_buf) - 1; zi++) {
            if (!(prof.zone_mask & (1u << zi))) {
                continue;
            }
            char name[16];
            const char *zname = (zones_config_get_name(zi, name, sizeof(name)) && name[0]) ? name : NULL;
            char piece[24];
            if (zname) {
                snprintf(piece, sizeof(piece), "%s%s", zlen ? ", " : "", zname);
            } else {
                snprintf(piece, sizeof(piece), "%sZone %u", zlen ? ", " : "", (unsigned)zi);
            }
            size_t piece_len = strlen(piece);
            if (zlen + piece_len < sizeof(zones_buf)) {
                memcpy(zones_buf + zlen, piece, piece_len + 1);
                zlen += piece_len;
            }
        }
        snprintf(body, sizeof(body), "Start \"%s\" now? This will energise %s for the duration of the "
                                      "firing, which can be hours.",
                 prof.name, zones_buf[0] ? zones_buf : "no zones");
    } else if (have_id) {
        /* Have an id but profiles_http_get() failed (slot no longer stored,
         * e.g. deleted between boot and now) -- name what we can rather than
         * a blank, and say plainly the rest could not be read. */
        snprintf(body, sizeof(body), "Start profile id %u now? Its saved details could not be read, "
                                      "but starting will energise heaters for the duration of the "
                                      "firing, which can be hours.",
                 (unsigned)id);
    } else {
        /* No non-idle profile this boot and no boot record either -- the
         * honest answer is "nothing to name," not a blank dialog. */
        snprintf(body, sizeof(body), "No profile can be identified to start (nothing has run yet this "
                                      "boot). Use the web dashboard's profile picker to choose one.");
    }

    ui_confirm_params_t params = {
        .title = "Confirm Start",
        .body = body,
        .confirm_label = "Start",
        .confirm_color = UI_THEME_ACCENT_4, /* start-green, matches the fire button */
        .on_confirm = confirm_start_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

static void show_stop_confirm(void)
{
    ui_confirm_params_t params = {
        .title = "Confirm Stop",
        .body = "Stop this firing now? This aborts the run in progress -- it cannot "
                "be resumed, and the load will not finish firing.",
        .confirm_label = "Stop",
        .confirm_color = UI_THEME_ACCENT_5, /* stop-red, matches the fire button */
        .on_confirm = confirm_stop_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

/* Merged Start/Stop button (single user-visible request: "the start stop
 * button should be one button on the lcd"). Idle/Done/Faulted -> "Start" +
 * confirm -> do_start(); Running/Paused -> "Stop" + confirm -> do_stop().
 * One callback reads current state at click time rather than two callbacks
 * each assuming a fixed action, so a state change between refresh_cb() ticks
 * and the actual tap can never fire the stale action -- the same reasoning
 * now also decides which of the two confirmation dialogs to show. */
static void fire_btn_cb(lv_event_t *e)
{
    (void)e;
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        show_stop_confirm();
    } else {
        show_start_confirm();
    }
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

/* out_label, if non-NULL, receives the button's label widget so a caller can
 * change its text/color later (the merged fire button's state-driven text --
 * see fire_btn_cb()/refresh_cb()). */
static lv_obj_t *build_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb,
                               lv_obj_t **out_label)
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
    if (out_label) {
        *out_label = label;
    }

    /* TODO.md 10.4's touch hit-area helper (ui_theme.c) -- this is a sparse
     * button row, not a dense grid, so compact_layout=false. Forces an
     * immediate layout pass first: ui_theme_apply_touch_area() reads back
     * lv_obj_get_width/height(), which flex_grow leaves unresolved until
     * layout actually runs. */
    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);

    return btn;
}

/* Small icon button (the gear) -- sized to its content rather than
 * flex_grow'd across a row, since it lives in the status bar next to the
 * WiFi label, not in the action row. compact_layout=false when calling
 * ui_theme_apply_touch_area(): this is a lone icon, not a dense grid cell, so
 * it gets the generous "reach UI_THEME_MIN_TOUCH_TARGET_PX effective size"
 * extension, same as build_button()'s row buttons -- see that helper's
 * doc comment in ui_theme.h. */
static lv_obj_t *build_icon_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(btn, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);

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
        /* ch->stale is the shared user-facing rule (older than
         * KILN_TEMP_STALE_AGE_MS), not the driver's per-poll flag -- see
         * dashboard_http.h. Showing a number that has not been refreshed in
         * over ten seconds as if it were live is how a kiln gets watched
         * against a temperature that stopped moving; "--" is the honest
         * answer, and it matches what the web page and the PC tools show for
         * the same reading. */
        if (ch && ch->valid && !ch->stale) {
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

    /* Merged fire button -- label and color follow the same st.state this
     * function already polled above. Running/Paused reads "Stop" in the
     * danger accent; everything else (Idle/Done/Faulted) reads "Start" in
     * the start-ish accent. */
    if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        lv_label_set_text(s_fire_btn_label, "Stop");
        lv_obj_set_style_bg_color(s_fire_btn, UI_THEME_ACCENT_5, 0);
    } else {
        lv_label_set_text(s_fire_btn_label, "Start");
        lv_obj_set_style_bg_color(s_fire_btn, UI_THEME_ACCENT_4, 0);
    }

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

    /* Gear (Menu) touch-target proxy -----------------------------------
     *
     * MEASURED ON HARDWARE (2026-08-20, injected touches + kiln_ui.c's
     * tap-target dump -- facts, not guesses): with the gear as an ordinary
     * child of the 32px status bar, its logged clickable rect was
     * (450,12)-(471,35) -- a 21x23px box, centre (460,23). Tapping 15px to
     * the LEFT of that box (still inside the bar vertically) hit, proving
     * ui_theme_apply_touch_area()'s horizontal ext_click_area extension was
     * being applied. Tapping 30px below/left of it -- outside the bar's
     * own 32px vertical span -- did NOT hit, even though the button's own
     * ext_click_area should have reached that far.
     *
     * Root cause: LVGL's hit test descends the widget tree and can only
     * recurse into a child if the *parent's* own (possibly extended) area
     * already contains the touch point. A child's ext_click_area can never
     * reach past a parent that doesn't itself cover that point. The status
     * bar is a fixed 32px tall box with no click extension of its own, so
     * any child of it -- gear included -- was hard-capped at 32px of
     * vertical reach no matter how generous its own ext_click_area was.
     *
     * Fix: this invisible object (s_gear_hit_area) is parented directly to
     * `scr` (the page root), the same level as `bar` and `content`, so it
     * is never clipped by the bar's height. It is moved to the foreground
     * once `content` exists below (see the lv_obj_move_foreground() call
     * after `content` is built) so it wins the hit test over both the bar
     * and the content beneath it wherever they overlap.
     *
     * Sized 80 x 36px: 36 = UI_THEME_STATUS_BAR_HEIGHT_PX (32) + the real
     * bar-to-content pad_gap (UI_THEME_PADDING_PX/2 = 4px, see the
     * lv_obj_set_style_pad_gap(scr, ...) call above and the matching
     * derivation in this file's build_zone_row()-area comments). That gap
     * is the hardware-verified miss point (430,45) sits just past --
     * absolute y=44 is where `content` begins, so this box stops exactly
     * there and never reaches into the first content/zone row. Width 80 is
     * generous sideways since nothing else in the status bar is clickable
     * (the WiFi label is plain text), so widening left of the visual gear
     * cannot steal a tap meant for another control.
     *
     * Transparent/borderless: purely a hit-target, not drawn. The real
     * gear glyph is s_gear_btn, an ordinary child of this proxy positioned
     * to land pixel-identical to where it used to sit as a bar child, so
     * this is invisible on screen -- only the effective touch rectangle
     * changed.
     *
     * DO NOT "tidy" the gear back into being a plain child of `bar` --
     * that silently reintroduces the 21x23px touch target measured above.
     *
     * FLEX TRAP (hit on hardware once already, see history): `scr` is a
     * flex-column container. A plain child added to it is placed IN THE
     * FLOW as the next column item -- it does NOT keep whatever
     * lv_obj_align() position you asked for, and its height is subtracted
     * from the column's available space same as any other row. The first
     * version of this fix skipped LV_OBJ_FLAG_FLOATING and the proxy was
     * measured on hardware sitting at the BOTTOM of the screen (pushed
     * there as the 3rd flow item after `bar` and `content`), while
     * `content` shrank by the proxy's own height (267px -> 227px),
     * breaking the no-scroll budget. LV_OBJ_FLAG_FLOATING (confirmed
     * present in this vendored LVGL as of writing, components/lvgl/src/
     * core/lv_obj.h) is what tells flex to skip the object entirely: it
     * takes no space in the column and is positioned purely by its own
     * align/coords, exactly like a normal absolutely-positioned overlay.
     * It MUST be set before anything reads back this object's layout
     * position/size (ext_click_area math, alignment of the visual gear
     * glyph below, etc.) -- set it immediately after creation, first.
     */
    /* Named so the status-label width cap below (which must never let text
     * reach under this proxy) is derived from the same number instead of
     * repeating the literal 80 and silently drifting from it later. */
    const int32_t gear_hit_area_w = 80;
    lv_obj_t *gear_hit_area = lv_obj_create(scr);
    lv_obj_add_flag(gear_hit_area, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(gear_hit_area, gear_hit_area_w, UI_THEME_STATUS_BAR_HEIGHT_PX + (UI_THEME_PADDING_PX / 2));
    lv_obj_set_style_bg_opa(gear_hit_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gear_hit_area, 0, 0);
    lv_obj_set_style_pad_all(gear_hit_area, 0, 0);
    lv_obj_remove_flag(gear_hit_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gear_hit_area, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(gear_hit_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear_hit_area, menu_nav_cb, LV_EVENT_CLICKED, NULL);

    /* Visual gear glyph -- same look as before (build_icon_button(), same
     * symbol, same click handler), just parented to the hit-area proxy
     * above instead of to `bar`. Aligned so its on-screen position matches
     * the old bar-relative RIGHT_MID placement exactly: the proxy's top
     * edge coincides with the bar's top edge (both offset 0 from scr's own
     * padding), so re-deriving "vertically centred in a
     * UI_THEME_STATUS_BAR_HEIGHT_PX-tall band from that shared top edge"
     * reproduces the identical pixel position. */
    s_gear_btn = build_icon_button(gear_hit_area, LV_SYMBOL_SETTINGS, menu_nav_cb);
    lv_obj_update_layout(s_gear_btn);
    lv_obj_align(s_gear_btn, LV_ALIGN_TOP_RIGHT, 0,
                 (UI_THEME_STATUS_BAR_HEIGHT_PX - lv_obj_get_height(s_gear_btn)) / 2);

    /* WiFi/IP/mDNS status readout -- right side of the status bar, just
     * left of the gear. Width capped so a long status string can't grow
     * under/behind the gear's (invisible, extended) hit area; the label
     * itself still just shows whatever wifi_status_ui_get_text() returns.
     * align_to() works across the gear's new parent (gear_hit_area instead
     * of bar) the same as before -- it resolves absolute coordinates, not
     * a shared-parent relationship.
     *
     * WIDTH MUST BE SET EXPLICITLY, NOT INFERRED FROM align_to() --
     * confirmed on hardware (2026-08-20 tap-target dump) after the user
     * reported the WiFi text overlapping the gear once connected (long
     * strings like "WiFi: 192.168.1.156 (kilnctl.local) -- Signal: -45
     * dBm"). Measured facts: s_status_label's clickable/paint rect was
     * (8,8)-(471,39) -- the FULL 463px bar width -- while the gear glyph
     * sat at (450,12)-(471,35) and its hit proxy at (392,8)-(471,43). The
     * label's box was underneath both. lv_obj_align_to() only sets a
     * POSITION (an anchor point); it never constrains WIDTH, and this
     * label's box was still sized to the full width of `bar` (its parent),
     * so the fixed anchor point did nothing to stop the box -- and
     * therefore the rendered text -- from running under the gear. Fix:
     * cap the label's own width so its box can never reach the proxy,
     * independent of parent width, then use LV_LABEL_LONG_DOT so any
     * string that still doesn't fit ellipsises ("...") instead of
     * overflowing into the gear. LV_LABEL_LONG_SCROLL_CIRCULAR was
     * considered and rejected: this is an always-on kiln panel, and a
     * perpetually scrolling label is a needless distraction and redraw
     * cost. Text stays left-aligned (the label's default) so short
     * strings look exactly as they did before.
     *
     * Width derivation (self-correcting if bar width or the proxy's width
     * ever change -- no hardcoded 384/392 here, only the constants those
     * numbers came from): cap = bar_width - gear_hit_area_w - gap, where
     * gap is the same UI_THEME_PADDING_PX/2 offset already used below to
     * keep the box off the proxy. With the measured 463px bar and the 80px
     * proxy this resolves to ~379px, right edge ~x=387 -- left of the
     * proxy's x=392 with a visible gap, gear glyph unaffected at
     * (450,12)-(471,35). */
    lv_obj_update_layout(bar);
    int32_t bar_w = lv_obj_get_width(bar);
    int32_t status_label_max_w = bar_w - gear_hit_area_w - (UI_THEME_PADDING_PX / 2);
    /* Guard the subtraction. lv_obj_update_layout() above normally resolves
     * the bar's lv_pct(100) against the screen, but this page is BUILT
     * DETACHED (kiln_ui.c builds a page before it is ever shown), and a
     * width read before layout resolves is 0 -- which would make this
     * subtraction negative and the label either invisible or garbage. If
     * that ever happens, fall back to the full bar width: a label that
     * overlaps the gear is a cosmetic bug, a label that vanishes is a
     * functional one, and the log line says which case this boot took. */
    if (status_label_max_w <= 0) {
        ESP_LOGW(TAG, "status label width fallback: bar_w=%ld resolved too small for the gear "
                      "reservation (%ld) -- label may overlap the gear this boot",
                 (long)bar_w, (long)gear_hit_area_w);
        status_label_max_w = (bar_w > 0) ? bar_w : LV_SIZE_CONTENT;
    } else {
        ESP_LOGI(TAG, "status label width %ld of bar %ld (gear reserves %ld)",
                 (long)status_label_max_w, (long)bar_w, (long)gear_hit_area_w);
    }
    s_status_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_width(s_status_label, status_label_max_w);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_status_label, "WiFi: --");
    /* Anchor to gear_hit_area (the proxy), not s_gear_btn (the visual
     * glyph) -- the proxy is the wider box and the one the label must
     * actually clear. */
    lv_obj_align_to(s_status_label, gear_hit_area, LV_ALIGN_OUT_LEFT_MID, -(UI_THEME_PADDING_PX / 2), 0);

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

    /* `content` is created after gear_hit_area, so without this it would
     * sit above the proxy in z-order and win taps in the overlap region
     * (the padding gap between the bar and the first zone row). Force the
     * proxy back to the top so it keeps winning the hit test there --
     * see the proxy's own comment above for why that matters. */
    lv_obj_move_foreground(gear_hit_area);

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

    /* Single merged Start/Stop button -- one user-visible request ("the
     * start stop button should be one button on the lcd"). Menu moved off
     * this row entirely into the status bar as a gear (see the status-bar
     * build above and menu_nav_cb()) per the other request, so this row is
     * now just the one button. build_button() still grows it across the
     * row's width via flex_grow(1).
     *
     * Action-row height arithmetic (2026-08-20, same style as
     * ui_page_config.c's hub-page comment), against this page's ~264px
     * content budget (see this file's header comment):
     *
     *     zone rows (up to 3, compact) ......... variable, unchanged
     *     state card (2 lines + progress bar) .. unchanged
     *     gap ................................... UI_THEME_PADDING_PX/2 = 4px
     *     action row: 1 button @ 72px .......... 72px
     *
     * Previously this row held 3 buttons side by side but was still only
     * 72px tall (build_button() fixes height, flex_grow only affects width) --
     * removing Menu frees width (each remaining button gets more of the row),
     * not height, exactly as expected: the row's own height contribution to
     * the vertical budget is unchanged at 72px, and the page's total vertical
     * budget is therefore unchanged or better (better because the gear also
     * moved a former content-row width constraint out, not because the
     * action row got shorter). */
    lv_obj_t *action_row = lv_obj_create(content);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(action_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);
    s_fire_btn = build_button(action_row, "Start", UI_THEME_ACCENT_4, fire_btn_cb, &s_fire_btn_label);

    /* Pages are never torn down (kiln_ui.h's header comment), so a timer
     * created once here and never deleted matches that lifetime. */
    lv_timer_create(refresh_cb, UI_PAGE_HOME_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
