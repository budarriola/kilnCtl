// ui_page_profiles_builtin_list -- the builtin catalogue entries for ONE
// family (set by ui_page_profiles_family_build()'s cells), paginated
// 4-per-page same as ui_page_profiles_mine.c. At most 8 entries per family
// (see this pass's FAMILIES split), so at most 2 pages.
#ifndef UI_PAGE_PROFILES_BUILTIN_LIST_H
#define UI_PAGE_PROFILES_BUILTIN_LIST_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets which family this screen lists next and resets paging to page 0.
 * `family` is a borrowed pointer -- ui_page_profiles_family.c passes string
 * literals with static-duration lifetime, so no copy is taken (same
 * convention profiles_builtin.h's own const char* fields already use). Call
 * BEFORE kiln_ui_show("profiles_builtin_list"). */
void ui_page_profiles_builtin_list_set_family(const char *family);

lv_obj_t *ui_page_profiles_builtin_list_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_BUILTIN_LIST_H
