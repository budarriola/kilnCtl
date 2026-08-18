#include "ui_page_temperature.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "ui_theme.h"
#include "zones_http.h"

// TODO.md 10.3's real "Temperature" page -- individual per-zone manual
// relay control, the touchscreen equivalent of section 2's web dashboard's
// manual relay override (dashboard_http.c's POST /api/relay). Replaces the
// pre-this-pass title+Back stub (see git history).
//
// Per TODO.md 10.1a, every write here goes through dashboard_set_relay()
// (dashboard_http.h/.c) -- extracted THIS pass from relay_post_handler(),
// the same way dashboard_get_status() was extracted from status_get_handler()
// last pass -- so this page cannot drift from the web dashboard's ownership
// (relay_authority_manual_blocked_by_owner()) and safety-fault
// (relay_authority_on_blocked(), ON direction only -- OFF is never gated,
// see relay_authority.h) gating, or from its kiln_io_set_relay() write.
// Nothing here calls kiln_io or relay_authority directly.
//
// Built this pass: per-zone current temperature (same dashboard_get_status()
// snapshot ui_page_home.c reads), one toggle button per relay bit set in
// that zone's relay_mask (zones_config_get_relay_mask()), and an honest
// refusal message -- if dashboard_set_relay() refuses a command (owned by a
// running profile, blocked by a safety fault, no board attached, or the
// kiln_io write itself failing), that reason is shown in a status label, not
// silently swallowed. A relay's button reflects live state (ON/OFF/"no
// board") every UI_PAGE_TEMPERATURE_REFRESH_MS tick, same cadence as
// ui_page_home.c.
//
// Known gaps, documented rather than silently missing:
// - **Zone names.** Same gap ui_page_home.c already flagged: zones_http.h
//   exposes no public getter for a zone's configured name, so each zone
//   card shows "Zone N".
// - **Per-relay, not per-zone, toggles.** The web dashboard's manual
//   override (POST /api/relay) always addressed one relay at a time, and
//   this page follows that shape exactly rather than inventing a
//   "toggle all of this zone's relays together" action the backend has
//   never had -- a zone with more than one relay in its mask gets more than
//   one button.
// - **A relay mask shared by more than one zone** (legal per zones_http.h,
//   though unusual) is rendered as one real toggle button under whichever
//   zone claims it first and a "(shared)" note under any later zone that
//   also claims it, rather than a second independent button for the same
//   physical relay -- two buttons for one relay would let the on-screen
//   state of one silently go stale the instant the other is tapped.

static const char *TAG = "ui_page_temperature";

#define UI_PAGE_TEMPERATURE_REFRESH_MS 1000

typedef struct {
    lv_obj_t *temp_label;
} zone_widgets_t;

/* Ties a relay toggle button's LV_EVENT_CLICKED callback back to which
 * relay it controls -- one static ctx per relay (board-wide, 1-based index
 * matching kiln_io_set_relay's convention), passed as the event's
 * user_data. Never freed/reused across pages (this page's screen, like
 * every other one, is built once and kept alive forever -- kiln_ui.h's
 * header comment), so a plain static array outlives every button that
 * points into it. */
typedef struct {
    uint8_t relay_index; /* 1-based */
} relay_ctx_t;

static zone_widgets_t s_zone[MAX31856_CHANNEL_COUNT];
static uint8_t s_zone_count;

static relay_ctx_t s_relay_ctx[KILN_IO_RELAY_COUNT];
static lv_obj_t *s_relay_btn[KILN_IO_RELAY_COUNT];   /* NULL if that relay has no button on this page */
static lv_obj_t *s_relay_label[KILN_IO_RELAY_COUNT]; /* label inside s_relay_btn[r] */
static bool s_relay_claimed[KILN_IO_RELAY_COUNT];    /* true once some zone's row has built this relay's button */

