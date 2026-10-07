#include "ui_page_safety.h"

#include <stdio.h>

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
// (ui_safety_view_derive), the clear action re-derives it again at execution
// time, and the only thing the button does is send the same clear the web
// route sends (dashboard_safety_clear_trip). Feedback text is cosmetic and
// is overwritten, never consulted.
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
/* One LVGL-task-only status snapshot shared by refresh_cb and clear_action
 * (both run on the LVGL task, never concurrently): keeps the large
 * dashboard_status_t off the 8 KB LVGL task stack. */
static dashboard_status_t s_ds;
static ui_topbar_t s_topbar;

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

    /* Static, not stack: this runs on the LVGL task (8 KB stack). The timer
     * callback is the only user and LVGL timers never nest, so one copy is
     * safe -- same reasoning as the other LVGL-task-only scratch buffers. */
    static char buf[200];
    dashboard_get_status(&s_ds);
    const ui_safety_view_t view = ui_safety_view_derive(s_ds.diag_ever_received, s_ds.diag_state,
                                                         s_ds.diag_age_ms);

    if (view.diag_unknown) {
        snprintf(buf, sizeof(buf), "State: UNKNOWN (no fresh diagnostics)");
    } else if (view.tripped_live) {
        snprintf(buf, sizeof(buf), "State: TRIPPED NOW -- %s",
                 safety_trip_words_short(s_ds.diag_trip_reason));
    } else {
        switch (s_ds.diag_state) {
        case SAFETY_LINK_DIAG_STATE_INIT:  snprintf(buf, sizeof(buf), "State: starting up"); break;
        case SAFETY_LINK_DIAG_STATE_GRACE: snprintf(buf, sizeof(buf), "State: startup grace"); break;
        case SAFETY_LINK_DIAG_STATE_ARMED: snprintf(buf, sizeof(buf), "State: ARMED, no trip"); break;
        case SAFETY_LINK_DIAG_STATE_WARN:  snprintf(buf, sizeof(buf), "State: ARMED (warning)"); break;
        default:                           snprintf(buf, sizeof(buf), "State: unrecognised"); break;
        }
    }
    lv_label_set_text(s_state_label, buf);
    lv_obj_set_style_border_color(lv_obj_get_parent(s_state_label),
                                  view.tripped_live ? UI_THEME_ACCENT_5 : UI_THEME_ACCENT_4, 0);

    if (!s_ds.trip_event_ever_received) {
        lv_label_set_text(s_last_label, "Last trip: none recorded");
        lv_label_set_text(s_cause_label, "Detected: --");
        lv_label_set_text(s_remedy_label, "To clear: --");
    } else {
        char age[24];
        ui_safety_format_age(s_ds.trip_event_age_ms, age, sizeof(age));
        snprintf(buf, sizeof(buf), "Last trip: %s (%s)", safety_trip_words_short(s_ds.trip_reason), age);
        lv_label_set_text(s_last_label, buf);

        static char num[320];
        static char cause[340];
        snprintf(cause, sizeof(cause), "Detected: %s",
                 safety_trip_words_cause_numbered(s_ds.trip_reason, s_ds.trip_safety_tc_c,
                                                   s_ds.trip_deciding_threshold, s_ds.trip_current_a,
                                                   s_ds.trip_context_age_100ms, num, sizeof(num)));
        lv_label_set_text(s_cause_label, cause);

        static char remedy[160];
        snprintf(remedy, sizeof(remedy), "To clear: %s", safety_trip_words_remedy(s_ds.trip_reason));
        lv_label_set_text(s_remedy_label, remedy);
    }

    /* Offered only while the live state says tripped; the admin gate runs on
     * tap, and the action re-derives both again. */
    set_button_enabled(view.tripped_live);
    if (!view.tripped_live) {
        /* Stale "Cleared"/"Refused" text must not outlive its trip. */
        lv_label_set_text(s_feedback_label, "");
    }
}

/* Runs only after the LCD admin gate has passed (or auth is off). Re-derives
 * the live view and re-checks the role: the gate is not trusted to be the
 * only check, and the trip may have been cleared elsewhere meanwhile. */
static void clear_action(void *user)
{
    (void)user;
    dashboard_get_status(&s_ds);
    const ui_safety_view_t view = ui_safety_view_derive(s_ds.diag_ever_received, s_ds.diag_state,
                                                         s_ds.diag_age_ms);
    switch (ui_safety_clear_verdict(&view, ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN))) {
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
        lv_label_set_text(s_feedback_label, "Refused: not clearable now");
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
