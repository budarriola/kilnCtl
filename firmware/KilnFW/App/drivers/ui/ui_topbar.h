// Shared top-bar chrome: the title on the left, and whatever navigation
// icons a page needs on the right.
//
// WHY THIS EXISTS. Until now every page hand-built its own status bar (a
// title label) and then spent 44px plus a gap of its scarce ~267px content
// budget on a full-width row of text buttons -- "Back", "< Prev", "Next >".
// On ui_page_config.c that row cost 48px of the 267px available, which is
// what forced the hub down to two rows of cells per page and therefore three
// pages for eleven destinations. Moving those controls into the status bar
// (which is already on screen and already mostly empty) hands that 48px back
// to content: the same hub fits three rows, six cells per page, two pages.
//
// It is also the answer to "the Back/Prev/Next buttons should always be
// icons at the top of the screen if they are required, like the gear is on
// the main screen" -- one implementation, so a page cannot drift into having
// its own subtly different back affordance.
//
// THE TWO LVGL TRAPS THIS MODULE ENCAPSULATES. Both were found on hardware,
// both cost a debugging session, and both are invisible in the source if you
// hand-roll a status bar yourself. They are the real reason this is a module
// rather than a copy-pasted 20-line helper:
//
//   1. HIT TESTING DOES NOT ESCAPE THE PARENT. LVGL's hit test descends the
//      widget tree and can only recurse into a child if the *parent's* own
//      area already contains the touch point. A child's ext_click_area can
//      never reach past a parent that does not itself cover that point. The
//      status bar is a fixed UI_THEME_STATUS_BAR_HEIGHT_PX (32px) box, so an
//      icon parented straight to it is hard-capped at 32px of vertical touch
//      reach regardless of how generous ui_theme_apply_touch_area() was --
//      measured as a 21x23px effective target for the home page's gear.
//      Fix: the icons live in `icons`, a proxy container parented to the
//      SCREEN, not to the bar, so nothing clips it.
//
//   2. THE FLEX TRAP. A page root here is a flex column. A plain child added
//      to it joins the flow as the next column item: it does NOT keep the
//      lv_obj_align() position you asked for, and its height is subtracted
//      from the column's available space like any other row. The first
//      version of the home page's gear proxy missed LV_OBJ_FLAG_FLOATING and
//      was measured sitting at the BOTTOM of the screen while the content
//      area shrank 267px -> 227px, breaking the no-scroll budget.
//      LV_OBJ_FLAG_FLOATING tells flex to skip the object entirely. This
//      module sets it, first thing after creation, before anything reads
//      back the proxy's geometry.
//
// USAGE, and the one thing a caller must not forget:
//
//     ui_topbar_t tb;
//     ui_topbar_create(scr, &(ui_topbar_cfg_t){
//         .title = "Diagnostics", .back_page = "config", .show_home = true,
//     }, &tb);
//     ... build the content area ...
//     ui_topbar_raise(&tb);   // REQUIRED -- see below
//
// ui_topbar_raise() must be called AFTER the content area exists. The icon
// proxy overlaps whatever sits beneath it, and LVGL resolves overlapping
// hit tests by child order: without the raise, a content widget created
// later wins the tap and the icons go dead in exactly the region they cover.
//
// Single-threaded like the rest of the UI: call only from the LVGL task (see
// lvgl_port.h).
#ifndef UI_TOPBAR_H
#define UI_TOPBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drawn size of one icon button, and the gap between two of them. 36x26
 * rather than UI_THEME_MIN_TOUCH_TARGET_PX (72): the bar is only 32px tall,
 * so a 72px-tall button cannot be DRAWN there. The 72px minimum is met by
 * the effective touch area instead -- ui_theme_apply_touch_area() extends
 * each icon's ext_click_area outward, and the proxy container is
 * deliberately taller than the bar (see UI_TOPBAR_ICON_ROW_H_PX) so that
 * extension is not clipped away by trap #1 above. */
#define UI_TOPBAR_ICON_W_PX   36
#define UI_TOPBAR_ICON_H_PX   26
#define UI_TOPBAR_ICON_GAP_PX 4

/* Relay-life budget indicator tier (RELAY_LIFE_BUDGET.md). Kept
 * as this module's own enum rather than including relay_cycles.h's
 * relay_budget_tier_t -- ui_topbar.c is generic chrome with no business
 * knowing what a "relay" is; the caller (ui_page_home.c) maps
 * relay_cycles_max_budget_tier() onto this 1:1. */
typedef enum {
    UI_TOPBAR_WARNING_NONE = 0,
    UI_TOPBAR_WARNING_WARN = 1,
    UI_TOPBAR_WARNING_ERROR = 2,
} ui_topbar_warning_tier_t;

