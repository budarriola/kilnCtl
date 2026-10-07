#include "ui_page_safety.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "dashboard_http.h"
#include "kiln_ui.h"
#include "lcd_auth_state.h"
#include "safety_link.h"
#include "safety_trip_words.h"
#include "ui_lcd_lock.h"
#include "ui_page_safety_logic.h"
#include "ui_theme.h"
#include "ui_topbar.h"

// LCD Safety / Alarm page, TODO.md sec 0.5.
//
// Owner decisions 2026-10-07: viewing (current trip state + last trip) is
// dashboard-level, no PIN. Clear Trip needs an LCD admin login
// (ui_lcd_lock_run_gated). A trip cleared anywhere else (web, MCP, UART) must
// make this page's alarm go away too.
//
// "Reset one side of a pair" (CLAUDE.md): this file holds NO trip latch. Every
// refresh re-derives ui_safety_view_t from the live DIAG state
// (ui_safety_view_derive), and the only thing the button does is send the
// same clear the web route sends (dashboard_safety_clear_trip), whose own
// live check refuses a trip that is no longer latched. Feedback text is
// cosmetic and is overwritten, never consulted.
//
// History: the only retained trip record is the last Frame D event
// (trip_event_*); there is no multi-trip ring on the link, so "history" here
// is that last event. The diagnostics Trip Detail page shows the same event
// in full (cause/remedy/fault source); this page clips cause and remedy to
// fit the no-scroll budget.

static const char *TAG __attribute__((unused)) = "ui_page_safety";

#define UI_PAGE_SAFETY_REFRESH_MS 1000u
#define UI_PAGE_SAFETY_ROW_PAD_PX (UI_THEME_PADDING_PX / 2)
#define UI_PAGE_SAFETY_GAP_PX     (UI_THEME_PADDING_PX / 2)
#define UI_PAGE_SAFETY_BTN_H_PX   72
#define UI_PAGE_SAFETY_BTN_W_PCT  45

/* Row heights: lines * font line height + top/bottom padding. */
#define ROW_H(lines) ((lines) * UI_THEME_FONT_LINE_HEIGHT_PX + 2 * UI_PAGE_SAFETY_ROW_PAD_PX)
#define UI_PAGE_SAFETY_TOTAL_PX                                             \
    (ROW_H(1) + ROW_H(1) + ROW_H(2) + ROW_H(3) + UI_PAGE_SAFETY_BTN_H_PX + \
     4 * UI_PAGE_SAFETY_GAP_PX)
_Static_assert(UI_PAGE_SAFETY_TOTAL_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "Safety page rows exceed the no-scroll content budget");

static lv_obj_t *s_state_label;
static lv_obj_t *s_last_label;
static lv_obj_t *s_cause_label;
static lv_obj_t *s_remedy_label;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_label;
static lv_obj_t *s_feedback_label;
static lv_obj_t *s_scr;
static ui_topbar_t s_topbar;
/* The view refresh_cb last derived (LVGL task only). clear_action uses it
 * ONLY to pick the operator-facing message; it is never the gate on whether
 * a clear may happen. The live "is a trip latched, is DIAG fresh" check is
 * safety_link_send_clear_trip()'s own, applied at send time on the same path
 * the web route uses, so a stale copy here can at worst produce a refused
 * send, never a clear of something that is not latched. Overwritten every
 * refresh, never reset by the button (no LCD-local latch). */
static ui_safety_view_t s_view = { .tripped_live = false, .diag_unknown = true };
/* One text scratch buffer for every label refresh_cb writes (LVGL task only;
 * lv_label_set_text() copies, so reuse between labels is safe). 340 =
 * "Detected: " (10) + the 320-byte numbered-cause room ui_page_diagnostics.c
 * sizes for pathological float magnitudes + NUL headroom; also covers
 * "To clear: " + the longest remedy (149). Static rather than on the LVGL
 * stack only because it is cheap to keep; the 1 KB dashboard_status_t is on
 * the stack, same as every other live LCD page. */
static char s_text[340];

