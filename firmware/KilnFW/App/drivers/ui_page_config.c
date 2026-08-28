#include "ui_page_config.h"

#include <stdio.h>

#include "esp_log.h"

#include "kiln_ui.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"

static const char *TAG __attribute__((unused)) = "ui_page_config";

// TODO.md 10.3's "Configuration" nav item destination: a navigation hub,
// deliberately not a full settings editor -- that is much bigger scope than
// this pass (see zones_http.c/rules_http.c/wifi_provision_http.c's web pages
// for the size of what each destination would eventually need to become on
// the LCD).
//
// 2026-08-27, THREE OWNER REQUESTS LANDED TOGETHER, and this file's whole
// shape changed because of them:
//
//   (b)/(d) "the diagnostics pages should also contain the safty processor
//   page, the board health, and thermocouple fault page's info. remove the
//   other 3 lcd pages when you combine the info" / "remove the kiln setup
//   and thermocouple types pages from the lcd includeing the kiln config
//   page" -- six of this hub's eleven destinations are gone: Board Health,
//   Safety Processor, and Thermocouple Faults folded into "diagnostics"
//   itself (see ui_page_diagnostics.c's header comment for the per-page
//   content inventory and where each fact landed -- nothing here duplicates
//   that accounting), and Kiln Setup / Kiln Config / Thermocouple Types are
//   deleted outright, no destination replaces them. kiln_ui.c's page
//   registry table lost all six ("board_health", "safety", "thermo_faults",
//   "kiln_setup", "kiln_cfg_setup", "tc_types") in the same pass -- see that
//   file's own comment.
//
//   (c) "move the profiles menu item to the top left of the first page and
//   scale all of the menu items to in the main menu to fit on one page" --
//   what's left after (b)/(d)'s six removals is exactly six destinations:
//   Profiles, Temperature, Network/Wi-Fi, Touch Calibration, Diagnostics,
//   and the units toggle. Six is exactly this grid's one-page capacity (see
//   UI_CONFIG_HUB_ROWS/UI_CONFIG_HUB_COLS below), which is what makes "fit
//   on one page" possible without shrinking anything below its existing
//   touch-target size -- this request could not have been done honestly
//   before (b)/(d) freed the five slots it needed. The hub is therefore back
//   to a SINGLE, non-paged screen: UI_CONFIG_HUB_PAGE_COUNT's old Prev/Next
//   paging machinery (hub_show_page()/hub_prev_cb()/hub_next_cb(), the
//   "N of M" title) is gone along with the second page it used to page to.
//   The topbar keeps its Back/Home icons but no longer requests Prev/Next.
//
// Grid arithmetic (unchanged 3-row, 2-column, UI_THEME_MIN_TOUCH_TARGET_PX
// (72px) cell shape from the paged-hub era -- see git history for that
// derivation): 3 rows x 72px + 2 x UI_THEME_PADDING_PX/2 (4px) inter-row
// gaps = 224px, comfortably inside the ~267px content budget
// (UI_THEME_PAGE_CONTENT_BUDGET_PX, ui_theme.h) with the same margin the old
// per-page arithmetic always had -- six cells fill the grid exactly, so
// there is no seventh slot to grow into without either a fourth (shorter,
// budget-violating) row or a return to paging.
static void temperature_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("temperature");
}

static void network_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("network");
}

static void touch_cal_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("touch_cal");
}

static void diagnostics_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("diagnostics");
}

static void profiles_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profiles");
}

/* ROADMAP.md 2026-08-21 "a real shared temperature-unit setting": an
 * in-place toggle cell rather than a navigation destination -- see its
 * original header comment (git history) for why this never got its own
 * kiln_ui.c route: it calls unit_pref_set() directly and repaints its own
 * label, so this hub file is the only LCD file the feature needed to touch. */
static lv_obj_t *s_units_cell_label;

static void units_cell_set_label(void)
{
    if (!s_units_cell_label) {
        return;
    }
    char buf[32];
    if (unit_pref_get() == UNIT_PREF_FAHRENHEIT) {
        snprintf(buf, sizeof(buf), "Units: Fahrenheit\n(tap for C)");
    } else {
        snprintf(buf, sizeof(buf), "Units: Celsius\n(tap for F)");
    }
    lv_label_set_text(s_units_cell_label, buf);
}