static lv_obj_t *s_msg_label; /* last refusal (or "") -- see this file's header comment */

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("home");
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Same plain-C getter dashboard_http.c's GET /api/status handler and
     * ui_page_home.c's refresh both call -- TODO.md 10.1a. */
    dashboard_status_t ds;
    dashboard_get_status(&ds);

    for (uint8_t zi = 0; zi < s_zone_count; zi++) {
        char buf[24];
        const dashboard_channel_status_t *ch = NULL;
        for (size_t i = 0; i < ds.channel_count; i++) {
            /* Legacy zone_index == MAX31856 channel mapping, same as
             * ui_page_home.c / dashboard_http.c. */
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
    }

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (!s_relay_btn[r]) {
            continue;
        }
        bool on = ds.io_ready && ds.relay_on[r];
        char label_buf[24];
        if (!ds.io_ready) {
            snprintf(label_buf, sizeof(label_buf), "Relay %u\nno board", (unsigned)(r + 1));
        } else {
            snprintf(label_buf, sizeof(label_buf), "Relay %u\n%s", (unsigned)(r + 1), on ? "ON" : "OFF");
        }
        lv_label_set_text(s_relay_label[r], label_buf);
        lv_obj_set_style_bg_color(s_relay_btn[r], on ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_CARD, 0);
    }
}

