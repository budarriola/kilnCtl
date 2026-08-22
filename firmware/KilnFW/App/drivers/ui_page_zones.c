#include "ui_page_zones.h"

#include <stdio.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "ui_num_pad.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "zones_http.h"

static const char *TAG __attribute__((unused)) = "ui_page_zones";

/* ---------------------------------------------------------------------
 * "zones" -- the list page. One row per configured zone
 * (zones_config_get_thermo_count()), same "rows divide up whatever's left"
 * flex_grow(1) approach ui_page_tc_types.c's build_row() uses, for the same
 * reason: a fixed per-row px total would have to be re-derived by hand every
 * time thermo_count changes, where flex_grow(1) never overflows the ~267px
 * content budget regardless of count (MAX31856_CHANNEL_COUNT is a small,
 * hardware-fixed ceiling either way). thermo_count is read once at build
 * time -- same accepted staleness ui_page_tc_types.c's own comment documents
 * (a web-page zone-layout change takes effect on next visit/reboot, not
 * live), for the identical reason: it changes only via a web zones-page
 * save, never a casual LCD-session event.
 * ------------------------------------------------------------------- */

#define UI_PAGE_ZONES_MAX_ROWS MAX31856_CHANNEL_COUNT

static lv_obj_t *s_list_row_label[UI_PAGE_ZONES_MAX_ROWS];
static uint8_t s_list_row_count;

static void zone_row_cb(lv_event_t *e)
{
    uint8_t zi = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    ui_page_zones_detail_prepare(zi);
    kiln_ui_show("zones_detail");
}

static void repaint_list_row(uint8_t zi)
{
    char name[16];
    char buf[40];
    if (zones_config_get_name(zi, name, sizeof(name)) && name[0]) {
        snprintf(buf, sizeof(buf), "Zone %u\n%s", (unsigned)zi, name);
    } else {
        snprintf(buf, sizeof(buf), "Zone %u", (unsigned)zi);
    }
    lv_label_set_text(s_list_row_label[zi], buf);
}

static lv_obj_t *build_list_row(lv_obj_t *parent, uint8_t zi)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(row, zone_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)zi);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, "--");
    lv_obj_center(label);

    lv_obj_update_layout(row);
    ui_theme_apply_touch_area(row, true);
    return label;
}

lv_obj_t *ui_page_zones_build(void)
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
        .title = "Zones & Thermocouples",
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

    s_list_row_count = zones_config_get_thermo_count();
    if (s_list_row_count > MAX31856_CHANNEL_COUNT) {
        s_list_row_count = MAX31856_CHANNEL_COUNT; /* defensive; never true in practice */
    }

    if (s_list_row_count == 0) {
        /* No zones configured yet -- honest empty state rather than a blank
         * page. Non-clickable, same "looks like a cell but is not a tappable
         * dead end" convention ui_page_config.c's build_nav_item() uses for
         * its own placeholder cells. */
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(label, "No zones configured yet.\nUse Settings > Zones & Thermocouples\non the web UI first.");
        lv_obj_center(label);
    } else {
        for (uint8_t zi = 0; zi < s_list_row_count; zi++) {
            s_list_row_label[zi] = build_list_row(content, zi);
            repaint_list_row(zi);
        }
    }

    ui_topbar_raise(&tb);

    return scr;
}

