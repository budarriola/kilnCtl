// ui_page_profiles_family -- the builtin-catalogue family picker: Bartlett /
// Plainsman / Crystalline / General. Exists because 28 builtin schedules at
// 4-per-page is 7 flat pages, a bad browse on a 480x320 no-scroll panel; the
// four families (builtin_profile_t.family, see profiles_builtin.h and
// tools/scripts/gen_builtin_profiles.py's FAMILIES dict) split that into
// four short lists of at most 8 entries (2 pages) each.
#ifndef UI_PAGE_PROFILES_FAMILY_H
#define UI_PAGE_PROFILES_FAMILY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_profiles_family_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_FAMILY_H
