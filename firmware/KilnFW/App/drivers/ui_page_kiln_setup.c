#include "ui_page_kiln_setup.h"

#include "kiln_ui.h"
#include "ui_theme.h"
#include "ui_topbar.h"

// Two-cell hub, same shape as ui_page_profiles.c's 2x2 (here 2x1) grid --
// see that file's own header comment for the arithmetic style this copies.
//
// WHY A HUB CELL POINTS BACK AT THE EXISTING "profiles" TREE RATHER THAN
// BUILDING A NEW PICKER: the user's request was "i also want to be able to
// save kiln profiles with different relay, thermocouple, and pid configs" --
// investigating the home page found it had NO firing-schedule picker at all
// (removed for the no-scroll rule, see ui_page_home.c's header comment), and
// the user then asked for a page holding BOTH a firing-profile picker and a
// kiln-config picker. But ui_page_profiles.c (browse My Profiles / Built-ins,
// per-profile detail with feasibility + START) already exists and already IS
// a full browse/pick/start experience -- rebuilding a second, narrower picker
// here would be a duplicate implementation of the same capability, not a
// second feature. This cell is the bridge: it satisfies "put a firing-profile
// selector on this page" by routing to the real one instead of a shadow copy.
//
// NAMING: "Kiln Config" (never "profile" -- see kiln_cfg_store.h's header
// comment for why the two concepts must stay verbally distinct on this
// screen).
#define UI_PAGE_KILN_SETUP_GRID_HEIGHT_PX UI_THEME_MIN_TOUCH_TARGET_PX

static void firing_profile_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profiles");
}

static void kiln_cfg_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("kiln_cfg_setup");
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

lv_obj_t *ui_page_kiln_setup_build(void)
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
        .title = "Kiln Setup",
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

    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, UI_PAGE_KILN_SETUP_GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    build_nav_item(grid, "Firing Profile", firing_profile_nav_cb);
    build_nav_item(grid, "Kiln Config", kiln_cfg_nav_cb);

    ui_topbar_raise(&tb);

    return scr;
}
