#include "ui_page_profiles_family.h"

#include "kiln_ui.h"
#include "ui_page_profiles_builtin_list.h"
#include "ui_theme.h"

/* Single-page 2x2 grid, same arithmetic as ui_page_profiles.c's hub:
 *
 *     nav row (Back) ............................ 44px
 *     gap ........................................  4px
 *     grid: 2 rows x 72px + 1 gap ............... 148px
 *                                                 ------
 *                                                 196px  <= 267px  OK
 *
 * Exactly four families -- one page, no paging needed. */
#define GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profiles");
}

static void family_clicked_cb(lv_event_t *e)
{
    const char *family = (const char *)lv_event_get_user_data(e);
    ui_page_profiles_builtin_list_set_family(family);
    kiln_ui_show("profiles_builtin_list");
}

static void build_cell(lv_obj_t *parent, const char *family)
{
    lv_obj_t *cell = lv_button_create(parent);
    lv_obj_set_width(cell, lv_pct(48));
    lv_obj_set_height(cell, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(cell, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(cell, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(cell, family_clicked_cb, LV_EVENT_CLICKED, (void *)family);

    lv_obj_t *label = lv_label_create(cell);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, family);
    lv_obj_center(label);

    lv_obj_update_layout(cell);
    ui_theme_apply_touch_area(cell, true);
}

lv_obj_t *ui_page_profiles_family_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Built-in Schedules");

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* String literals, not copies -- same lifetime as this translation unit,
     * matching profiles_builtin_table.inc's own .family string literals so a
     * pointer comparison-free strcmp() in ui_page_profiles_builtin_list.c
     * always sees identical byte content either way. */
    build_cell(grid, "Bartlett");
    build_cell(grid, "Plainsman");
    build_cell(grid, "Crystalline");
    build_cell(grid, "General");

    lv_obj_t *nav_row = lv_obj_create(scr);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 44);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_remove_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(nav_row);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(back, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_update_layout(back);
    ui_theme_apply_touch_area(back, false);

    return scr;
}