/* ---------------------------------------------------------------------
 * "zones_detail" -- the per-zone editor, paged (Assignment;
 * Calibration & Limits) via the shared top bar's Prev/Next, exactly the
 * pattern ui_page_config.c's hub uses for ITS paging (hub_show_page(),
 * ui_topbar_set_prev_enabled()/set_next_enabled()) -- see that file's
 * "Paged hub" comment for the arithmetic this mirrors.
 *
 * Sub-page 0, Assignment -- thermo_mask (which MAX31856 channels feed this
 * zone's control temperature) and relay_mask (which relays this zone
 * drives), both as tap-to-toggle chip rows identical in spirit to
 * ui_page_profile_builder_zones.c's zone_mask chip row (same toggle-a-bit,
 * repaint-one-chip shape), against the real content budget:
 *
 *     "Thermocouples (tap to toggle):" label ... ~18px
 *     gap ..........................................  4px
 *     thermo chip row ............................... 44px
 *     gap ..........................................  4px
 *     "Relays (tap to toggle):" label .............. ~18px
 *     gap ..........................................  4px
 *     relay chip row ................................ 44px
 *                                                    ------
 *                                                     136px  <= ~267px  OK
 *
 * Sub-page 1, Calibration & Limits -- three tap-to-edit rows (cal_offset_c,
 * max_temp_c, min_temp_c), each opening ui_num_pad.c the same way
 * ui_page_profile_builder_segment.c's target/ramp/dwell cards do:
 *
 *     cal offset row ................................. 44px
 *     gap ..........................................  4px
 *     max temp row ................................... 44px
 *     gap ..........................................  4px
 *     min temp row ................................... 44px
 *                                                    ------
 *                                                     140px  <= ~267px  OK
 *
 * Both sub-pages are built once, up front, and toggled with
 * LV_OBJ_FLAG_HIDDEN -- kiln_ui.c never tears a page down, matching every
 * other paged page in this codebase (ui_page_config.c's hub).
 *
 * EDITABLE TEMPERATURE C/F DECISION: cal_offset_c, max_temp_c, and
 * min_temp_c stay in CELSIUS end to end -- caption, bounds (the ZONE_*_C
 * macros in zones_http.h, which are themselves Celsius), the value shown in
 * ui_num_pad, and the value written back through the zones_config_set_*()
 * setters. This is the SAME documented convention
 * ui_page_profile_builder_segment.c's target/ramp cards use (see that
 * file's 2026-08-21 "LCD item 2" comment) for the identical reason:
 * ui_num_pad_params_t has no partial-conversion mode, so correctly
 * F-enabling an editable field means converting min/max/initial_value AND
 * converting the typed-in-Fahrenheit result back to Celsius before it
 * reaches a setter that stores/guards a real kiln limit -- getting any one
 * of those three wrong is a safety hazard on a stored limit, not a cosmetic
 * bug, so this pass follows the established "if in doubt, Celsius with a
 * Celsius label" rule rather than half-converting. Row captions say "(C)"
 * explicitly so this is never ambiguous on screen regardless of the
 * board's unit_pref.
 * ------------------------------------------------------------------- */

#define UI_ZONES_DETAIL_PAGE_COUNT 2

static uint8_t s_zone_index;
static uint8_t s_detail_page;
static bool s_detail_built;

static ui_topbar_t s_detail_tb;
static lv_obj_t *s_detail_subpage[UI_ZONES_DETAIL_PAGE_COUNT];

static lv_obj_t *s_thermo_chips[MAX31856_CHANNEL_COUNT];
static lv_obj_t *s_relay_chips[KILN_IO_RELAY_COUNT];

static lv_obj_t *s_cal_label;
static lv_obj_t *s_max_temp_label;
static lv_obj_t *s_min_temp_label;

static void set_detail_title(void)
{
    char name[16];
    char title[40];
    if (zones_config_get_name(s_zone_index, name, sizeof(name)) && name[0]) {
        snprintf(title, sizeof(title), "Zone %u (%s)  %u of %u", (unsigned)s_zone_index, name,
                 (unsigned)(s_detail_page + 1), (unsigned)UI_ZONES_DETAIL_PAGE_COUNT);
    } else {
        snprintf(title, sizeof(title), "Zone %u  %u of %u", (unsigned)s_zone_index,
                 (unsigned)(s_detail_page + 1), (unsigned)UI_ZONES_DETAIL_PAGE_COUNT);
    }
    ui_topbar_set_title(&s_detail_tb, title);
}