static void relay_toggle_cb(lv_event_t *e)
{
    relay_ctx_t *ctx = (relay_ctx_t *)lv_event_get_user_data(e);

    /* Read current state fresh rather than trusting the button's last-drawn
     * color -- the same "read through the same getter, don't trust cached
     * widget state" discipline as everything else on this page. */
    dashboard_status_t ds;
    dashboard_get_status(&ds);
    bool currently_on = ds.io_ready && ds.relay_on[ctx->relay_index - 1];
    bool want_on = !currently_on;

    /* The one write path -- see this file's header comment. */
    uint32_t sources = 0;
    dashboard_relay_result_t result = dashboard_set_relay(ctx->relay_index, want_on, &sources);

    char msg[64];
    switch (result) {
    case DASHBOARD_RELAY_OK:
        msg[0] = '\0';
        break;
    case DASHBOARD_RELAY_ERR_NO_BOARD:
        snprintf(msg, sizeof(msg), "Relay %u: no relay board attached", (unsigned)ctx->relay_index);
        break;
    case DASHBOARD_RELAY_ERR_RANGE:
        /* Should not happen -- ctx->relay_index is always a valid
         * board-wide relay index built from KILN_IO_RELAY_COUNT below --
         * but handled rather than assumed unreachable. */
        snprintf(msg, sizeof(msg), "Relay %u: out of range", (unsigned)ctx->relay_index);
        break;
    case DASHBOARD_RELAY_ERR_OWNED:
        snprintf(msg, sizeof(msg), "Relay %u refused -- owned by a running profile", (unsigned)ctx->relay_index);
        break;
    case DASHBOARD_RELAY_ERR_SAFETY:
        snprintf(msg, sizeof(msg), "Relay %u refused -- safety fault 0x%02X", (unsigned)ctx->relay_index,
                 (unsigned)sources);
        break;
    case DASHBOARD_RELAY_ERR_IO_FAIL:
    default:
        snprintf(msg, sizeof(msg), "Relay %u: command failed", (unsigned)ctx->relay_index);
        break;
    }

    if (result != DASHBOARD_RELAY_OK) {
        ESP_LOGW(TAG, "%s", msg);
    }
    lv_label_set_text(s_msg_label, msg);
    lv_obj_set_style_text_color(s_msg_label, result == DASHBOARD_RELAY_OK ? UI_THEME_COLOR_TEXT_SECONDARY
                                                                           : UI_THEME_ACCENT_5,
                                 0);

    refresh_cb(NULL); /* repaint immediately instead of waiting one tick */
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
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(row, UI_THEME_PADDING_PX / 2, 0);

    lv_obj_t *header = lv_obj_create(row);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSPARENT, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(header);
    lv_obj_set_style_text_color(name, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char name_buf[16];
    /* Same "no zone-name getter" gap as ui_page_home.c -- see this file's
     * header comment. */
    snprintf(name_buf, sizeof(name_buf), "Zone %u", (unsigned)zone_index);
    lv_label_set_text(name, name_buf);

    lv_obj_t *temp = lv_label_create(header);
    lv_obj_set_style_text_color(temp, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(temp, "-- C");
    s_zone[zone_index].temp_label = temp;

    uint8_t relay_mask = 0;
    bool have_mask = zones_config_get_relay_mask(zone_index, &relay_mask);

    lv_obj_t *relay_row = lv_obj_create(row);
    lv_obj_set_width(relay_row, lv_pct(100));
    lv_obj_set_height(relay_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(relay_row, LV_OPA_TRANSPARENT, 0);
    lv_obj_set_style_border_width(relay_row, 0, 0);
    lv_obj_set_style_pad_all(relay_row, 0, 0);
    lv_obj_set_flex_flow(relay_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(relay_row, UI_THEME_PADDING_PX / 2, 0);

    if (!have_mask || relay_mask == 0) {
        lv_obj_t *none = lv_label_create(relay_row);
        lv_obj_set_style_text_color(none, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(none, "no relay assigned");
        return;
    }

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (!(relay_mask & (1u << r))) {
            continue;
        }

        if (s_relay_claimed[r]) {
            /* See this file's header comment on the shared-relay edge
             * case -- a note, not a second independent button. */
            lv_obj_t *dup = lv_label_create(relay_row);
            lv_obj_set_style_text_color(dup, UI_THEME_COLOR_TEXT_SECONDARY, 0);
            char buf[24];
            snprintf(buf, sizeof(buf), "Relay %u (shared)", (unsigned)(r + 1));
            lv_label_set_text(dup, buf);
            continue;
        }
        s_relay_claimed[r] = true;

        lv_obj_t *btn = lv_button_create(relay_row);
        lv_obj_set_width(btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2);
        lv_obj_set_height(btn, UI_THEME_MIN_TOUCH_TARGET_PX);
        lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
        s_relay_ctx[r].relay_index = (uint8_t)(r + 1);
        lv_obj_add_event_cb(btn, relay_toggle_cb, LV_EVENT_CLICKED, &s_relay_ctx[r]);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        char label_buf[24];
        snprintf(label_buf, sizeof(label_buf), "Relay %u\n--", (unsigned)(r + 1));
        lv_label_set_text(label, label_buf);
        lv_obj_center(label);

        /* TODO.md 10.4's touch hit-area helper -- a wrapped row of several
         * relay buttons is the "dense grid" case (compact_layout=true), not
         * ui_page_home.c's sparse single-row button layout. */
        lv_obj_update_layout(btn);
        ui_theme_apply_touch_area(btn, true);

        s_relay_btn[r] = btn;
        s_relay_label[r] = label;
    }
}

lv_obj_t *ui_page_temperature_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX, 0);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSPARENT, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Temperature -- manual relay control");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSPARENT, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX, 0);

    memset(s_relay_claimed, 0, sizeof(s_relay_claimed));
    memset(s_relay_btn, 0, sizeof(s_relay_btn));
    memset(s_relay_label, 0, sizeof(s_relay_label));

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

    /* Refusal/status message -- see this file's header comment: a refused
     * write is shown here, never silently dropped. */
    s_msg_label = lv_label_create(content);
    lv_obj_set_width(s_msg_label, lv_pct(100));
    lv_obj_set_style_text_color(s_msg_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_msg_label, "");

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(back, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_update_layout(back);
    ui_theme_apply_touch_area(back, false);

    /* Pages are never torn down (kiln_ui.h's header comment) -- same
     * "create once, keep refreshing forever" timer lifetime as
     * ui_page_home.c. */
    lv_timer_create(refresh_cb, UI_PAGE_TEMPERATURE_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
