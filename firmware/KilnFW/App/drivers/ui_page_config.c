#include "ui_page_config.h"

#include "kiln_ui.h"
#include "ui_theme.h"

// TODO.md 10.3's "Configuration" nav item destination: a navigation hub,
// deliberately not a full settings editor -- that is much bigger scope than
// this pass (see zones_http.c/rules_http.c/wifi_provision_http.c's web pages
// for the size of what each destination would eventually need to become on
// the LCD).
//
// 2026-08-18 no-scroll rewrite: this hub grew from 4 to 7 destinations this
// pass (Temperature moved here from ui_page_home.c's old dedicated nav
// button, plus two brand-new pages -- Safety Processor and Temperature
// History -- that used to be cards on ui_page_home.c before that page's
// content was re-budgeted to fit without scrolling; see ui_page_home.c's
// header comment). Seven rows at the old single-column,
// UI_THEME_MIN_TOUCH_TARGET_PX-tall (72px) layout plus a Back button would
// have needed roughly 630px of vertical space against this page's real
// ~264px content budget (480x320 landscape, this codebase's actual runtime
// canvas) -- nowhere close. This pass switches the nav list from a single
// column to a 2-column flex-wrap grid at a shorter row height (44px,
// compact_layout=true's "dense grid" case ui_theme.h's own doc comment
// names explicitly), computed to fit 7 rows (4 grid rows at 2-per-row) plus
// a separate full-width Back row comfortably inside the budget -- see the
// row-count arithmetic in build_nav_item()'s caller below.
//
// 2026-08-19 touch-target audit (UI_PLAN.md section 3, LCD item 5): this
// pass's own comment above never actually checked a per-cell px number
// against UI_THEME_MIN_TOUCH_TARGET_PX, as UI_PLAN.md flagged. Checked now,
// against ui_theme.c's real ui_theme_apply_touch_area() code (not assumed):
// cell WIDTH is fine on its own -- lv_pct(48) of the ~464px-wide grid
// (480px panel minus 2*UI_THEME_PADDING_PX scr padding) is ~223px, far past
// 72px. Cell HEIGHT was the real gap: the old 44px-drawn cell only got
// compact_layout=true's small capped extension (UI_THEME_PADDING_PX/2 = 4px
// per side, per ui_theme.c's own comment on why compact mode is capped that
// low -- avoiding two dense cells' expanded boxes overlapping into the
// shared gap) -- NOT the "extend until it reaches 72px" behavior that only
// applies to compact_layout=false. 44 + 2*4 = 52px effective, genuinely
// short of the 72px minimum, and raising the drawn row height enough to
// close that gap for all 4 grid rows (would need roughly +20px x 4 rows =
// +80px) blows straight through the ~264px content budget the row-count
// arithmetic above was built against -- a real, budget-locked conflict, not
// an oversight. Fixed the same way LCD item 2 fixed ui_page_temperature.c's
// analogous conflict: `grid` below is now a fixed-height, internally
// scrollable container (own comment on `grid`'s build below has the
// arithmetic) holding real UI_THEME_MIN_TOUCH_TARGET_PX-tall (72px) cells,
// rather than shrinking every cell to fit all 8 in the visible budget at
// once. Every cell now genuinely meets the 72px minimum on both edges; not
// every cell is on-screen without a scroll gesture, which is the same
// tradeoff ui_page_network.c's Scan/Saved lists and
// ui_page_temperature.c's relay grid already make.
//
// Eight items (Touch Calibration added after this pass, see
// ui_page_touch_cal.c/.h -- lands on the same 4-row grid with no height
// regression since 8 is still an even row count):
//   - Zones & Thermocouples, Relays & Rules -- still "not built yet"
//     placeholders (non-clickable, dimmed text), unchanged from before this
//     pass.
//   - Temperature -- real navigation to ui_page_temperature.c/.h (manual
//     relay control), moved here from ui_page_home.c's old dedicated nav
//     button (see ui_page_home.c's header comment).
//   - Board Health -- real navigation to ui_page_board_health.c/.h
//     (TODO.md 10.7).
//   - Network / Wi-Fi -- real navigation to ui_page_network.c/.h
//     (TODO.md 10.9).
//   - Safety Processor -- real navigation to the new ui_page_safety.c/.h
//     (ROADMAP.md M6's card, moved off ui_page_home.c this pass).
//   - Temperature History -- real navigation to the new
//     ui_page_history.c/.h (TODO.md 10.3's chart, moved off
//     ui_page_home.c this pass).
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("home");
}