static void show_detail_subpage(uint8_t index)
{
    if (index >= UI_ZONES_DETAIL_PAGE_COUNT) {
        return;
    }
    s_detail_page = index;
    for (uint8_t i = 0; i < UI_ZONES_DETAIL_PAGE_COUNT; i++) {
        if (!s_detail_subpage[i]) {
            continue;
        }
        if (i == index) {
            lv_obj_remove_flag(s_detail_subpage[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_detail_subpage[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    set_detail_title();
    ui_topbar_set_prev_enabled(&s_detail_tb, index > 0);
    ui_topbar_set_next_enabled(&s_detail_tb, index + 1 < UI_ZONES_DETAIL_PAGE_COUNT);
}

static void detail_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_page > 0) {
        show_detail_subpage((uint8_t)(s_detail_page - 1));
        kiln_ui_log_tap_targets();
    }
}

static void detail_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_page + 1 < UI_ZONES_DETAIL_PAGE_COUNT) {
        show_detail_subpage((uint8_t)(s_detail_page + 1));
        kiln_ui_log_tap_targets();
    }
}

/* --- Sub-page 0: Assignment -------------------------------------------- */

static void repaint_thermo_chip(uint8_t ch)
{
    if (!s_thermo_chips[ch]) {
        return;
    }
    uint8_t mask = 0;
    zones_config_get_thermo_mask(s_zone_index, &mask);
    bool active = (mask & (1u << ch)) != 0;
    lv_obj_set_style_bg_color(s_thermo_chips[ch], active ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
}

static void thermo_chip_cb(lv_event_t *e)
{
    uint8_t ch = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t mask = 0;
    if (!zones_config_get_thermo_mask(s_zone_index, &mask)) {
        return;
    }
    mask ^= (uint8_t)(1u << ch);
    if (!zones_config_set_thermo_mask(s_zone_index, mask)) {
        ESP_LOGW(TAG, "zone %u: failed to set thermo_mask 0x%02x", (unsigned)s_zone_index, (unsigned)mask);
        return;
    }
    repaint_thermo_chip(ch);
}

static void repaint_relay_chip(uint8_t ri)
{
    if (!s_relay_chips[ri]) {
        return;
    }
    uint8_t mask = 0;
    zones_config_get_relay_mask(s_zone_index, &mask);
    bool active = (mask & (1u << ri)) != 0;
    lv_obj_set_style_bg_color(s_relay_chips[ri], active ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
}

static void relay_chip_cb(lv_event_t *e)
{
    uint8_t ri = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(s_zone_index, &mask)) {
        return;
    }
    mask ^= (uint8_t)(1u << ri);
    if (!zones_config_set_relay_mask(s_zone_index, mask)) {
        ESP_LOGW(TAG, "zone %u: failed to set relay_mask 0x%02x", (unsigned)s_zone_index, (unsigned)mask);
        return;
    }
    repaint_relay_chip(ri);
}

static void refresh_assignment_subpage(void)
{
    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t ch = 0; ch < thermo_count; ch++) {
        repaint_thermo_chip(ch);
    }
    uint8_t relay_count = zones_config_get_relay_count();
    if (relay_count > KILN_IO_RELAY_COUNT) {
        relay_count = KILN_IO_RELAY_COUNT;
    }
    for (uint8_t ri = 0; ri < relay_count; ri++) {
        repaint_relay_chip(ri);
    }
}

static lv_obj_t *build_chip_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *build_assignment_subpage(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(page, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *thermo_label = lv_label_create(page);
    lv_obj_set_style_text_color(thermo_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(thermo_label, "Thermocouples (tap to toggle):");

    lv_obj_t *thermo_row = build_chip_row(page);
    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t ch = 0; ch < thermo_count; ch++) {
        lv_obj_t *chip = lv_button_create(thermo_row);
        lv_obj_set_height(chip, 44);
        lv_obj_set_flex_grow(chip, 1);
        lv_obj_set_style_bg_color(chip, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(chip, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_add_event_cb(chip, thermo_chip_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)ch);
        s_thermo_chips[ch] = chip;

        char buf[8];
        snprintf(buf, sizeof(buf), "Ch %u", (unsigned)ch);
        lv_obj_t *label = lv_label_create(chip);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(label, buf);
        lv_obj_center(label);
        lv_obj_update_layout(chip);
        ui_theme_apply_touch_area(chip, true);
    }

    lv_obj_t *relay_label = lv_label_create(page);
    lv_obj_set_style_text_color(relay_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(relay_label, "Relays (tap to toggle):");

    lv_obj_t *relay_row = build_chip_row(page);
    uint8_t relay_count = zones_config_get_relay_count();
    if (relay_count > KILN_IO_RELAY_COUNT) {
        relay_count = KILN_IO_RELAY_COUNT;
    }
    for (uint8_t ri = 0; ri < relay_count; ri++) {
        lv_obj_t *chip = lv_button_create(relay_row);
        lv_obj_set_height(chip, 44);
        lv_obj_set_flex_grow(chip, 1);
        lv_obj_set_style_bg_color(chip, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(chip, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_add_event_cb(chip, relay_chip_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)ri);
        s_relay_chips[ri] = chip;

        char buf[10];
        snprintf(buf, sizeof(buf), "Relay %u", (unsigned)(ri + 1));
        lv_obj_t *label = lv_label_create(chip);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(label, buf);
        lv_obj_center(label);
        lv_obj_update_layout(chip);
        ui_theme_apply_touch_area(chip, true);
    }

    return page;
}

/* --- Sub-page 1: Calibration & Limits ----------------------------------- */

static void repaint_cal_limit_labels(void)
{
    float cal_c = 0.0f;
    zones_config_get_cal_offset(s_zone_index, &cal_c);
    char buf[40];
    if (s_cal_label) {
        snprintf(buf, sizeof(buf), "Cal Offset (C): %.1f\n(tap to edit)", (double)cal_c);
        lv_label_set_text(s_cal_label, buf);
    }

    float max_c = 0.0f, min_c = 0.0f;
    zones_config_get_temp_limits(s_zone_index, &max_c, &min_c);
    if (s_max_temp_label) {
        if (max_c == 0.0f) {
            snprintf(buf, sizeof(buf), "Max Temp (C): none\n(tap to edit)");
        } else {
            snprintf(buf, sizeof(buf), "Max Temp (C): %.0f\n(tap to edit)", (double)max_c);
        }
        lv_label_set_text(s_max_temp_label, buf);
    }
    if (s_min_temp_label) {
        snprintf(buf, sizeof(buf), "Min Temp (C): %.0f\n(tap to edit)", (double)min_c);
        lv_label_set_text(s_min_temp_label, buf);
    }
}

static void cal_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)text;
    (void)user_data;
    if (!accepted) {
        return;
    }
    if (!zones_config_set_cal_offset(s_zone_index, value)) {
        ESP_LOGW(TAG, "zone %u: failed to set cal_offset_c %.1f", (unsigned)s_zone_index, (double)value);
        return;
    }
    repaint_cal_limit_labels();
}

static void cal_row_cb(lv_event_t *e)
{
    (void)e;
    float cal_c = 0.0f;
    zones_config_get_cal_offset(s_zone_index, &cal_c);
    ui_num_pad_params_t params = {
        .caption = "Cal Offset (C)",
        .mode = UI_NUM_PAD_MODE_NUMBER,
        .initial_value = cal_c,
        .min = ZONE_CAL_OFFSET_MIN_C,
        .max = ZONE_CAL_OFFSET_MAX_C,
        .decimals = 1,
        .on_done = cal_done_cb,
    };
    ui_num_pad_show(&params);
}

/* max_temp_c/min_temp_c share one setter (zones_config_set_temp_limits())
 * that bundles both fields -- see its own doc comment for why (a rejected
 * call must not leave one half updated). Each row's done_cb therefore reads
 * the OTHER field back out before writing, rather than caching a stale
 * value locally that could drift from what the setter actually persisted. */
static void max_temp_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)text;
    (void)user_data;
    if (!accepted) {
        return;
    }
    float cur_max_c = 0.0f, cur_min_c = 0.0f;
    zones_config_get_temp_limits(s_zone_index, &cur_max_c, &cur_min_c);
    if (!zones_config_set_temp_limits(s_zone_index, value, cur_min_c)) {
        ESP_LOGW(TAG, "zone %u: failed to set max_temp_c %.0f", (unsigned)s_zone_index, (double)value);
        return;
    }
    repaint_cal_limit_labels();
}

static void max_temp_row_cb(lv_event_t *e)
{
    (void)e;
    float max_c = 0.0f, min_c = 0.0f;
    zones_config_get_temp_limits(s_zone_index, &max_c, &min_c);
    ui_num_pad_params_t params = {
        .caption = "Max Temp (C, 0 = no ceiling)",
        .mode = UI_NUM_PAD_MODE_NUMBER,
        .initial_value = max_c,
        .min = 0.0f,
        .max = ZONE_MAX_TEMP_C_MAX,
        .decimals = 0,
        .on_done = max_temp_done_cb,
    };
    ui_num_pad_show(&params);
}

static void min_temp_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)text;
    (void)user_data;
    if (!accepted) {
        return;
    }
    float cur_max_c = 0.0f, cur_min_c = 0.0f;
    zones_config_get_temp_limits(s_zone_index, &cur_max_c, &cur_min_c);
    if (!zones_config_set_temp_limits(s_zone_index, cur_max_c, value)) {
        ESP_LOGW(TAG, "zone %u: failed to set min_temp_c %.0f", (unsigned)s_zone_index, (double)value);
        return;
    }
    repaint_cal_limit_labels();
}

