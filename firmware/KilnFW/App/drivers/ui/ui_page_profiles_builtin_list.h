// ui_page_profiles_builtin_list -- the builtin catalogue entries for ONE
// firing type, set via ui_page_profiles_builtin_list_set_firing_type()
// (formerly called from ui_page_profiles_family_build()'s cells, deleted by
// UI_PLAN.md 6.2's picker rewrite -- see ui_page_profile_picker.c/.h),
// sorted by cone ascending and paginated 4-per-page same as
// ui_page_profile_picker.c. Glaze holds up to 23 of the 28 entries as of
// 2026-09-04 -- up to 6 pages, same per-page arithmetic as the old 2-page
// family lists, just more pages.
#ifndef UI_PAGE_PROFILES_BUILTIN_LIST_H
#define UI_PAGE_PROFILES_BUILTIN_LIST_H

#include "lvgl.h"

#include "profiles_builtin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets which firing type this screen lists next and resets paging to page 0.
 * Call BEFORE kiln_ui_show("profiles_builtin_list"). */
void ui_page_profiles_builtin_list_set_firing_type(profile_firing_type_t type);

lv_obj_t *ui_page_profiles_builtin_list_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_BUILTIN_LIST_H
