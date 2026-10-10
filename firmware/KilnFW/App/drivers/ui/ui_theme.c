#include "ui_theme.h"

#include <stdint.h>
#include <assert.h>

#include "esp_log.h"
#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- s_touch_groups: LVGL-task only, no ISR/DMA/flash */

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

/* Phase 7 theme/style pass (TODO.md 1223-1225) -- see ui_theme.h's block
 * comment above the declaration for what this mirrors (theme.css's
 * --ui-shadow-1/-2) and why a single lv_style shadow layer is the closest
 * LVGL analog to the web side's two-layer rgba() shadows. Pure paint: does
 * not touch layout. */
void ui_theme_apply_card_shadow(lv_obj_t *card, int level)
{
    if (!card) return;

    lv_opa_t opa = (level == 2) ? UI_THEME_SHADOW_2_OPA : UI_THEME_SHADOW_1_OPA;
    int32_t width = (level == 2) ? UI_THEME_SHADOW_2_WIDTH_PX : UI_THEME_SHADOW_1_WIDTH_PX;

    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, opa, 0);
    lv_obj_set_style_shadow_width(card, width, 0);
    lv_obj_set_style_shadow_spread(card, 0, 0);
    lv_obj_set_style_shadow_ofs_y(card, UI_THEME_SHADOW_OFS_Y_PX, 0);
}

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

/* --- Touch-group arbitration -- see ui_theme.h's block comment for the full
 * design writeup (why this exists, what it does and doesn't replace, and
 * exactly how lvgl_port.c's touch_read_cb() gets its override to "take" by
 * rewriting data->point rather than fighting LVGL's indev state machine). */

typedef struct {
    lv_obj_t *widgets[UI_THEME_TOUCH_GROUP_MAX_WIDGETS];
    size_t count;
} touch_group_t;

static EXT_RAM_BSS_ATTR touch_group_t s_touch_groups[UI_THEME_TOUCH_GROUP_MAX_GROUPS];
static size_t s_touch_group_count = 0;

void ui_theme_register_touch_group(lv_obj_t **widgets, size_t count)
{
    if (!widgets || count < 2) return; /* nothing to arbitrate between */
    if (s_touch_group_count >= UI_THEME_TOUCH_GROUP_MAX_GROUPS) {
        ESP_LOGE("ui_theme", "touch-group registry full (%d); group dropped",
                 (int)UI_THEME_TOUCH_GROUP_MAX_GROUPS);
        assert(!"touch-group registry full");
        return;
    }
    if (count > UI_THEME_TOUCH_GROUP_MAX_WIDGETS) {
        ESP_LOGE("ui_theme", "touch group of %u exceeds %d widgets; dropped",
                 (unsigned)count, (int)UI_THEME_TOUCH_GROUP_MAX_WIDGETS);
        assert(!"touch group too large");
        return;
    }

    touch_group_t *g = &s_touch_groups[s_touch_group_count];
    for (size_t i = 0; i < count; i++) {
        g->widgets[i] = widgets[i];
    }
    g->count = count;
    s_touch_group_count++;
}

bool ui_theme_touch_groups_active(void)
{
    return s_touch_group_count > 0;
}

/* Point-in-click-area test using LVGL's own already-expanded box
 * (lv_obj_get_click_area() = the widget's real coords plus whatever
 * ext_click_area ui_theme_apply_touch_area() gave it) rather than
 * re-deriving the extension amount here -- this file doesn't need to track
 * ext_click_area itself; LVGL already does, and hands it back. */
static bool point_in_click_area(lv_obj_t *widget, lv_point_t point)
{
    lv_area_t area;
    lv_obj_get_click_area(widget, &area);
    return point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2;
}

lv_obj_t *ui_theme_resolve_touch_target(lv_obj_t *default_target, lv_point_t point)
{
    if (!default_target) return default_target;

    for (size_t gi = 0; gi < s_touch_group_count; gi++) {
        touch_group_t *g = &s_touch_groups[gi];

        bool is_member = false;
        for (size_t i = 0; i < g->count; i++) {
            if (g->widgets[i] == default_target) {
                is_member = true;
                break;
            }
        }
        if (!is_member) continue;

        /* default_target is in this group -- find whichever member's real
         * (unexpanded) center is closest to the touch point, among members
         * whose expanded click area actually contains the point. Squared
         * distance: only the ordering matters, no need for sqrt(). */
        lv_obj_t *best = default_target;
        int64_t best_dist_sq = -1;
        for (size_t i = 0; i < g->count; i++) {
            lv_obj_t *w = g->widgets[i];
            if (!point_in_click_area(w, point)) continue;

            lv_area_t coords;
            lv_obj_get_coords(w, &coords);
            int32_t cx = (coords.x1 + coords.x2) / 2;
            int32_t cy = (coords.y1 + coords.y2) / 2;
            int64_t dx = (int64_t)point.x - cx;
            int64_t dy = (int64_t)point.y - cy;
            int64_t dist_sq = dx * dx + dy * dy;

            if (best_dist_sq < 0 || dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                best = w;
            }
        }
        /* default_target's own click area is guaranteed to contain point
         * (that's how LVGL picked it in the first place), so best_dist_sq
         * is always set by the loop above -- `best` never falls back to an
         * uninitialized choice. */
        return best;
    }

    return default_target; /* not in any registered group -- LVGL's pick stands */
}
