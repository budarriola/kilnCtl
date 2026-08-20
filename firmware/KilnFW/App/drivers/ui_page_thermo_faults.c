#include "ui_page_thermo_faults.h"

#include <stdio.h>
#include <string.h>

#include "MAX31856.h"
#include "kiln_ui.h"
#include "thermo_owner.h"
#include "ui_theme.h"

// TODO.md "Diagnostics / System info page" item's thermocouple-IC half,
// split into its own page per explicit user request ("for the diagnostic
// pages i also want the ability to see the status of the thermocouple ics
// and all of their possible faults. diagnostics should be broken up into
// multiple pages") -- kept entirely separate from ui_page_diagnostics.c's
// ESP-only system info (firmware version/uptime/heap/die temp), which this
// file does not touch or duplicate.
//
// Live data comes from thermo_owner_command_read_all() only -- never
// MAX31856_read_all()/thermo_bus directly, respecting thermo_owner.h's
// single-writer discipline for the shared MAX31856 SPI bus (see that
// header's top comment). safety_link.c's periodic snapshot is the other
// caller of the same entry point; this page is a second, independent reader
// of the same owner-serialized data, at its own display cadence.
//
// Note MAX31856_read_all()'s (and therefore thermo_owner_command_read_all()'s)
// contract: it fills readings[0..out_count-1] for *initialized* channels
// only, packed by position, not by channel number -- so this page indexes
// the result by each MAX31856Reading::channel field, not by array position,
// exactly like safety_link.c's own call site does.
//
// "Clear faults" is deliberately NOT here: MAX31856_clear_faults() /
// thermo_owner_command_clear_faults() exist, but the user only asked for
// visibility ("the ability to see the status... and all of their possible
// faults"), not a clear action. Left as a natural follow-up, not built.
static const char *TAG __attribute__((unused)) = "ui_page_thermo_faults";

#define UI_PAGE_THERMO_FAULTS_REFRESH_MS 1500

/* Fixed-height, internally-scrollable list -- same sanctioned pattern as
 * ui_page_diagnostics.c's `list` (see that file's header comment, and
 * ui_page_home.c's for the ~264px/480x320-landscape no-scroll budget this
 * is all working within). Each channel's row is capped at
 * UI_PAGE_THERMO_FAULTS_ROW_HEIGHT_PX rather than LV_SIZE_CONTENT, so a
 * worst-case fault string can't make one row balloon and starve the others
 * -- it wraps and, if it still doesn't fit, the row's own overflow is
 * clipped by the fixed row height while the outer list's scrollbar (this
 * page reuses the same list for all 3 rows) remains available regardless.
 *
 * Worst-case row content: "Ch 3: OPEN, OVUV, TCLOW, TCHIGH, CJLOW, CJHIGH,
 * TCRANGE, CJRANGE" (all 8 SR bits asserted at once, the longest this string
 * ever gets since there are only 8 named fault bits total) plus a second
 * line "FAULT pin: yes   SPI: yes". At this page's ~430px usable row width
 * (480px panel minus outer padding) and LVGL's default ~14px font, the fault
 * line (~62 chars) wraps to at most 2 lines; with the status line that's 3
 * text lines per row, comfortably under UI_PAGE_THERMO_FAULTS_ROW_HEIGHT_PX
 * (72px, chosen as exactly UI_THEME_MIN_TOUCH_TARGET_PX so it also reads as
 * a normal-density row, not squeezed). 3 channels x 72px = 216px of content
 * against a UI_PAGE_THERMO_FAULTS_LIST_HEIGHT_PX of 180px -- i.e. the list is
 * deliberately sized to NOT show all 3 rows at once without a touch-drag
 * (matching ui_page_diagnostics.c's own "first ~4 of 7 visible" tradeoff),
 * trading a bit of always-visible content for guaranteed headroom against
 * the worst-case 8-fault row height, rather than sizing the list to the
 * common case and letting a genuinely faulted board's text overflow it. */
#define UI_PAGE_THERMO_FAULTS_LIST_HEIGHT_PX 180
#define UI_PAGE_THERMO_FAULTS_ROW_HEIGHT_PX  72

static lv_obj_t *s_fault_label[MAX31856_CHANNEL_COUNT];
static lv_obj_t *s_status_label[MAX31856_CHANNEL_COUNT];

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("config");
}

/* Appends every asserted bit's name to buf (comma-separated), or "OK" if
 * fault_status is 0. Reuses MAX31856_MASK_* for the low six bits (SR shares
 * bit positions/names with MASK, see MAX31856.h's comment above
 * MAX31856_FAULT_TCRANGE) and the two SR-only MAX31856_FAULT_* bits for the
 * top two. */
