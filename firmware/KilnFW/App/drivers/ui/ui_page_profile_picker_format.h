#ifndef UI_PAGE_PROFILE_PICKER_FORMAT_H
#define UI_PAGE_PROFILE_PICKER_FORMAT_H

/* Pure formatting/policy helpers for ui_page_profile_picker.c's rows,
 * factored out so they are host-testable with no LVGL -- same split as
 * ui_page_home_graph.c/.h (firmware/KilnFW/docs/UI_PLAN.md Section 6.5's
 * rail cites that precedent explicitly; this file is Section 6.2's use of
 * the same idiom). ui_page_profile_picker.c itself includes lvgl.h and is
 * therefore NOT linked into the host-test executable -- only this file and
 * ui_profile_list_order.c are, matching test_ui_page_home_graph.c's
 * lvgl-free split.
 *
 * UI_PLAN.md 6.2's "tests owed" list items (c) and (d):
 *   (c) the favorite star is a display-only label prefix -- it must never
 *       change the id the row carries, matching main_page.html's
 *       profileOptionLabel() (a star prepended to the rendered string, the
 *       <option>'s value stays p.id).
 *   (d) a builtin id (>= PROFILE_BUILTIN_ID_BASE) never yields a deletable
 *       row -- profiles_edit_http.c's delete route only ever addresses a
 *       user slot (0..PROFILES_MAX_COUNT-1); the builtin catalogue is a
 *       const .rodata table with no writable storage to delete. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Formats one row's label exactly like main_page.html's
 * profileOptionLabel(): "* " prepended when `is_favorite`, then `name`
 * verbatim, nothing else. `name` NULL is treated as an empty string rather
 * than crashing (defends the caller against a profile_t read that failed --
 * see profiles_http_get()'s bool return). `out_cap` >= 1 always leaves `out`
 * NUL-terminated (snprintf-backed); a `name` that does not fit is truncated,
 * never overflowed. The returned string carries no id -- see this header's
 * comment above for why that matters. */
void ui_page_profile_picker_format_label(const char *name, bool is_favorite, char *out, size_t out_cap);

/* True only for a user slot id (0..PROFILES_MAX_COUNT-1, see
 * profiles_types.h). A builtin id (>= PROFILE_BUILTIN_ID_BASE, profiles_
 * builtin.h) is never deletable -- it is a const table entry, matching the
 * web dashboard's exportable-but-not-deletable rule for the same ids
 * (profiles_edit_http.c's delete route). */
bool ui_page_profile_picker_is_deletable(uint8_t id);

/* How many pages `id_count` ids need at `rows_per_page` rows each -- always
 * at least 1, so an empty list still shows "1 of 1" / an empty page rather
 * than 0 pages. `rows_per_page` is passed in (rather than this file taking a
 * dependency on ui_page_profile_picker.h, which pulls in lvgl.h) so this
 * stays host-testable with no LVGL. */
uint8_t ui_page_profile_picker_format_page_count(uint8_t id_count, uint8_t rows_per_page);

/* The flat index into an ids[] array that (page, slot) refers to --
 * `page * rows_per_page + slot`, done in a wide-enough type that it cannot
 * silently wrap for any page/slot/rows_per_page this page ever uses. */
uint16_t ui_page_profile_picker_format_row_index(uint8_t page, uint8_t slot, uint8_t rows_per_page);

/* Clamps `page` so it is a valid page for `id_count` ids at `rows_per_page`
 * rows each -- i.e. so it is < ui_page_profile_picker_format_page_count(...).
 * Used after a delete shrinks id_count out from under the current page. */
uint8_t ui_page_profile_picker_format_clamp_page(uint8_t page, uint8_t id_count, uint8_t rows_per_page);

#ifdef __cplusplus
}
#endif

#endif /* UI_PAGE_PROFILE_PICKER_FORMAT_H */
