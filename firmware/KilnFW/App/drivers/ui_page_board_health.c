#include "ui_page_board_health.h"

#include <stdio.h>

#include "board_temps.h"
#include "MAX31856.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"

// TODO.md 10.7's LCD-side board-health page -- see this file's header
// comment for why it's a separate page. Every number here comes from
// board_temps_get_live() (board_temps.c/.h), the plain-C getter extracted
// THIS pass from api_board_temps_get_handler() the same way
// dashboard_get_status() was extracted from status_get_handler() last pass
// (TODO.md 10.1a) -- it performs the exact same live read
// GET /api/board_temps does (ESP32-S3 internal die sensor +
// MAX31856_read_all()'s cold-junction field per active channel), not a
// second, possibly-drifting read of the same hardware. Nothing here touches
// driver/temperature_sensor.h or MAX31856_read_all() directly.
//
// null/unavailable handling (TODO.md 10.7's own null-not-zero convention
// for GET /api/board_temps, board_temps.h's board_temps_t doc comment):
// - esp32_c shows "n/a" in secondary/dim text rather than a blank or a
//   fabricated 0.0 when board_temps_t::esp32_valid is false (the internal
//   sensor's temperature_sensor_install()/_enable() never succeeded this
//   boot, or board_temps_start() was never called -- see board_temps.c's
//   header comment on this driver being unverified against the installed
//   toolchain).
// - Each MAX31856 channel row shows "n/a" the same way when that channel's
//   thermo_cj_valid[] entry is false (channel not initialized this boot, a
//   transport failure, or a CJRANGE fault -- board_temps_get()'s doc
//   comment) or when the channel index is beyond board_temps_t::thermo_count
//   (no thermo_bus attached, or fewer channels came up than
//   MAX31856_CHANNEL_COUNT). A row is always drawn for every one of the
//   MAX31856_CHANNEL_COUNT possible channels regardless of how many are
//   actually populated this boot, so a channel that never reports anything
//   still shows as a visible "n/a" row, not a row that silently never
//   appears.

/* reserved -- no ESP_LOGx call needed yet on this read-only page, kept for
 * parity with every other ui_page_*.c's TAG convention in case a future pass
 * adds one. __attribute__((unused)) instead of a `(void)TAG;` statement --
 * that statement is only valid inside a function, not at file scope; found
 * building 2026-08-18. */
static const char *TAG __attribute__((unused)) = "ui_page_board_health";

#define UI_PAGE_BOARD_HEALTH_REFRESH_MS 1000

static lv_obj_t *s_esp32_label;
static lv_obj_t *s_cj_label[MAX31856_CHANNEL_COUNT];

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    board_temps_t bt;
    board_temps_get_live(&bt);

    /* ROADMAP.md 2026-08-21 shared unit preference. unit_pref_get() is O(1)
     * RAM-only, no need to route this through dashboard_get_status() -- see
     * unit_pref.h. ABSOLUTE conversion in both spots below: these are live
     * sensor readings, never rates. */
    unit_pref_t unit = unit_pref_get();

    char buf[32];
    if (bt.esp32_valid) {
        snprintf(buf, sizeof(buf), "%.1f %s", (double)unit_pref_convert(bt.esp32_c, unit, UNIT_PREF_KIND_ABSOLUTE),
                 unit_pref_suffix(unit));
        lv_obj_set_style_text_color(s_esp32_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        snprintf(buf, sizeof(buf), "n/a");
        lv_obj_set_style_text_color(s_esp32_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }
    lv_label_set_text(s_esp32_label, buf);

    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        bool valid = (ch < bt.thermo_count) && bt.thermo_cj_valid[ch];
        char cbuf[32];
        if (valid) {
            snprintf(cbuf, sizeof(cbuf), "%.1f %s",
                     (double)unit_pref_convert(bt.thermo_cj_c[ch], unit, UNIT_PREF_KIND_ABSOLUTE),
                     unit_pref_suffix(unit));
            lv_obj_set_style_text_color(s_cj_label[ch], UI_THEME_COLOR_TEXT_PRIMARY, 0);
        } else {
            snprintf(cbuf, sizeof(cbuf), "n/a");
            lv_obj_set_style_text_color(s_cj_label[ch], UI_THEME_COLOR_TEXT_SECONDARY, 0);
        }
        lv_label_set_text(s_cj_label[ch], cbuf);
    }
}

/* 2026-08-18 no-scroll pass: pad_all trimmed from UI_THEME_PADDING_PX (8px)
 * to UI_THEME_PADDING_PX/2 (4px) so this page's esp32 + MAX31856_CHANNEL_COUNT
 * (3) channel rows fit with a real margin inside its content budget (480x320
 * landscape, this codebase's actual runtime canvas -- see ui_page_home.c's
 * header comment for that number's derivation). The Back button that used to
 * share this budget moved into the shared top bar (ui_topbar.c) in the
 * 2026-08-21 icon-topbar pass, freeing 44px + the 4px row gap back to
 * content. Row content/behavior is otherwise unchanged. */
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

lv_obj_t *ui_page_board_health_build(void)
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
        .title = "Board Health -- onboard IC temperatures",
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

    s_esp32_label = build_stat_row(content, "ESP32-S3 die temp", UI_THEME_ACCENT_1);

    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        char name[40];
        snprintf(name, sizeof(name), "MAX31856 ch %u cold-junction", (unsigned)ch);
        lv_color_t accent;
        switch (ch % 5) {
        case 0: accent = UI_THEME_ACCENT_2; break;
        case 1: accent = UI_THEME_ACCENT_3; break;
        case 2: accent = UI_THEME_ACCENT_4; break;
        case 3: accent = UI_THEME_ACCENT_1; break;
        default: accent = UI_THEME_ACCENT_2; break;
        }
        s_cj_label[ch] = build_stat_row(content, name, accent);
    }

    ui_topbar_raise(&tb);

    /* Pages are never torn down (kiln_ui.h's header comment) -- same
     * "create once, keep refreshing forever" timer lifetime as
     * ui_page_home.c / ui_page_temperature.c. */
    lv_timer_create(refresh_cb, UI_PAGE_BOARD_HEALTH_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