typedef struct {
    /* Left-hand title. NULL for a page that puts something else there --
     * ui_page_home.c shows a live Wi-Fi/IP status string instead. */
    const char *title;

    /* Back icon. The kiln_ui_show() page name to navigate to, i.e. exactly
     * one level up; NULL for no back icon (the home page). The string is not
     * copied, so pass a literal. */
    const char *back_page;

    /* Home icon -- navigates to "home". Set on every page except home
     * itself. Deliberately separate from back_page: on a page one level down
     * from home the two icons would go to the same place, and showing both
     * is still correct (they mean different things once the tree is deeper),
     * so this module does not try to be clever and suppress one. */
    bool show_home;

    /* Prev/Next paging icons. NULL for a page that does not page. A page
     * that sets these is expected to clamp rather than wrap at the ends --
     * see ui_topbar_set_prev_enabled()/set_next_enabled() for the visual
     * half of that. */
    lv_event_cb_t prev_cb;
    lv_event_cb_t next_cb;

    /* Settings gear. Home only; every other page reaches config through the
     * hub it came from. */
    lv_event_cb_t gear_cb;

    /* Relay-life budget warning icon (LV_SYMBOL_WARNING). Home only, per
     * RELAY_LIFE_BUDGET.md -- set true to reserve the slot; the
     * icon itself starts hidden and is toggled at runtime by
     * ui_topbar_set_warning() from the page's own periodic refresh. Built as
     * a plain (non-clickable) indicator, not a button -- it has nothing to
     * navigate to. */
    bool warning_icon;
} ui_topbar_cfg_t;

typedef struct {
    lv_obj_t *bar;      /* the in-flow status-bar row; the title lives here */
    lv_obj_t *title;    /* NULL if cfg->title was NULL */
    lv_obj_t *icons;    /* the FLOATING icon proxy, parented to the screen */
    lv_obj_t *back_btn;
    lv_obj_t *home_btn;
    lv_obj_t *prev_btn;
    lv_obj_t *next_btn;
    lv_obj_t *gear_btn;
    lv_obj_t *warning_btn;   /* NULL unless cfg->warning_icon was set; starts hidden */

    /* Horizontal px the icon proxy occupies on the right-hand side. A page
     * that puts its own wide widget in the bar (the home page's status
     * label) must cap that widget's WIDTH to bar_width - icons_w - gap.
     * lv_obj_align_to() sets a position and never constrains width, so
     * anchoring alone does not stop a long string running under the icons --
     * that exact bug shipped once already. */
    int32_t icons_w;
} ui_topbar_t;

/* Builds the bar and its icons. `scr` must be the page root (a flex column,
 * as every page here is). Writes the handles into *out; every unused handle
 * is NULL. */
void ui_topbar_create(lv_obj_t *scr, const ui_topbar_cfg_t *cfg, ui_topbar_t *out);

/* Moves the icon proxy above everything else on the screen. MUST be called
 * once, after the page's content area has been created -- see this header's
 * usage note. */
void ui_topbar_raise(const ui_topbar_t *tb);

/* Retitle after construction (a page whose title reflects live state). No-op
 * if the bar was built without a title. */
void ui_topbar_set_title(const ui_topbar_t *tb, const char *text);

/* Dim and un-click a paging icon that would do nothing -- i.e. Prev on the
 * first page. Clamping is what these pages do instead of wrapping (wrapping
 * makes "Next" on the last page jump back to the first, which reads as the
 * UI having lost the press), and greying the dead end says so on screen
 * rather than leaving a live-looking button that ignores taps. */
void ui_topbar_set_prev_enabled(const ui_topbar_t *tb, bool enabled);
void ui_topbar_set_next_enabled(const ui_topbar_t *tb, bool enabled);

/* Shows/hides the relay-life warning icon and colours it for the tier.
 * NONE hides it; WARN/ERROR show it in the existing UI_THEME_ACCENT_1
 * (orange) / UI_THEME_ACCENT_5 (red) tokens respectively -- this header has
 * no dedicated "warn"/"bad" colour of its own (see ui_theme.h's provenance
 * note: this palette has never been measured on real hardware), so this
 * reuses the two accents diagnostics already treats as warning/fault
 * colours rather than adding a new hex value. No-op if cfg->warning_icon
 * was not set at ui_topbar_create() time. Deliberately PERSISTENT once
 * shown -- unlike every other transient notice on this page, this warning
 * does not clear itself; see the call site's comment in ui_page_home.c for
 * why that is intentional rather than the "warning nobody reads" anti-
 * pattern this codebase otherwise avoids. */
void ui_topbar_set_warning(const ui_topbar_t *tb, ui_topbar_warning_tier_t tier);

#ifdef __cplusplus
}
#endif

#endif // UI_TOPBAR_H
