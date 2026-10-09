// ui_page_profiles -- the LCD "Profiles" page (reached from ui_page_config.c's
// Config hub). A thin alias for ui_page_profile_picker.c's MANAGE mode: the
// unified, favorites-first, paginated list of every profile (user slots plus
// the builtin catalogue), with a New icon in the topbar and a per-row Delete
// for user slots. UI_PLAN.md Section 6.2 replaced the old four-cell hub
// (My Profiles/Built-ins/Restore hidden/New Profile) and its two sub-pages
// with this single list -- "Restore hidden" is web-only now (owner decision,
// 6.2's "tests owed" section).
//
// See ui_page_profile_detail.c for where START actually runs, and
// ui_page_profile_builder_zones.h for the on-LCD create/edit flow the New
// icon and each row's detail screen hand off to.
#ifndef UI_PAGE_PROFILES_H
#define UI_PAGE_PROFILES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_profiles_build(void);

/* Resets to page 0 and reloads/reorders ids. Callers navigating INTO
 * "profiles" must call this first -- kiln_ui_show() caches the page after
 * its first build, so a stale render would otherwise survive a delete/import
 * made elsewhere (e.g. from the web dashboard). */
void ui_page_profiles_refresh(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_H
