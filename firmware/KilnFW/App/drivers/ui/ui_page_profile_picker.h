// ui_page_profile_picker -- the unified, favorites-first, paginated profile
// list, UI_PLAN.md Section 6.2. One widget, two modes:
//
//   MANAGE mode (registered as "profiles", replacing the old four-cell hub
//   and its ui_page_profiles_mine.c / ui_page_profiles_family.c sub-pages):
//   New (topbar icon) and a per-row Delete for user slots.
//
//   PICK mode (registered as "profile_picker", for Section 6.1's dashboard
//   picker -- a later wave wires the caller): no New, no Delete, tapping a
//   row invokes the callback set by ui_page_profile_picker_set_pick_cb()
//   instead of opening the detail screen.
//
// Both modes share the same 4-row-per-page, 64px-row column
// (UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE / _ROW_H_PX below) built once in
// ui_page_profile_picker.c -- "do not fork it" per the plan.
//
// Data source: profiles_http_get() over user slots 0..PROFILES_MAX_COUNT-1
// plus profiles_builtin_entry() over the builtin catalogue, skipping
// profiles_builtin_is_hidden() entries. Ordered via ui_profile_list_order()
// (favorites first, stable partition) using profiles_favorites_is() as the
// predicate -- see that header for why this is a plain RAM read, safe
// directly on the LVGL task.
#ifndef UI_PAGE_PROFILE_PICKER_H
#define UI_PAGE_PROFILE_PICKER_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Layout constants -- UI_PLAN.md 6.2's owner-decided 4x64 column, zero slack
 * against UI_THEME_PAGE_CONTENT_BUDGET_PX. See ui_page_profile_picker.c for
 * the _Static_assert that pins this at build time. */
#define UI_PAGE_PROFILE_PICKER_ROWS_PER_PAGE 4
#define UI_PAGE_PROFILE_PICKER_ROW_H_PX      64

/* Builds the MANAGE-mode screen -- New icon in the topbar, per-row Delete for
 * user slots (never for a builtin id, PROFILE_BUILTIN_ID_BASE and up).
 * Registered in kiln_ui.c as "profiles". */
lv_obj_t *ui_page_profile_picker_build_manage(void);

/* Resets manage mode to page 0 and reloads/reorders ids. Callers navigating
 * INTO "profiles" must call this first (kiln_ui_show() caches the page after
 * its first build, so a stale render would otherwise survive a delete/import
 * made elsewhere) -- same "refresh before show" contract
 * ui_page_profiles_mine_refresh() used to document. */
void ui_page_profile_picker_manage_refresh(void);

/* Builds the PICK-mode screen -- no New, no Delete, a row tap invokes the
 * pick callback instead of opening the detail screen. Registered in
 * kiln_ui.c as "profile_picker". */
lv_obj_t *ui_page_profile_picker_build_pick(void);

/* Resets pick mode to page 0 and reloads/reorders ids. A caller opening
 * "profile_picker" must call this first, same reason as the manage refresh
 * above. */
void ui_page_profile_picker_pick_refresh(void);

/* Row-tap callback for PICK mode, invoked with the tapped row's profile id
 * (a user slot or a builtin id -- never altered by the favorite star, which
 * is a label-only prefix, see ui_page_profile_picker_format.h). NULL (the
 * default) makes a pick-mode row tap a no-op. The pointer is not copied;
 * pass a function with static storage duration. */
typedef void (*ui_page_profile_picker_pick_cb_t)(uint8_t profile_id);
void ui_page_profile_picker_set_pick_cb(ui_page_profile_picker_pick_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILE_PICKER_H