static void temperature_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("temperature");
}

static void board_health_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("board_health");
}

static void network_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("network");
}

static void safety_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("safety");
}

static void history_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("history");
}

static void touch_cal_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("touch_cal");
}

/* A real, clickable nav cell (cb non-NULL) or an honest "not built yet"
 * placeholder cell (cb NULL -- not clickable, dimmed text). `parent` is a
 * FLEX_FLOW_ROW_WRAP container -- each cell claims lv_pct(48) width so two
 * fit per row with a gap between (see this file's header comment for the
 * row-count arithmetic this depends on). */
static void build_nav_item(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, lv_pct(48));
    /* Real UI_THEME_MIN_TOUCH_TARGET_PX height, not the old 44px -- see this
     * file's 2026-08-19 header comment (LCD item 5). `parent` is now a
     * fixed-height scrollable container so this no longer costs page-level
     * budget the way growing every cell in place would have. */
    lv_obj_set_height(row, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    } else {
        /* Placeholder destination: looks like a cell in the same grid, but
         * is not a tappable dead end. */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, cb ? UI_THEME_COLOR_TEXT_PRIMARY : UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    /* TODO.md 10.4's touch hit-area helper -- a wrapped grid of nav cells is
     * exactly the "dense grid / settings list row" compact_layout=true case
     * ui_theme.h's doc comment names explicitly; it also extends an
     * undersized cell's effective click area toward
     * UI_THEME_MIN_TOUCH_TARGET_PX, which is why 44px-tall cells are still a
     * reasonable touch target despite being shorter than that constant.
     * Skipped for a non-clickable placeholder cell -- nothing to size a hit
     * area for. */
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
    lv_label_set_text(title, "Configuration");
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

    /* 2-column wrap grid, fixed-height and internally scrollable -- see this
     * file's 2026-08-19 header comment (LCD item 5). Budget: content's own
     * ~264px minus the Back row below (72px + UI_THEME_PADDING_PX/2 gap =
     * ~76px) leaves ~188px for `grid`; 200px would already be tight against
     * that, so this uses a deliberately conservative 180px, well inside the
     * remaining room even accounting for the bar/content gap above. 180px
     * shows a bit over 2 of the 4 real 72px-tall rows at once (2*72 +
     * 1*UI_THEME_PADDING_PX/2 gap = 148px, plus a peek of row 3) -- same
     * "not everything visible without a scroll gesture, but real touch
     * targets and a bounded page height" tradeoff as
     * ui_page_temperature.c's relay_row (LCD item 2). */
    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, 180);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, UI_THEME_PADDING_PX / 2, 0);

    build_nav_item(grid, "Zones & Thermocouples (not built yet)", NULL);
    build_nav_item(grid, "Relays & Rules (not built yet)", NULL);
    build_nav_item(grid, "Temperature", temperature_nav_cb);
    build_nav_item(grid, "Network / Wi-Fi", network_nav_cb);
    build_nav_item(grid, "Board Health", board_health_nav_cb);
    build_nav_item(grid, "Safety Processor", safety_nav_cb);
    build_nav_item(grid, "Temperature History", history_nav_cb);
    build_nav_item(grid, "Touch Calibration", touch_cal_nav_cb);

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX * 2, 44);
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
