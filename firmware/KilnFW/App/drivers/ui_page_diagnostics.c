#include "ui_page_diagnostics.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board_temps.h"
#include "kiln_ui.h"
#include "ui_theme.h"

// TODO.md "Diagnostics / System info page" -- "One place to look before
// reaching for a serial console." That item names two halves: safety-link
// stats (uptime/RX-TX counts/last-seen age) and "the ESP-only half
// (heap/flash/IC temps)". The safety-link half stays out of scope here,
// same as it does for every other page that has run into it (see
// ui_page_safety.c's own header comment) -- it needs M5's link-stats
// surface, which this pass doesn't touch. This page is the ESP-only half
// only: firmware version/build, ESP uptime, free heap (current + worst-case-
// ever), free PSRAM if any, and the ESP32-S3's own die temperature (reusing
// board_temps_get_live() -- see that file's header comment for why this
// isn't a second, possibly-drifting read of the same hardware). The
// MAX31856 cold-junction detail board_temps_get_live() also returns already
// has its own page (ui_page_board_health.c); repeating it here would just
// be the same numbers under a second name, so this page only reads the
// esp32_valid/esp32_c half of that struct.
//
// "Flash-free" was in the original TODO wording but is deliberately left
// out: this board's OTA-partitioned layout (TODO.md 9's dual app slots +
// several dedicated NVS partitions, see ROADMAP.md M8) doesn't reduce to
// one meaningful "bytes free" number the way a single-partition device's
// would -- reporting just the running app partition's free space would be
// misleading (most of the flash is the OTHER OTA slot plus NVS partitions,
// none of which "free space" here would account for), so this shows heap
// instead, which does have one honest, real-time number.
static const char *TAG __attribute__((unused)) = "ui_page_diagnostics";

#define UI_PAGE_DIAGNOSTICS_REFRESH_MS 2000

/* Fixed-height, internally-scrollable stat list -- same sanctioned pattern
 * ui_page_config.c's nav grid and ui_page_temperature.c's relay_row already
 * use (see either file's header comment) rather than letting 7 stat rows
 * grow this page past its ~264px no-scroll budget (480x320 landscape, see
 * ui_page_home.c's header comment for that number's derivation). Sized so
 * the common case (nothing scrolled) still shows the first ~4 rows without
 * a touch-drag. */
#define UI_PAGE_DIAGNOSTICS_LIST_HEIGHT_PX 180

static lv_obj_t *s_fw_version_label;
static lv_obj_t *s_build_label;
static lv_obj_t *s_uptime_label;
static lv_obj_t *s_heap_free_label;
static lv_obj_t *s_heap_min_free_label;
static lv_obj_t *s_psram_free_label;
static lv_obj_t *s_esp32_temp_label;

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("config");
}

static void format_uptime(char *buf, size_t buf_len)
{
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    int64_t days = uptime_s / 86400;
    int hours = (int)((uptime_s % 86400) / 3600);
    int mins = (int)((uptime_s % 3600) / 60);
    int secs = (int)(uptime_s % 60);
    if (days > 0) {
        snprintf(buf, buf_len, "%lldd %02d:%02d:%02d", (long long)days, hours, mins, secs);
    } else {
        snprintf(buf, buf_len, "%02d:%02d:%02d", hours, mins, secs);
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    char buf[48];

    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min_free = esp_get_minimum_free_heap_size();
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(heap_free / 1024));
    lv_label_set_text(s_heap_free_label, buf);
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(heap_min_free / 1024));
    lv_label_set_text(s_heap_min_free_label, buf);

    /* MALLOC_CAP_SPIRAM is 0 free on a board with no PSRAM populated/enabled
     * rather than an error -- heap_caps_get_free_size() doesn't distinguish
     * "no PSRAM" from "PSRAM full" by return value alone, so 0 is shown as
     * "0 KB", not invented as "n/a" (unlike board_temps_t's genuine
     * hardware-absent case below, there is no separate validity bit here to
     * honestly report absence with). */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(psram_free / 1024));
    lv_label_set_text(s_psram_free_label, buf);

    format_uptime(buf, sizeof(buf));
    lv_label_set_text(s_uptime_label, buf);

    board_temps_t bt;
    board_temps_get_live(&bt);
    if (bt.esp32_valid) {
        snprintf(buf, sizeof(buf), "%.1f C", (double)bt.esp32_c);
        lv_obj_set_style_text_color(s_esp32_temp_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        snprintf(buf, sizeof(buf), "n/a");
        lv_obj_set_style_text_color(s_esp32_temp_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }
    lv_label_set_text(s_esp32_temp_label, buf);
}

static lv_obj_t *build_stat_row(lv_obj_t *parent, const char *name, lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, accent, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, name);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_color(value, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(value, "--");

    return value;
}

lv_obj_t *ui_page_diagnostics_build(void)
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
    lv_label_set_text(title, "Diagnostics");
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
     * on why (7 rows would blow the ~264px budget at LV_SIZE_CONTENT row
     * heights). */
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, UI_PAGE_DIAGNOSTICS_LIST_HEIGHT_PX);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, UI_THEME_PADDING_PX / 2, 0);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    s_fw_version_label = build_stat_row(list, "Firmware version", UI_THEME_ACCENT_1);
    lv_label_set_text(s_fw_version_label, app_desc ? app_desc->version : "n/a");
    s_build_label = build_stat_row(list, "Build date/time", UI_THEME_ACCENT_2);
    if (app_desc) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", app_desc->date, app_desc->time);
        lv_label_set_text(s_build_label, buf);
    } else {
        lv_label_set_text(s_build_label, "n/a");
    }
    s_uptime_label = build_stat_row(list, "Uptime", UI_THEME_ACCENT_3);
    s_heap_free_label = build_stat_row(list, "Free heap", UI_THEME_ACCENT_4);
    s_heap_min_free_label = build_stat_row(list, "Free heap (worst-case)", UI_THEME_ACCENT_1);
    s_psram_free_label = build_stat_row(list, "Free PSRAM", UI_THEME_ACCENT_2);
    s_esp32_temp_label = build_stat_row(list, "ESP32-S3 die temp", UI_THEME_ACCENT_3);

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
    lv_timer_create(refresh_cb, UI_PAGE_DIAGNOSTICS_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