static void min_temp_row_cb(lv_event_t *e)
{
    (void)e;
    float max_c = 0.0f, min_c = 0.0f;
    zones_config_get_temp_limits(s_zone_index, &max_c, &min_c);
    ui_num_pad_params_t params = {
        .caption = "Min Temp (C)",
        .mode = UI_NUM_PAD_MODE_NUMBER,
        .initial_value = min_c,
        .min = ZONE_MIN_TEMP_C_MIN,
        .max = ZONE_MIN_TEMP_C_MAX,
        .decimals = 0,
        .on_done = min_temp_done_cb,
    };
    ui_num_pad_show(&params);
}

static lv_obj_t *build_tap_row(lv_obj_t *parent, lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, "--");
    lv_obj_center(label);
    lv_obj_update_layout(row);
    ui_theme_apply_touch_area(row, false);

    *out_label = label;
    return row;
}

static lv_obj_t *build_cal_limits_subpage(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(page, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    build_tap_row(page, cal_row_cb, &s_cal_label);
    build_tap_row(page, max_temp_row_cb, &s_max_temp_label);
    build_tap_row(page, min_temp_row_cb, &s_min_temp_label);

    return page;
}

void ui_page_zones_detail_prepare(uint8_t zone_index)
{
    s_zone_index = zone_index;
    if (s_detail_built) {
        refresh_assignment_subpage();
        repaint_cal_limit_labels();
        show_detail_subpage(0);
    }
}

lv_obj_t *ui_page_zones_detail_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Zone",
        .back_page = "zones",
        .show_home = true,
        .prev_cb = detail_prev_cb,
        .next_cb = detail_next_cb,
    }, &s_detail_tb);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_subpage[0] = build_assignment_subpage(content);
    s_detail_subpage[1] = build_cal_limits_subpage(content);

    ui_topbar_raise(&s_detail_tb);

    s_detail_built = true;
    refresh_assignment_subpage();
    repaint_cal_limit_labels();
    show_detail_subpage(0);

    return scr;
}
