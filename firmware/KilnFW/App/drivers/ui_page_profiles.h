// ui_page_profiles -- the LCD "Profiles" hub (reached from ui_page_config.c's
// Config hub). Three destinations:
//   - My Profiles     -- the 8 user slots (ui_page_profiles_mine.c)
//   - Built-ins (28)   -- firing-type picker (ui_page_profiles_family.c) -> a
//                         cone-sorted list for that type (ui_page_profiles_builtin_list.c)
//   - Restore hidden   -- profiles_builtin_restore_all(), direct call
//
// This closes the LCD gap the user asked about: today's home page Start
// button only ever runs a blind fallback profile id (current non-idle run,
// else the last boot record) -- there was no way to actually BROWSE and pick
// a profile from the panel. See ui_page_profile_detail.c for where START
// actually runs.
//
// Builder (creating/editing a profile ON the LCD) is explicitly OUT of scope
// for this pass -- see ui_page_profile_detail.c's header comment for the
// hook left for that future work.
#ifndef UI_PAGE_PROFILES_H
#define UI_PAGE_PROFILES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_profiles_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_H
