#include "ui_page_config.h"

#include "kiln_ui.h"
#include "ui_theme.h"

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("home");
}

lv_obj_t *ui_page_config_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX * 2, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Configuration");

    lv_obj_t *note = lv_label_create(scr);
    lv_obj_set_style_text_color(note, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(note, "Thermocouples & Zones / Relays & Rules / Network\n(not built yet)");
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(back, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    ui_theme_apply_touch_area(back, false); /* TODO.md 10.4 helper -- explicit size, no layout wait needed */

    return scr;
}
