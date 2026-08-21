#include "ui_page_profiles.h"

#include "esp_log.h"

#include "kiln_ui.h"
#include "profiles_builtin.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_page_profiles_mine.h"
#include "ui_theme.h"

static const char *TAG = "ui_page_profiles";

/* Single-page hub, four destinations -- see this file's header. Arithmetic
 * (same style as ui_page_config.c's paged-hub comment, against the same real
 * ~267px content budget that file's header measured on hardware):
 *
 *     nav row (Back) ............................ 44px
 *     gap ........................................  4px
 *     hub grid: 2 rows x 72px + 1 gap ........... 148px
 *                                                 ------
 *                                                  196px  <= 267px  OK
 *
 * Four items exactly fill a single 2x2 grid page -- no paging needed, unlike
 * ui_page_config.c's ten-item hub. "New Profile" (this pass's LCD builder,
 * ui_page_profile_builder_zones.c) is the fourth cell, filling what used to
 * be an empty one -- confirmed there was room before adding it, not assumed. */
#define UI_PAGE_PROFILES_GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("config");
}

static void mine_nav_cb(lv_event_t *e)
{
    (void)e;
    ui_page_profiles_mine_refresh();
    kiln_ui_show("profiles_mine");
}

static void builtins_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profiles_family");
}

static void restore_hidden_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = profiles_builtin_restore_all();
    ESP_LOGI(TAG, "restore hidden builtins: %s", esp_err_to_name(err));
}

static void new_profile_nav_cb(lv_event_t *e)
{
    (void)e;
    ui_page_profile_builder_start_new();
    kiln_ui_show("profile_builder_zones");
}

static void build_nav_item(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(48));
    lv_obj_set_height(row, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    lv_obj_update_layout(row);
    ui_theme_apply_touch_area(row, true);
}

lv_obj_t *ui_page_profiles_build(void)
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
    lv_label_set_text(title, "Profiles");
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

    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, UI_PAGE_PROFILES_GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    build_nav_item(grid, "My Profiles", mine_nav_cb);
    build_nav_item(grid, "Built-ins (28)", builtins_nav_cb);
    build_nav_item(grid, "Restore hidden", restore_hidden_cb);
    build_nav_item(grid, "New Profile", new_profile_nav_cb);

    lv_obj_t *nav_row = lv_obj_create(content);
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
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_update_layout(back);
    ui_theme_apply_touch_area(back, false);

    return scr;
}