static lv_obj_t *build_row(lv_obj_t *parent, lv_color_t accent, bool dot_long)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_PAGE_SAFETY_ROW_PAD_PX, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, accent, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, dot_long ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, "--");
    return label;
}

/* Fixed-height label: LONG_DOT needs a definite height to know where to cut. */
static void pin_label_lines(lv_obj_t *label, int lines)
{
    lv_obj_set_height(label, lines * UI_THEME_FONT_LINE_HEIGHT_PX);
}

static void set_button_enabled(bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(s_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_btn, UI_THEME_ACCENT_5, 0);
    } else {
        lv_obj_add_state(s_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_btn, UI_THEME_COLOR_CARD, 0);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    /* dashboard_get_status() is a heavy producer call (SPI reads, heap
     * walks): skip it while this page is not the active screen. The page is
     * cached, never torn down, so the timer outlives visibility. */
    if (s_scr == NULL || lv_screen_active() != s_scr) {
        return;
    }

    /* On the LVGL task stack (10 KB), like ui_page_home_refresh.c,
     * ui_page_temperature.c and ui_page_diagnostics.c -- the last holds this
     * same struct plus ~1 KB of text buffers in one frame, so this frame is
     * strictly shallower than one the stack already carries. Kept off .bss:
     * .dram0.bss headroom is the scarcer budget. */
    dashboard_status_t ds;
    char *const buf = s_text;
    const size_t buf_len = sizeof(s_text);
    dashboard_get_status(&ds);
    const ui_safety_view_t view = ui_safety_view_derive(ds.diag_ever_received, ds.diag_state,
                                                         ds.diag_age_ms);
    s_view = view;

    if (view.diag_unknown) {
        snprintf(buf, buf_len, "State: UNKNOWN (no fresh diagnostics)");
    } else if (view.tripped_live) {
        snprintf(buf, buf_len, "State: TRIPPED NOW -- %s", safety_trip_words_short(ds.diag_trip_reason));
    } else {
        switch (ds.diag_state) {
        case SAFETY_LINK_DIAG_STATE_INIT:  snprintf(buf, buf_len, "State: starting up"); break;
        case SAFETY_LINK_DIAG_STATE_GRACE: snprintf(buf, buf_len, "State: startup grace"); break;
        case SAFETY_LINK_DIAG_STATE_ARMED: snprintf(buf, buf_len, "State: ARMED, no trip"); break;
        case SAFETY_LINK_DIAG_STATE_WARN:  snprintf(buf, buf_len, "State: ARMED (warning)"); break;
        default:                           snprintf(buf, buf_len, "State: unrecognised"); break;
        }
    }
    lv_label_set_text(s_state_label, buf);
    lv_obj_set_style_border_color(lv_obj_get_parent(s_state_label),
                                  view.tripped_live ? UI_THEME_ACCENT_5 : UI_THEME_ACCENT_4, 0);

    if (!ds.trip_event_ever_received) {
        lv_label_set_text(s_last_label, "Last trip: none recorded");
        lv_label_set_text(s_cause_label, "Detected: --");
        lv_label_set_text(s_remedy_label, "To clear: --");
    } else {
        char age[24];
        ui_safety_format_age(ds.trip_event_age_ms, age, sizeof(age));
        snprintf(buf, buf_len, "Last trip: %s (%s)", safety_trip_words_short(ds.trip_reason), age);
        lv_label_set_text(s_last_label, buf);

        /* Prefix written in place, numbered cause rendered straight after it
         * (cause_numbered() always writes into the buffer it is given and
         * NUL-terminates within its length), so no second 320-byte buffer. */
        static const char k_detected[] = "Detected: ";
        memcpy(buf, k_detected, sizeof(k_detected) - 1u);
        (void)safety_trip_words_cause_numbered(ds.trip_reason, ds.trip_safety_tc_c,
                                               ds.trip_deciding_threshold, ds.trip_current_a,
                                               ds.trip_context_age_100ms, buf + (sizeof(k_detected) - 1u),
                                               buf_len - (sizeof(k_detected) - 1u));
        lv_label_set_text(s_cause_label, buf);

        snprintf(buf, buf_len, "To clear: %s", safety_trip_words_remedy(ds.trip_reason));
        lv_label_set_text(s_remedy_label, buf);
    }

    /* Offered only while the live state says tripped; the admin gate runs on
     * tap, and the send re-checks the live state itself. */
    set_button_enabled(view.tripped_live);
    if (!view.tripped_live) {
        /* Stale "Cleared"/"Refused" text must not outlive its trip. */
        lv_label_set_text(s_feedback_label, "");
    }
}

