#include "ui_page_config.h"

#include "esp_log.h"

#include "kiln_ui.h"
#include "ui_theme.h"

static const char *TAG = "ui_page_config";

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
// Ten items (Touch Calibration, Diagnostics, and Thermocouple Faults added
// after this pass, see ui_page_touch_cal.c/.h, ui_page_diagnostics.c/.h, and
// ui_page_thermo_faults.c/.h -- all land on the same fixed-height,
// internally scrollable grid LCD work-queue item 5 switched this to, so
// growing the item count costs nothing beyond more scrolling inside `grid`):
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

static void diagnostics_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("diagnostics");
}

static void thermo_faults_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("thermo_faults");
}

static void profiles_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profiles");
}

/* --- Paged hub, 2026-08-20 ------------------------------------------------
 * This hub used to be a single fixed-height container that scrolled
 * internally. Measuring the real post-layout geometry on hardware (the
 * per-page tap-target dump kiln_ui_show() now emits) showed how bad that
 * actually was: the scroll viewport ended at y=223, but the ten cells ran
 * to y=419 on a 320px-tall panel. Six of ten items -- including the
 * Diagnostics and Thermocouple Faults pages -- were unreachable without a
 * scroll gesture, and the two newest pages were the two furthest off-screen.
 *
 * Scrolling is not permitted on this project's LCD pages, so the fix is to
 * page the hub rather than scroll it. Every cell now sits on-screen at full
 * UI_THEME_MIN_TOUCH_TARGET_PX height, with Prev/Next stepping between
 * pages.
 *
 * The arithmetic, against content's real ~267px (y=44..311) and a
 * UI_THEME_PADDING_PX/2 inter-child gap:
 *
 *     nav row (Back / Prev / N of M / Next) .... 44px
 *     gap ....................................... 4px
 *     one hub page: 2 rows x 72px + 1 gap ...... 148px
 *                                               ------
 *                                                196px  <= 267px  OK
 *
 * Three rows would need 224px and total 272px, which overflows -- hence two
 * rows, i.e. four cells per page, and three pages for the current ten items.
 * If an eleventh item is added it lands on page 3 (which currently holds
 * two); a thirteenth needs a fourth page, and UI_CONFIG_HUB_PAGE_COUNT plus
 * the switch in ui_page_config_build() are the only two places to change.
 *
 * The active page index is deliberately module state that survives leaving
 * this screen: kiln_ui.c never tears a page down, so a user who reaches
 * Diagnostics from hub page 3 and presses Back returns to hub page 3 rather
 * than being dumped back on page 1. That keeps every sub-page's "Back goes
 * back exactly one level" contract intact -- Back lands on the hub view the
 * user actually came from. */
#define UI_CONFIG_HUB_PAGE_COUNT     3
#define UI_CONFIG_HUB_ITEMS_PER_PAGE 4
#define UI_CONFIG_HUB_PAGE_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static lv_obj_t *s_hub_pages[UI_CONFIG_HUB_PAGE_COUNT];
static lv_obj_t *s_hub_indicator;
static uint8_t s_hub_page;

/* Shows hub page `index` and hides the rest. Safe before the page has been
 * built (all slots NULL) so the Prev/Next callbacks never need their own
 * guard. */
static void hub_show_page(uint8_t index)
{
    if (index >= UI_CONFIG_HUB_PAGE_COUNT) {
        return;
    }
    s_hub_page = index;

    for (uint8_t i = 0; i < UI_CONFIG_HUB_PAGE_COUNT; i++) {
        if (!s_hub_pages[i]) {
            continue;
        }
        if (i == index) {
            lv_obj_remove_flag(s_hub_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_hub_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_hub_indicator) {
        lv_label_set_text_fmt(s_hub_indicator, "%u of %u", (unsigned)(index + 1),
                              (unsigned)UI_CONFIG_HUB_PAGE_COUNT);
    }

    ESP_LOGI(TAG, "hub page %u of %u", (unsigned)(index + 1), (unsigned)UI_CONFIG_HUB_PAGE_COUNT);
}

/* Prev/Next clamp rather than wrap. Wrapping would make "Next" on the last
 * page silently jump back to the first, which on a four-cell page reads as
 * the UI having lost the press -- clamping leaves the screen visibly
 * unchanged, which is the honest response to "there is nothing after this". */
/* Both callbacks re-emit the tap-target dump after switching. Paging does not
 * go through kiln_ui_show(), so nothing else would: every cell on the screen
 * changes while the last dump on record still describes the previous page,
 * which would quietly mislead anything aiming an injected touch. */
void ui_page_config_reset_to_first_page(void)
{
    hub_show_page(0);
}

static void hub_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_hub_page > 0) {
        hub_show_page((uint8_t)(s_hub_page - 1));
        kiln_ui_log_tap_targets();
    }
}

