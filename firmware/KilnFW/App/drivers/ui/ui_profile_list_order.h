#ifndef UI_PROFILE_LIST_ORDER_H
#define UI_PROFILE_LIST_ORDER_H

/* Pure ordering helper for the LCD profile list/picker (firmware/KilnFW/docs/
 * UI_PLAN.md Section 6.2) -- host-testable, no LVGL, no lock. It must match
 * main_page.html's orderProfilesByFavorite() exactly:
 *
 *   function orderProfilesByFavorite(list, favIds) {
 *     const favSet = {};
 *     (favIds || []).forEach(function (id) { favSet[String(id)] = true; });
 *     const favs = [], rest = [];
 *     (list || []).forEach(function (p) {
 *       if (favSet[String(p.id)]) favs.push(p); else rest.push(p);
 *     });
 *     return favs.concat(rest);
 *   }
 *
 * i.e. a STABLE PARTITION: every id that is a favorite, in the order it
 * already appeared in the input list, followed by every id that is not, also
 * in its original order -- the same set of ids in, the same set out, nothing
 * dropped or duplicated. The star main_page.html's profileOptionLabel()
 * prepends is a display-only label prefix and never touched here: this
 * module orders ids, it does not format them.
 *
 * The favorite predicate is passed in as a function pointer (rather than
 * calling profiles_favorites_is()/profiles_favorites_masks() directly) so
 * this file has zero firmware dependencies and can be linked into a host
 * test with a fake predicate -- UI_PLAN.md's "the module stays host-testable
 * with no firmware deps" requirement. A caller wiring this into the real UI
 * passes profiles_favorites_is (matching signature: bool(uint8_t)) as
 * `is_favorite`. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*ui_profile_list_order_is_favorite_fn)(uint8_t id);

/* Stable-partitions `ids[0..count-1]` into `out[0..count-1]`: every id for
 * which `is_favorite(id)` returns true, in original relative order, followed
 * by every id for which it returns false, also in original relative order.
 * `out` must have room for `count` entries and may NOT alias `ids` (the
 * partition is built by two independent forward scans over `ids`, so writing
 * into `ids` itself while still reading it would corrupt the second scan).
 *
 * count == 0 is a no-op (nothing written, `ids`/`out`/`is_favorite` may all
 * be anything including NULL). A NULL `is_favorite` with count > 0 is
 * treated as "nothing is a favorite" -- every id lands in the tail group,
 * unchanged order -- rather than a crash, matching main_page.html's own
 * `(favIds || [])` degrade-to-empty behaviour when the favorites endpoint is
 * unreachable. */
void ui_profile_list_order(const uint8_t *ids, size_t count, ui_profile_list_order_is_favorite_fn is_favorite,
                            uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* UI_PROFILE_LIST_ORDER_H */