/* Runs only after the LCD admin gate has passed (or auth is off). Re-checks
 * the role (the gate is not trusted to be the only check) and then sends
 * through dashboard_safety_clear_trip() -- the web route's exact path, whose
 * safety_link_send_clear_trip() re-reads the LIVE cached DIAG under the link
 * lock and refuses (ESP_ERR_INVALID_STATE) a trip that is no longer latched
 * or a stale link. No dashboard_get_status() here: that is a heavy producer
 * call (SPI reads, heap walks) and the authoritative live check is the
 * send's own; s_view (<= one refresh old) only chooses the message. */
static void clear_action(void *user)
{
    (void)user;
    switch (ui_safety_clear_verdict(&s_view, ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN))) {
    case UI_SAFETY_CLEAR_NOT_TRIPPED:
        lv_label_set_text(s_feedback_label, "No trip to clear");
        return;
    case UI_SAFETY_CLEAR_NEEDS_ADMIN:
        lv_label_set_text(s_feedback_label, "Admin login required");
        return;
    case UI_SAFETY_CLEAR_SEND:
    default:
        break;
    }

    esp_err_t err = dashboard_safety_clear_trip();
    if (err == ESP_OK) {
        /* Fire-and-forget: the Pico decides. The state row flips only when a
         * later DIAG frame says so, never from this call's return. */
        lv_label_set_text(s_feedback_label, "Clear sent");
    } else if (err == ESP_ERR_INVALID_STATE) {
        lv_label_set_text(s_feedback_label, "Refused: no live trip");
    } else {
        lv_label_set_text(s_feedback_label, "Clear failed");
    }
    ESP_LOGI(TAG, "LCD clear trip -> %s", esp_err_to_name(err));
}

static void btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_run_gated("Enter admin PIN to clear trip", LCD_PIN_ROLE_ADMIN, clear_action, NULL);
}

lv_obj_t *ui_page_safety_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_PAGE_SAFETY_GAP_PX, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Safety",
        .back_page = "config",
        .show_home = true,
    }, &s_topbar);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_PAGE_SAFETY_GAP_PX, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    s_state_label = build_row(content, UI_THEME_ACCENT_4, false);
    s_last_label = build_row(content, UI_THEME_ACCENT_1, false);
    s_cause_label = build_row(content, UI_THEME_ACCENT_2, true);
    pin_label_lines(s_cause_label, 2);
    s_remedy_label = build_row(content, UI_THEME_ACCENT_3, true);
    pin_label_lines(s_remedy_label, 3);

    lv_obj_t *brow = lv_obj_create(content);
    lv_obj_set_width(brow, lv_pct(100));
    lv_obj_set_height(brow, UI_PAGE_SAFETY_BTN_H_PX);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brow, UI_THEME_PADDING_PX, 0);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    s_btn = lv_button_create(brow);
    lv_obj_set_size(s_btn, lv_pct(UI_PAGE_SAFETY_BTN_W_PCT), UI_PAGE_SAFETY_BTN_H_PX);
    lv_obj_set_style_radius(s_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(s_btn, btn_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(s_btn);
    lv_obj_set_style_text_color(s_btn_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_btn_label, "Clear Trip");
    lv_obj_center(s_btn_label);
    set_button_enabled(false);

    s_feedback_label = lv_label_create(brow);
    lv_obj_set_flex_grow(s_feedback_label, 1);
    lv_label_set_long_mode(s_feedback_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_feedback_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_feedback_label, "");

    ui_topbar_raise(&s_topbar);
    lv_timer_create(refresh_cb, UI_PAGE_SAFETY_REFRESH_MS, NULL);
    return scr;
}
