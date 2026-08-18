#include "ui_page_config.h"

#include "kiln_ui.h"
#include "ui_theme.h"

// TODO.md 10.3's "Configuration" nav item destination: a navigation hub,
// deliberately not a full settings editor -- that is much bigger scope than
// this pass (see zones_http.c/rules_http.c/wifi_provision_http.c's web pages
// for the size of what each destination would eventually need to become on
// the LCD). Replaces the pre-this-pass title+Back-only stub.
//
// Four list items, matching section 3's Settings pages plus TODO.md 10.7's
// board-health surface:
//   - Zones & Thermocouples, Relays & Rules, Network/Wi-Fi -- all three
//     still "not built yet" placeholders this pass (non-clickable list
//     rows, dimmed text) rather than dead navigation to a page that doesn't
//     exist yet or a half-built editor. Building a real one of these (e.g.
//     zone name/thermo_mask editing, since zones_http.h already exposes the
//     getters/setters) is reasonable future scope but was not attempted
//     this pass in favor of a working hub plus one other real page
//     (Temperature, see ui_page_temperature.c) and the new Board Health
//     page below.
//   - Board Health -- real navigation to ui_page_board_health.c/.h, new
//     this pass, itself reading board_temps_get_live() (extracted from
//     board_temps.c's HTTP handler, TODO.md 10.1a).
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("home");
}

static void board_health_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("board_health");
}

/* A real, clickable nav row (cb non-NULL) or an honest "not built yet"
 * placeholder row (cb NULL -- not clickable, dimmed text) -- see this
 * file's header comment for which of the four items on this page are
 * which. */
static void build_nav_item(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    } else {
        /* Placeholder destination: looks like a row in the same list, but
         * is not a tappable dead end -- TODO.md 10.3's honest-placeholder
         * rule, same spirit as the pre-this-pass stub pages' "(not built
         * yet)" text, just without navigating anywhere at all this time. */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, cb ? UI_THEME_COLOR_TEXT_PRIMARY : UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    /* TODO.md 10.4's touch hit-area helper -- a vertical settings list is
     * the "dense grid / settings list row" compact_layout=true case
     * ui_theme.h's doc comment names explicitly. Skipped for a
     * non-clickable placeholder row -- nothing to size a hit-area for. */
    if (cb) {
        lv_obj_update_layout(row);
        ui_theme_apply_touch_area(row, true);
    }
}

lv_obj_t *ui_page_config_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX, 0);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, UI_THEME_STATUS_BAR_HEIGHT_PX);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *title = lv_label_create(bar);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Configuration");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX, 0);

    build_nav_item(content, "Zones & Thermocouples (not built yet)", NULL);
    build_nav_item(content, "Relays & Rules (not built yet)", NULL);
    build_nav_item(content, "Network / Wi-Fi (not built yet)", NULL);
    build_nav_item(content, "Board Health", board_health_nav_cb);

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(back, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_update_layout(back);
    ui_theme_apply_touch_area(back, false); /* sparse -- one button on its own row */

    return scr;
}
