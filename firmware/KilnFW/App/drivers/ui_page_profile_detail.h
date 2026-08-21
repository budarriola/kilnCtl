// ui_page_profile_detail -- one profile's detail screen: full title, segment
// count, a feasibility-coloured card, and the START action. Reached from
// ui_page_profiles_mine.c (a user slot) or ui_page_profiles_builtin_list.c
// (a builtin catalogue entry) -- either caller must call
// ui_page_profile_detail_set_id() with the id it's about to show BEFORE
// kiln_ui_show("profile_detail"), same "set state, then navigate" pattern
// ui_page_home.c's menu_nav_cb() already uses for the Config hub's paging
// state.
//
// START goes through profile_executor_run(id, ...) -- the SAME function the
// web dashboard and the home page's fire button call, via the SAME
// ui_confirm.c dialog the home page uses for its own Start confirmation. On
// refusal, the specific reason profile_executor_run() returns is shown, not
// a generic message (a thermal-guard-latched refusal and a ramp-rate-ceiling
// refusal are different problems needing different operator action).
//
// BUILDER HOOK: creating or editing a profile on the LCD is a separate,
// out-of-scope task. If that work ever lands, the natural place for an
// "Edit" action is this screen, next to START -- nothing here reserves a
// specific button slot for it (this screen only has room for one action row
// today, see the pixel arithmetic in the .c file), so that future pass will
// need its own budget accounting, not just a bolt-on button.
#ifndef UI_PAGE_PROFILE_DETAIL_H
#define UI_PAGE_PROFILE_DETAIL_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets which profile id (user slot 0..7 or builtin PROFILE_BUILTIN_ID_BASE+n)
 * this screen shows next, and which page ("profiles_mine" or
 * "profiles_family" -> "profiles_builtin_list") Back should return to.
 * back_page is remembered rather than hardcoded to one hub because this
 * screen has two distinct callers with two distinct "one level up"
 * destinations. */
void ui_page_profile_detail_set_id(uint8_t profile_id, const char *back_page);

/* The id currently shown -- ui_page_profile_segments.c reads this rather
 * than duplicating its own copy of "which profile am I showing", so the two
 * screens can never disagree about which profile they're both looking at. */
uint8_t ui_page_profile_detail_get_id(void);

lv_obj_t *ui_page_profile_detail_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILE_DETAIL_H
