#include "ui_page_profiles_family.h"

#include <stdint.h>

#include "kiln_ui.h"
#include "profiles_builtin.h"
#include "ui_page_profiles_builtin_list.h"
#include "ui_theme.h"
#include "ui_topbar.h"

/* Single-page grid, same arithmetic as ui_page_profiles.c's hub:
 *
 *     grid: 2 rows x 72px + 1 gap ............... 148px
 *                                                 ------
 *                                                 148px  <= 267px  OK
 *
 * The nav row's Back button moved into the shared top bar (ui_topbar.c) in
 * the 2026-08-21 icon-topbar pass, freeing the 44px + 4px gap it used to
 * cost here.
 *
 * Exactly three firing types (Bisque/Glaze/Other) -- one page, no paging
 * needed; they wrap 2-per-row same as the old 4-family grid, just with the
 * last row holding one cell instead of two. */
#define GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static void firing_type_clicked_cb(lv_event_t *e)
{
    profile_firing_type_t type = (profile_firing_type_t)(uintptr_t)lv_event_get_user_data(e);
    ui_page_profiles_builtin_list_set_firing_type(type);
    kiln_ui_show("profiles_builtin_list");
}

static void build_cell(lv_obj_t *parent, profile_firing_type_t type)
{
    lv_obj_t *cell = lv_button_create(parent);
    lv_obj_set_width(cell, lv_pct(48));
    lv_obj_set_height(cell, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(cell, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(cell, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(cell, firing_type_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)type);

    lv_obj_t *label = lv_label_create(cell);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, profiles_builtin_firing_type_label(type));
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

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Built-in Schedules",
        .back_page = "profiles",
        .show_home = true,
    }, &tb);

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    build_cell(grid, PROFILE_FIRING_BISQUE);
    build_cell(grid, PROFILE_FIRING_GLAZE);
    build_cell(grid, PROFILE_FIRING_OTHER);

    ui_topbar_raise(&tb);

    return scr;
}
