#include "ui_page_config.h"

#include <stdio.h>

#include "esp_log.h"

#include "kiln_ui.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"

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

static void tc_types_nav_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("tc_types");
}

/* ROADMAP.md 2026-08-21 "a real shared temperature-unit setting": the hub's
 * one free cell (page 2 holds 5 of 6, per this file's own "Paged hub"
 * comment above -- verified against the current build, not assumed) becomes
 * a live in-place toggle rather than a navigation destination. This is
 * deliberate, not a shortcut: adding a whole new LCD page/route requires
 * registering it in kiln_ui.c's page table (kiln_ui_register_page()), and
 * kiln_ui.c is off-limits this pass (another agent owns the bench/safety
 * work there right now). A toggle cell needs no new route -- it calls
 * unit_pref_set() directly and repaints its own label, so this hub file is
 * the only LCD file this feature needed to touch to get an on-device
 * control, which is also arguably the more discoverable place for it
 * anyway (no extra tap to a sub-page for a single binary choice). */
static lv_obj_t *s_units_cell_label;

static void units_cell_set_label(void)
{
    if (!s_units_cell_label) {
        return;
    }
    char buf[32]; /* was 24 -- too small for "Units: Fahrenheit\n(tap for C)" (30 chars +
                   * NUL); gcc's format-truncation check caught this at -Werror build time. */
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
         * case. A silent log line is enough here: the label below already
         * shows the operator the choice took effect this session. */
        ESP_LOGW(TAG, "unit preference applied but not persisted -- will not survive a reboot");
    }
    units_cell_set_label();
}

/* --- Paged hub, 2026-08-21 ------------------------------------------------
 * This hub used to be a single fixed-height container that scrolled
 * internally, then (2026-08-20) a paged grid with its own bottom nav row
 * (Back / "< Prev" / "N of M" / "Next >") costing 44px + a 4px gap out of
 * content's budget. The user's request ("the configuration menu can be more
 * dense and the back/prev/next buttons should always be icons at the top of
 * the screen if they are required") moves those three controls into the
 * shared top bar (ui_topbar.c/.h) instead: Back and Prev/Next are now icons
 * next to the gear-equivalent row, and the page indicator ("N of M") is the
 * bar's TITLE (see hub_show_page()'s ui_topbar_set_title() call), so no
 * on-screen real estate is spent duplicating information the title slot
 * already had room for.
 *
 * Removing the nav row frees its 44px plus the 4px gap that used to separate
 * it from the grid above -- 48px back to content. Verified cell count in
 * this file before touching the arithmetic (do not trust a stale comment
 * here either): eleven destinations total -- page 0 held 4 (Zones &
 * Thermocouples, Relays & Rules, Temperature, Network/Wi-Fi), page 1 held 4
 * (Board Health, Safety Processor, Temperature History, Touch Calibration),
 * page 2 held 3 (Diagnostics, Thermocouple Faults, Profiles).
 *
 * The arithmetic, against content's real ~267px (y=44..311) and a
 * UI_THEME_PADDING_PX/2 = 4px inter-child gap, now that the nav row is gone:
 *
 *     one hub page: 3 rows x 72px + 2 gaps ..... 224px
 *                                               ------
 *                                                224px  <= 267px  OK
 *
 * Three rows of two cells is six cells per page -- up from four -- so the
 * same eleven destinations now fit on two pages (6 + 5) instead of three.
 * Page assignments below were re-split accordingly. A twelfth item still
 * fits page 1 [sic -- page 2] (which now holds 5 of 6); a thirteenth needs a
 * third page, and UI_CONFIG_HUB_PAGE_COUNT plus the switch in
 * ui_page_config_build() are the only two places to change.
 *
 * 2026-08-21: that twelfth slot is now used -- the shared temperature-unit
 * toggle (build_unit_toggle_item()) fills page 2's one remaining cell, so
 * both hub pages are now full (6 + 6). A thirteenth destination genuinely
 * needs a third page now; there is no more free room.
 *
 * 2026-08-21, LCD item 1: that thirteenth destination arrived the same day
 * -- "Thermocouple Types" (ui_page_tc_types.c/.h), the LCD equivalent of the
 * web zones page's new per-channel/safety-processor type selector. Verified
 * both existing pages were genuinely full (6 + 6, per the paragraph above)
 * before touching the arithmetic here, not assumed from a possibly-stale
 * comment. UI_CONFIG_HUB_PAGE_COUNT is now 3; page 3 holds exactly one real
 * cell (Thermocouple Types) against its 6-cell capacity -- five cells free
 * for whatever the fourteenth through eighteenth destinations turn out to
 * be, no further hub surgery needed until then.
 *
 * A whole new page/route was chosen over reusing an existing thermocouple-
 * related page: "Zones & Thermocouples" (page 1) is still an honest
 * "not built yet" placeholder with no real content to attach a control to,
 * and "Thermocouple Faults" (ui_page_thermo_faults.c) is a read-only,
 * fast-refresh-timer live-fault monitor -- bolting a config-write control
 * onto that page's refresh_cb() would mean either the fault page owning NVS
 * writes it has no other business touching, or a config editor silently
 * inheriting a 1500ms repaint timer it doesn't need. A dedicated page keeps
 * both existing pages' contracts unchanged.
 *
 * The active page index is deliberately module state that survives leaving
 * this screen: kiln_ui.c never tears a page down, so a user who reaches
 * Diagnostics from hub page 2 and presses Back returns to hub page 2 rather
 * than being dumped back on page 1. That keeps every sub-page's "Back goes
 * back exactly one level" contract intact -- Back lands on the hub view the
 * user actually came from. */
