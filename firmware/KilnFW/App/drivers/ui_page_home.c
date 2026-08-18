#include "ui_page_home.h"

#include "ui_theme.h"

/* Deliberately still the pre-10.2 placeholder content (a centered label on a
 * plain background) -- only the colors changed, to ui_theme.h's palette
 * instead of the old hardcoded lv_color_black()/lv_color_white(). The real
 * content (per-zone temp/heater status, profile graph, time remaining,
 * start/stop, nav to Configuration/Temperature -- TODO.md 10.3's list) is
 * still unbuilt; this is the smoke-test screen kiln_ui_init() shows so
 * there's always something on the glass. */
lv_obj_t *ui_page_home_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, "kilnCtl");
    lv_obj_center(label);

    return scr;
}
