#include "ui_theme.h"

/* ui_theme.c -- companion to ui_theme.h's constants, TODO.md 10.4 ("Touch
 * hit-testing"). See ui_theme.h's "Touch hit-area sizing" comment block for
 * why this leans on LVGL's own lv_obj_set_ext_click_area() rather than a
 * custom nearest-center hit-tester, and for what that mechanism does and
 * does not solve.
 *
 * Nothing calls ui_theme_apply_touch_area() yet -- 10.3 (real page/button
 * layouts) is what will call this once it builds a button grid to size.
 * ui_page_home.c's placeholder label is intentionally left untouched (not a
 * real touch target, nothing to size).
 */

void ui_theme_apply_touch_area(lv_obj_t *widget, bool compact_layout)
{
    if (!widget) return;

    int32_t w = lv_obj_get_width(widget);
    int32_t h = lv_obj_get_height(widget);
    int32_t smaller_edge = (w < h) ? w : h;

    int32_t ext;
    if (compact_layout) {
        /* Dense grid (numeric keypad, a settings list's rows): extend just
         * enough to soften the exact pixel edge, capped at half of the
         * standard inter-cell gap (UI_THEME_PADDING_PX) so this cell's
         * expanded box can never reach past the midpoint between it and its
         * neighbor -- if it did, and the neighbor is expanded too, the two
         * boxes would overlap in the gap, and LVGL's z-order-first-match
         * hit test (see ui_theme.h) would silently hand a touch in that
         * overlap to whichever cell is tested first, not whichever is
         * actually closer. Integer /2 rounds down, erring toward the safe
         * (smaller, non-overlapping) side. */
        ext = UI_THEME_PADDING_PX / 2;
    } else {
        /* Sparse layout (main-page buttons, nav items): a generous fixed
         * margin, three padding-units wide, chosen so a normal full-size
         * button (already >= UI_THEME_MIN_TOUCH_TARGET_PX) gets a
         * comfortable halo without needing to reach any particular size.
         * If the widget itself is smaller than the minimum touch target
         * (e.g. a small icon button), extend further so the *effective*
         * clickable square reaches that minimum on each side, even though
         * the drawn widget doesn't grow -- whichever of the two computed
         * extensions is larger wins, so a big sparse button never gets less
         * than the generous default just because it's already big. */
        int32_t generous = UI_THEME_PADDING_PX * 3;
        if (smaller_edge > 0 && smaller_edge < UI_THEME_MIN_TOUCH_TARGET_PX) {
            int32_t needed = (UI_THEME_MIN_TOUCH_TARGET_PX - smaller_edge) / 2;
            ext = (needed > generous) ? needed : generous;
        } else {
            ext = generous;
        }
    }

    lv_obj_set_ext_click_area(widget, ext);
}