static void format_fault_summary(uint8_t fault_status, char *buf, size_t buf_len)
{
    if (fault_status == 0) {
        snprintf(buf, buf_len, "OK");
        return;
    }

    static const struct {
        uint8_t mask;
        const char *name;
    } bits[] = {
        { MAX31856_MASK_OPEN,    "OPEN" },
        { MAX31856_MASK_OVUV,    "OVUV" },
        { MAX31856_MASK_TCLOW,   "TCLOW" },
        { MAX31856_MASK_TCHIGH,  "TCHIGH" },
        { MAX31856_MASK_CJLOW,   "CJLOW" },
        { MAX31856_MASK_CJHIGH,  "CJHIGH" },
        { MAX31856_FAULT_TCRANGE, "TCRANGE" },
        { MAX31856_FAULT_CJRANGE, "CJRANGE" },
    };

    buf[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (fault_status & bits[i].mask) {
            size_t used = strlen(buf);
            snprintf(buf + used, buf_len - used, "%s%s", first ? "" : ", ", bits[i].name);
            first = false;
        }
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t reading_count = 0;
    memset(readings, 0, sizeof(readings));
    (void)thermo_owner_command_read_all(readings, MAX31856_CHANNEL_COUNT, &reading_count);

    /* Not-present-in-this-read-back default: a channel that never came up
     * (thermo_bus down, or this channel never initialized) is left as "no
     * data" rather than silently showing stale/zeroed OK text. */
    bool seen[MAX31856_CHANNEL_COUNT] = { false };

    for (size_t i = 0; i < reading_count && i < MAX31856_CHANNEL_COUNT; i++) {
        uint8_t ch = readings[i].channel;
        if (ch >= MAX31856_CHANNEL_COUNT) {
            continue; /* defensive; channel is always 0..2 on this board */
        }
        seen[ch] = true;

        char fault_buf[80];
        format_fault_summary(readings[i].fault_status, fault_buf, sizeof(fault_buf));
        bool faulted = (readings[i].fault_status != 0) || readings[i].spi_failed;
        lv_label_set_text(s_fault_label[ch], fault_buf);
        lv_obj_set_style_text_color(s_fault_label[ch],
                                     faulted ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_TEXT_PRIMARY, 0);

        char status_buf[48];
        snprintf(status_buf, sizeof(status_buf), "FAULT pin: %s   SPI: %s",
                 readings[i].fault_pin_asserted ? "yes" : "no",
                 readings[i].spi_failed ? "FAILED" : "ok");
        lv_label_set_text(s_status_label[ch], status_buf);
        lv_obj_set_style_text_color(s_status_label[ch],
                                     readings[i].spi_failed ? UI_THEME_ACCENT_5
                                                             : UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }

    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        if (!seen[ch]) {
            lv_label_set_text(s_fault_label[ch], "no data");
            lv_obj_set_style_text_color(s_fault_label[ch], UI_THEME_COLOR_TEXT_SECONDARY, 0);
            lv_label_set_text(s_status_label[ch], "channel not initialized");
            lv_obj_set_style_text_color(s_status_label[ch], UI_THEME_COLOR_TEXT_SECONDARY, 0);
        }
    }
}

/* One fixed-height card per channel: a title row ("Channel N"), a fault
 * summary label (wraps within the row's width, see this file's header
 * comment for the worst-case sizing), and a status line (FAULT pin / SPI).
 * `accent` colors the left border, matching build_stat_row()'s pattern in
 * ui_page_diagnostics.c/ui_page_board_health.c. */
static void build_channel_row(lv_obj_t *parent, uint8_t channel, lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, UI_PAGE_THERMO_FAULTS_ROW_HEIGHT_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, accent, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char title_buf[24];
    snprintf(title_buf, sizeof(title_buf), "Channel %u", (unsigned)channel);
    lv_label_set_text(title, title_buf);

    lv_obj_t *fault_label = lv_label_create(row);
    lv_obj_set_width(fault_label, lv_pct(100));
    lv_label_set_long_mode(fault_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(fault_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(fault_label, "--");
    s_fault_label[channel] = fault_label;

    lv_obj_t *status_label = lv_label_create(row);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(status_label, "--");
    s_status_label[channel] = status_label;
}

lv_obj_t *ui_page_thermo_faults_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Thermocouple Faults");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* Fixed-height, internally scrollable -- see this file's header comment
     * for the worst-case row-height math this depends on. */
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, UI_PAGE_THERMO_FAULTS_LIST_HEIGHT_PX);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, UI_THEME_PADDING_PX / 2, 0);

    lv_color_t accent_rotation[3] = { UI_THEME_ACCENT_1, UI_THEME_ACCENT_2, UI_THEME_ACCENT_3 };
    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        build_channel_row(list, ch, accent_rotation[ch % 3]);
    }

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX * 2, 44);
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
     * "create once, keep refreshing forever" timer lifetime as every other
     * live-data page. */
    lv_timer_create(refresh_cb, UI_PAGE_THERMO_FAULTS_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real fault state immediately instead of waiting one tick */

    return scr;
}
