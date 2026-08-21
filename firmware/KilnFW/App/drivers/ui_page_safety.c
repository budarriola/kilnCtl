#include "ui_page_safety.h"

#include <stdio.h>

#include "dashboard_http.h"
#include "ui_theme.h"
#include "ui_topbar.h"

// See ui_page_safety.h for why this page exists (moved off ui_page_home.c
// in the 2026-08-18 no-scroll rewrite). Contents/behavior are otherwise
// unchanged from the card ui_page_home.c used to build: same
// dashboard_get_status() read, same null-tolerant "---" convention for a
// field the safety link hasn't got real data for (same convention
// ui_page_board_health.c's per-channel rows use), same
// TODO.md 9.0 "which link version is older" line.
//
// ROADMAP.md M5 (2026-08-19): gained a fifth row, "Last trip", from
// SAFETY_CMD_TRIP_EVENT (Frame D) -- LINK_PROTOCOL.md sec 6's "the answer to
// 'why did the kiln stop'". This is the only new row that fit the page's
// ~264px no-scroll budget; SAFETY_CMD_DIAG (Frame B)'s warn/trip masks and
// context-health counters are cached in safety_link_status_t and surfaced
// over GET /api/status (dashboard_http.c) but deliberately left off this
// page -- a diagnostics page (TODO.md 10.7's ui_page_board_health.c
// precedent) is the better home for that if it's ever added, not a fifth
// and sixth row squeezed in here.

static const char *TAG __attribute__((unused)) = "ui_page_safety";

#define UI_PAGE_SAFETY_REFRESH_MS 1000

static lv_obj_t *s_safety_temp_label;
static lv_obj_t *s_enclosure_temp_label;
static lv_obj_t *s_safety_power_label;
static lv_obj_t *s_link_version_label;
static lv_obj_t *s_trip_label;

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    dashboard_status_t ds;
    dashboard_get_status(&ds);

    if (ds.safety_temp_valid) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Safety temp: %.1f C", (double)ds.safety_temp_c);
        lv_label_set_text(s_safety_temp_label, buf);
    } else {
        lv_label_set_text(s_safety_temp_label, "Safety temp: ---");
    }
    if (ds.enclosure_temp_valid) {
        char buf[28];
        snprintf(buf, sizeof(buf), "Enclosure temp: %.1f C", (double)ds.enclosure_temp_c);
        lv_label_set_text(s_enclosure_temp_label, buf);
    } else {
        lv_label_set_text(s_enclosure_temp_label, "Enclosure temp: ---");
    }
    if (ds.power_valid) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Power: %.0f W", (double)ds.power_w);
        lv_label_set_text(s_safety_power_label, buf);
    } else {
        lv_label_set_text(s_safety_power_label, "Power: ---");
    }

    if (!ds.link_version_known) {
        lv_label_set_text(s_link_version_label, "Link version: ---");
    } else if (ds.link_version_compatible) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Link version: ESP %u / Pico %u (OK)",
                 (unsigned)ds.self_protocol_version, (unsigned)ds.peer_protocol_version);
        lv_label_set_text(s_link_version_label, buf);
    } else {
        char buf[112];
        const char *older = (ds.peer_protocol_version < ds.self_protocol_version) ? "Pico"
                            : (ds.peer_protocol_version > ds.self_protocol_version) ? "ESP"
                                                                                     : "neither";
        snprintf(buf, sizeof(buf),
                 "Link version: ESP %u / Pico %u -- INCOMPATIBLE, %s is older. Update ESP first.",
                 (unsigned)ds.self_protocol_version, (unsigned)ds.peer_protocol_version, older);
        lv_label_set_text(s_link_version_label, buf);
    }

    /* ROADMAP.md M5: SAFETY_CMD_TRIP_EVENT (Frame D) -- "the answer to 'why
     * did the kiln stop'" (LINK_PROTOCOL.md sec 6), so it earns the one new
     * row this page's ~264px budget has room for (see this file's header
     * comment and build_stat_label()'s doc comment for that budget).
     * trip_reason is a raw SAFETY_TRIP_* value -- this firmware has no
     * guard-name table to decode it against, same reasoning
     * dashboard_http.c's JSON leaves it numeric. Age is reported in whole
     * seconds; sub-second precision isn't useful once a trip is more than a
     * moment old. */
    if (!ds.trip_event_ever_received) {
        lv_label_set_text(s_trip_label, "Last trip: ---");
    } else {
        char buf[56];
        snprintf(buf, sizeof(buf), "Last trip: reason 0x%02X, %lus ago",
                 (unsigned)ds.trip_reason, (unsigned long)(ds.trip_event_age_ms / 1000u));
        lv_label_set_text(s_trip_label, buf);
    }
}

/* Compact stat row -- pad_all trimmed to UI_THEME_PADDING_PX/2 (4px) rather
 * than the full 8px so all five rows fit comfortably inside this page's
 * content budget (same 480x320 landscape budget ui_page_home.c's header
 * comment derives). The in-content Back button that used to share this
 * budget moved into the shared top bar (ui_topbar.c) in the 2026-08-21
 * icon-topbar pass, freeing UI_THEME_MIN_TOUCH_TARGET_PX (72px) + the 4px
 * row gap back to content. */
static lv_obj_t *build_stat_label(lv_obj_t *parent, const char *initial_text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, initial_text);
    return label;
}

lv_obj_t *ui_page_safety_build(void)
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
        .title = "Safety Processor",
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

    s_safety_temp_label = build_stat_label(content, "Safety temp: ---");
    s_enclosure_temp_label = build_stat_label(content, "Enclosure temp: ---");
    s_safety_power_label = build_stat_label(content, "Power: ---");
    s_link_version_label = build_stat_label(content, "Link version: ---");
    s_trip_label = build_stat_label(content, "Last trip: ---");

    ui_topbar_raise(&tb);

    lv_timer_create(refresh_cb, UI_PAGE_SAFETY_REFRESH_MS, NULL);
    refresh_cb(NULL);

    return scr;
}