static void units_toggle_cb(lv_event_t *e)
{
    (void)e;
    unit_pref_t next =
        unit_pref_get() == UNIT_PREF_FAHRENHEIT ? UNIT_PREF_CELSIUS : UNIT_PREF_FAHRENHEIT;
    esp_err_t err = unit_pref_set(next);
    if (err != ESP_OK) {
        /* Live value still took effect (unit_pref_set() updates RAM before
         * attempting the NVS write) -- only persistence failed, same
         * "reported but not treated as user-facing failure" convention
         * dashboard_http.c's unit_pref_post_handler() uses for the same
         * case. */
        ESP_LOGW(TAG, "unit preference applied but not persisted -- will not survive a reboot");
    }
    units_cell_set_label();
}

static ui_topbar_t s_topbar;

#define UI_CONFIG_HUB_GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 3 + (UI_THEME_PADDING_PX / 2) * 2)

/* A real, clickable nav cell -- `parent` is a FLEX_FLOW_ROW_WRAP container,
 * each cell claims lv_pct(48) width so two fit per row with a gap between. */
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

    /* TODO.md 10.4's touch hit-area helper -- a wrapped grid of nav cells is
     * exactly the "dense grid / settings list row" compact_layout=true case
     * ui_theme.h's doc comment names explicitly. */
    lv_obj_update_layout(row);
    ui_theme_apply_touch_area(row, true);
}

/* Same cell shape as build_nav_item(), but its own click handler
 * (units_toggle_cb) instead of a kiln_ui_show() navigation, and it keeps a
 * handle to its label so units_cell_set_label() can repaint it in place. */
static void build_unit_toggle_item(lv_obj_t *parent)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(48));
    lv_obj_set_height(row, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(row, units_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_center(label);

    s_units_cell_label = label;
    units_cell_set_label(); /* paint the real current preference, not a placeholder */

    lv_obj_update_layout(row);
    ui_theme_apply_touch_area(row, true);
}

/* No-op -- see ui_page_config.h's comment: the hub is a single page now
 * (2026-08-27), so there is no paging state left to rewind. Kept so
 * ui_page_home.c's menu_nav_cb() call site needs no change. */
void ui_page_config_reset_to_first_page(void)
{
}

lv_obj_t *ui_page_config_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar -- ui_topbar.c/.h owns the two LVGL traps (hit-test-escape and
     * the FLOATING flex trap); see that header. No Prev/Next any more: this
     * hub is a single page now that (b)/(d) freed enough slots for (c)'s
     * "fit on one page" -- see this file's header comment. */
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Configuration",
        .back_page = "home",
        .show_home = false,
    }, &s_topbar);

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
    lv_obj_set_height(grid, UI_CONFIG_HUB_GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* (c): "move the profiles menu item to the top left of the first page"
     * -- Profiles is the first cell built into a ROW_WRAP grid, which is
     * exactly its top-left slot. Order after that is otherwise the same
     * relative order the old two-page hub used (Temperature, Network,
     * Touch Calibration, Diagnostics, the units toggle), just compacted
     * onto one page now that Board Health/Safety Processor/Thermocouple
     * Faults/Thermocouple Types/Kiln Setup are gone. */
    build_nav_item(grid, "Profiles", profiles_nav_cb);
    build_nav_item(grid, "Temperature", temperature_nav_cb);
    build_nav_item(grid, "Network / Wi-Fi", network_nav_cb);
    build_nav_item(grid, "Touch Calibration", touch_cal_nav_cb);
    build_nav_item(grid, "Diagnostics", diagnostics_nav_cb);
    build_unit_toggle_item(grid);

    /* content is created after the topbar's icon proxy, so without this it
     * would sit above the proxy in z-order and win taps in the overlap
     * region -- see ui_topbar.h's usage note: raise MUST happen after the
     * content area exists. */
    ui_topbar_raise(&s_topbar);

    return scr;
}