#define UI_CONFIG_HUB_PAGE_COUNT     3
#define UI_CONFIG_HUB_ITEMS_PER_PAGE 6
#define UI_CONFIG_HUB_PAGE_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 3 + (UI_THEME_PADDING_PX / 2) * 2)

static ui_topbar_t s_topbar;

static lv_obj_t *s_hub_pages[UI_CONFIG_HUB_PAGE_COUNT];
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

    /* The page indicator used to be its own label in a nav row; it is now
     * the top bar's TITLE (ui_topbar_set_title()) -- see this file's "Paged
     * hub" comment for why folding it into the bar is what pays for the
     * denser 3-row grid. No-op before the page is built (title is NULL
     * until ui_topbar_create() runs), same as the old s_hub_indicator NULL
     * guard this replaces. */
    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Configuration  %u of %u", (unsigned)(index + 1),
             (unsigned)UI_CONFIG_HUB_PAGE_COUNT);
    ui_topbar_set_title(&s_topbar, title_buf);

    /* Dim (not hide -- see ui_topbar.c's set_icon_enabled() comment) the
     * paging icon that would do nothing at this end of the hub. Clamp, not
     * wrap -- same rule this hub always used, now shown via the shared
     * top-bar visual instead of a page-local one. */
    ui_topbar_set_prev_enabled(&s_topbar, index > 0);
    ui_topbar_set_next_enabled(&s_topbar, index + 1 < UI_CONFIG_HUB_PAGE_COUNT);

    ESP_LOGI(TAG, "hub page %u of %u", (unsigned)(index + 1), (unsigned)UI_CONFIG_HUB_PAGE_COUNT);
}

/* Prev/Next clamp rather than wrap. Wrapping would make "Next" on the last
 * page silently jump back to the first, which on a six-cell page reads as
 * the UI having lost the press -- clamping leaves the screen visibly
 * unchanged, which is the honest response to "there is nothing after this".
 * The visual half of clamping (dimming the dead-end icon) is
 * ui_topbar_set_prev_enabled()/set_next_enabled(), called from
 * hub_show_page() above. */
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

