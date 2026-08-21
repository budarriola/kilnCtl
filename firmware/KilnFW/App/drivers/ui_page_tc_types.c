#include "ui_page_tc_types.h"

#include <stdio.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "kiln_ui.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "uart_task_ids.h" /* THERMO_TC_* codes */
#include "zones_http.h"

static const char *TAG __attribute__((unused)) = "ui_page_tc_types";

/* Names by register code, matching zones_page.html's TC_TYPES table exactly
 * (same THERMO_TC_B..THERMO_TC_T = 0..7 ordering, uart_task_ids.h). Verified
 * against that table rather than assumed -- this page's whole reason to
 * exist is "show the type by NAME, never a register nibble," so silently
 * drifting from the web page's names would defeat the point even though
 * nothing would fail to compile. Index == register code, THERMO_TC_T (7) is
 * the last real entry -- codes 8-0xF are voltage-input modes and never
 * reach this page (zones_config_set_tc_type() rejects them). */
static const char *tc_type_name(uint8_t code)
{
    static const char *names[] = {
        "Type B", "Type E", "Type J", "Type K",
        "Type N", "Type R", "Type S", "Type T",
    };
    if (code >= sizeof(names) / sizeof(names[0])) {
        return "Unknown"; /* defensive -- getters below only ever hand back 0-7 */
    }
    return names[code];
}

/* One row per physical MAX31856 channel that has a zone configured
 * (zones_config_get_thermo_count()), plus one more for the safety
 * processor's own, independent type -- MAX31856_CHANNEL_COUNT + 1 is the
 * hard ceiling this page ever needs, so the row label arrays are sized to
 * that rather than a magic number. */
#define UI_PAGE_TC_TYPES_MAX_ROWS (MAX31856_CHANNEL_COUNT + 1)

static lv_obj_t *s_row_label[UI_PAGE_TC_TYPES_MAX_ROWS];
static uint8_t s_row_count;

/* Row `UI_PAGE_TC_TYPES_MAX_ROWS - 1` slot (i.e. the last built row) is
 * always the safety-processor row when the page is showing one -- tracked
 * explicitly rather than inferred from position, since the number of
 * per-channel rows above it varies with thermo_count. */
static bool s_has_safety_row;
static uint8_t s_safety_row_index;

static void repaint_channel_row(uint8_t ch)
{
    uint8_t tc_type;
    if (!zones_config_get_tc_type(ch, &tc_type)) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "Channel %u\n%s", (unsigned)ch, tc_type_name(tc_type));
    lv_label_set_text(s_row_label[ch], buf);
}

static void repaint_safety_row(void)
{
    if (!s_has_safety_row) {
        return;
    }
    uint8_t tc_type;
    if (!zones_config_get_safety_tc_type(&tc_type)) {
        return;
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "Safety Processor\n%s", tc_type_name(tc_type));
    lv_label_set_text(s_row_label[s_safety_row_index], buf);
}

/* Tap-to-cycle: the only editable control this page has. Advances one step
 * B->E->J->K->N->R->S->T->B, wrapping at the end (ZONE_TC_TYPE_MAX_REAL in
 * zones_http.c is 7 == THERMO_TC_T, so wrap is "8 mod 8 == 0" == THERMO_TC_B).
 * Chosen over a dropdown/roller: LVGL has no stock 8-item roller that both
 * fits this page's row height AND avoids the "opens a popup that must itself
 * respect the no-scroll budget" complication a full picker would add -- a
 * single tap per step is a few extra taps worst-case (7, B->T) against a
 * meaningfully simpler, guaranteed-no-scroll control. Writes immediately via
 * zones_config_set_tc_type()/_set_safety_tc_type() -- no separate "Save"
 * step, matching ui_page_config.c's units toggle (build_unit_toggle_item())
 * and the general LCD convention that a control this simple commits on tap
 * rather than staging an edit. */
static void channel_row_cb(lv_event_t *e)
{
    uint8_t ch = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t tc_type;
    if (!zones_config_get_tc_type(ch, &tc_type)) {
        return;
    }
    uint8_t next = (uint8_t)((tc_type + 1) % 8);
    if (!zones_config_set_tc_type(ch, next)) {
        ESP_LOGW(TAG, "ch%u: failed to set thermocouple type %u", (unsigned)ch, (unsigned)next);
        return;
    }
    repaint_channel_row(ch);
}

static void safety_row_cb(lv_event_t *e)
{
    (void)e;
    uint8_t tc_type;
    if (!zones_config_get_safety_tc_type(&tc_type)) {
        return;
    }
    uint8_t next = (uint8_t)((tc_type + 1) % 8);
    if (!zones_config_set_safety_tc_type(next)) {
        ESP_LOGW(TAG, "failed to set safety processor thermocouple type %u", (unsigned)next);
        return;
    }
    repaint_safety_row();
}

/* Same card-button shape as ui_page_config.c's build_nav_item()/
 * build_unit_toggle_item() -- full-width here rather than lv_pct(48) since
 * this page's rows are a simple vertical list, not a 2-column grid. */
static lv_obj_t *build_row(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);

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

lv_obj_t *ui_page_tc_types_build(void)
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
        .title = "Thermocouple Types",
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

    /* Every row shares content's ~264px via flex_grow(1), same "rows divide
     * up whatever's left, never a fixed px total that could exceed the
     * budget" approach ui_page_thermo_faults.c's build_channel_row() uses --
     * see that file's comment for why a hard-coded per-row height overflowed
     * the page there. thermo_count is read once at build time: this page is
     * built once and kept (kiln_ui.h -- pages are never torn down), so a
     * config change that alters thermo_count after this page has already
     * been visited would not add/remove a row until reboot. Acceptable here:
     * thermo_count itself only changes via a web zones-page save, which is
     * already documented (zones_http.h) to require reconsidering the whole
     * zone layout, not a casual edit a user does mid-session on the LCD. */
    s_row_count = zones_config_get_thermo_count();
    if (s_row_count > MAX31856_CHANNEL_COUNT) {
        s_row_count = MAX31856_CHANNEL_COUNT; /* defensive; never true in practice */
    }
    for (uint8_t ch = 0; ch < s_row_count; ch++) {
        s_row_label[ch] = build_row(content, channel_row_cb, (void *)(uintptr_t)ch);
        repaint_channel_row(ch);
    }

    s_has_safety_row = true;
    s_safety_row_index = s_row_count;
    s_row_label[s_safety_row_index] = build_row(content, safety_row_cb, NULL);
    repaint_safety_row();

    ui_topbar_raise(&tb);

    return scr;
}