static void hub_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_hub_page + 1 < UI_CONFIG_HUB_PAGE_COUNT) {
        hub_show_page((uint8_t)(s_hub_page + 1));
        kiln_ui_log_tap_targets();
    }
}

/* One hub page: a non-scrollable two-row wrap grid sized to exactly the two
 * rows it holds. LV_OBJ_FLAG_SCROLLABLE is cleared explicitly (rather than
 * relied on being off) because a cell that overflowed would otherwise
 * silently reintroduce the scrolling this rework exists to remove. */
static lv_obj_t *build_hub_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_height(page, UI_CONFIG_HUB_PAGE_HEIGHT_PX);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(page, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

/* A small fixed-width button for the nav row (Prev/Next/Back). Kept separate
 * from build_nav_item(): those are lv_pct(48) grid cells, these are a fixed
 * 44px-tall row that must leave space for the page indicator between them. */
static lv_obj_t *build_hub_nav_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);
    return btn;
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

    /* Three stacked hub pages, exactly one visible at a time -- see the
     * "Paged hub" comment block above for the height arithmetic and for why
     * this replaced the old internally-scrolling grid. All three are built
     * up front and toggled with LV_OBJ_FLAG_HIDDEN rather than rebuilt on
     * demand: kiln_ui.c never tears a page down, so building once keeps
     * Prev/Next instant and keeps every cell's coordinates stable for the
     * tap-target dump. */
    for (uint8_t i = 0; i < UI_CONFIG_HUB_PAGE_COUNT; i++) {
        s_hub_pages[i] = build_hub_page(content);
    }

    /* Page 1: the two not-yet-built placeholders sit here, with the two most
     * frequently used real destinations, so the placeholders never push a
     * working page off the visible area the way they did when all ten cells
     * shared one scrolling grid. */
    build_nav_item(s_hub_pages[0], "Zones & Thermocouples (not built yet)", NULL);
    build_nav_item(s_hub_pages[0], "Relays & Rules (not built yet)", NULL);
    build_nav_item(s_hub_pages[0], "Temperature", temperature_nav_cb);
    build_nav_item(s_hub_pages[0], "Network / Wi-Fi", network_nav_cb);

    build_nav_item(s_hub_pages[1], "Board Health", board_health_nav_cb);
    build_nav_item(s_hub_pages[1], "Safety Processor", safety_nav_cb);
    build_nav_item(s_hub_pages[1], "Temperature History", history_nav_cb);
    build_nav_item(s_hub_pages[1], "Touch Calibration", touch_cal_nav_cb);

    /* Page 3 holds the two diagnostic pages -- the two that were furthest
     * off-screen (cell centres at y=383 on a 320px panel) under the old
     * scrolling grid, i.e. the ones this rework most needed to make
     * reachable. Room for two more items here before a fourth page is
     * needed.
     *
     * "Profiles" (LCD profile browse/start, this pass) is the 11th item --
     * verified against this file before relying on the claim: this page
     * held exactly 2 of its 4 cells before this pass (Diagnostics,
     * Thermocouple Faults), so the 11th item lands here with NO layout or
     * UI_CONFIG_HUB_PAGE_COUNT change, leaving exactly one cell free for a
     * 12th. */
    build_nav_item(s_hub_pages[2], "Diagnostics", diagnostics_nav_cb);
    build_nav_item(s_hub_pages[2], "Thermocouple Faults", thermo_faults_nav_cb);
    build_nav_item(s_hub_pages[2], "Profiles", profiles_nav_cb);

    /* Nav row: Back on the left, then Prev / "N of M" / Next. Back keeps its
     * own callback and its one-level-up target (home) unchanged -- paging
     * within the hub is not navigation between pages in kiln_ui's sense, so
     * it must not consume the Back press. */
    lv_obj_t *nav_row = lv_obj_create(content);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 44);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);

    build_hub_nav_button(nav_row, "Back", back_btn_cb);
    build_hub_nav_button(nav_row, "< Prev", hub_prev_cb);

    s_hub_indicator = lv_label_create(nav_row);
    lv_obj_set_style_text_color(s_hub_indicator, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_hub_indicator, "");

    build_hub_nav_button(nav_row, "Next >", hub_next_cb);

    /* Apply the remembered page (0 on the first build) so the indicator text
     * and hidden flags start consistent with s_hub_page. */
    hub_show_page(s_hub_page);

    return scr;
}
