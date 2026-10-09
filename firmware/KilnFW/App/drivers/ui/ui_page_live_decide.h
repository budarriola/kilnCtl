// ui_page_live_decide -- LCD end-of-run decision for a live-edited firing.
//
// When a firing that was edited live (docs/LIVE_PROFILE_EDIT.md) ends,
// the working copy is still owed a decision: Discard it, Save it as a new
// profile, or Overwrite the original. The web does this on /live_profile;
// this is the LCD equivalent (ROADMAP: LCD save/discard of live edits).
//
// Every rule lives in profiles_live_decide_apply()
// (drivers/http/profiles_live_http.c) -- the SAME function POST
// /api/profile/live/decide calls. This page only shows buttons and maps the
// result code to text. Overwrite is disabled for a builtin origin (the web's
// 403). The LCD has no text entry, so Save as auto-names the copy
// ("<origin>-E", "-E2"...) and says so.
//
// Reached only through the PIN gate: the home page's "Keep?" button
// (ui_home_edit_btn_cb(), tools/check_lcd_home_nav_gated.ps1). Discard and
// Overwrite additionally ask for confirmation.
#ifndef UI_PAGE_LIVE_DECIDE_H
#define UI_PAGE_LIVE_DECIDE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Re-reads the pending record. Call before kiln_ui_show("live_decide").
void ui_page_live_decide_prepare(void);

lv_obj_t *ui_page_live_decide_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_LIVE_DECIDE_H