/* One hub page: a non-scrollable three-row wrap grid sized to exactly the
 * three rows it holds. LV_OBJ_FLAG_SCROLLABLE is cleared explicitly (rather
 * than relied on being off) because a cell that overflowed would otherwise
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

/* The units toggle cell -- same shape/size as build_nav_item()'s real
 * (clickable) cells, but two-line text and its own click handler
 * (units_toggle_cb) instead of a kiln_ui_show() navigation, and it keeps a
 * handle to its label so units_cell_set_label() can repaint it in place
 * after every toggle (and on rebuild, so a reboot with a saved Fahrenheit
 * preference shows the right text immediately rather than a stale
 * "Celsius" the operator has to tap once to correct). */
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
     * the FLOATING flex trap); see that header. Back already goes to home
     * from here, so show_home is deliberately false (a second identical
     * icon to the same destination is noise, not redundancy worth having).
     * Prev/Next live here too now -- see this file's "Paged hub" comment for
     * why moving them off a dedicated nav row is what pays for the third
     * grid row. The title starts as a placeholder; hub_show_page() (called
     * at the bottom of this function) immediately overwrites it with the
     * real "Configuration  N of M" text via ui_topbar_set_title(). */
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Configuration",
        .back_page = "home",
        .show_home = false,
        .prev_cb = hub_prev_cb,
        .next_cb = hub_next_cb,
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

    /* Two stacked hub pages, exactly one visible at a time -- see the
     * "Paged hub" comment block above for the height arithmetic and for why
     * this replaced the old three-page/nav-row layout. Both are built up
     * front and toggled with LV_OBJ_FLAG_HIDDEN rather than rebuilt on
     * demand: kiln_ui.c never tears a page down, so building once keeps
     * Prev/Next instant and keeps every cell's coordinates stable for the
     * tap-target dump. */
    for (uint8_t i = 0; i < UI_CONFIG_HUB_PAGE_COUNT; i++) {
        s_hub_pages[i] = build_hub_page(content);
    }

    /* Page 1 (6 cells, full): the two not-yet-built placeholders sit here,
     * with the most frequently used real destinations, so the placeholders
     * never push a working page off the visible area. */
    build_nav_item(s_hub_pages[0], "Zones & Thermocouples (not built yet)", NULL);
    build_nav_item(s_hub_pages[0], "Relays & Rules (not built yet)", NULL);
    build_nav_item(s_hub_pages[0], "Temperature", temperature_nav_cb);
    build_nav_item(s_hub_pages[0], "Network / Wi-Fi", network_nav_cb);
    build_nav_item(s_hub_pages[0], "Board Health", board_health_nav_cb);
    build_nav_item(s_hub_pages[0], "Safety Processor", safety_nav_cb);

    /* Page 2 (5 of 6 cells -- one free for a 12th destination before a third
     * page is needed): the remaining five destinations. */
    build_nav_item(s_hub_pages[1], "Temperature History", history_nav_cb);
    build_nav_item(s_hub_pages[1], "Touch Calibration", touch_cal_nav_cb);
    build_nav_item(s_hub_pages[1], "Diagnostics", diagnostics_nav_cb);
    build_nav_item(s_hub_pages[1], "Thermocouple Faults", thermo_faults_nav_cb);
    build_nav_item(s_hub_pages[1], "Profiles", profiles_nav_cb);
    /* 12th destination, in the one cell page 2 had free (5 of 6 -- confirmed
     * against this file's own "Paged hub" comment before adding this, not
     * assumed). See build_unit_toggle_item()'s comment for why this is an
     * in-place toggle rather than a new kiln_ui_show() route. */
    build_unit_toggle_item(s_hub_pages[1]);

    /* Page 3 (1 of 6 cells, 2026-08-21): "Thermocouple Types" -- see this
     * file's "Paged hub" comment for why this got a third page instead of
     * being squeezed onto an already-full page 1/2 or bolted onto an
     * existing thermocouple-related page. */
    build_nav_item(s_hub_pages[2], "Thermocouple Types", tc_types_nav_cb);

    /* content is created after the topbar's icon proxy, so without this it
     * would sit above the proxy in z-order and win taps in the overlap
     * region -- see ui_topbar.h's usage note: raise MUST happen after the
     * content area exists. */
    ui_topbar_raise(&s_topbar);

    /* Apply the remembered page (0 on the first build) so the title text
     * and hidden flags start consistent with s_hub_page. */
    hub_show_page(s_hub_page);

    return scr;
}
