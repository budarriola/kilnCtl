// ui_page_edit_firing -- LCD page for editing the CURRENTLY RUNNING/PAUSED
// firing, the LCD equivalent of the web's /live_profile page
// (docs/LIVE_PROFILE_EDIT_PLAN.md) and its "Edit firing" button (d484e51a).
//
// Owner request 2026-09-28: "Can you make a simple screen for the lcd that
// also allows modifying the current firing like the web does." This is
// deliberately the SIMPLE first version: pick a segment (paged, no
// scrolling), adjust target/ramp/dwell with +/- steppers, Apply. All
// validation is the SAME C code the HTTP routes call
// (profiles_validate_candidate(), live_edit_check_window(),
// live_profile_fork()/_save_working() in firmware/KilnFW/App/drivers/persist/
// live_profile.h) -- this page never re-implements a rule, it only calls
// through to it and reports whatever reason it refuses with.
//
// NOT in this version, by design (owner: "leave the working copy's end-of-
// run decision to the web"): no save-as/overwrite UI. If the backend ever
// requires a decision to be resolved from this page (it does not today --
// discard/save_as/overwrite is prompted only once the run is no longer
// RUNNING/PAUSED, i.e. after the LCD's Edit button is already hidden), the
// default is DISCARD, never overwrite.
//
// Reached only through the PIN gate off the home page (ui_page_home_actions.c's
// ui_home_edit_btn_cb()) -- see tools/check_lcd_home_nav_gated.ps1.
#ifndef UI_PAGE_EDIT_FIRING_H
#define UI_PAGE_EDIT_FIRING_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Re-reads live status/the working (or origin) profile content and resets
// paging to the currently-running segment. Call every time BEFORE
// kiln_ui_show("edit_firing") -- same "_prepare() before show()" idiom as
// ui_page_profile_builder_segment_prepare().
void ui_page_edit_firing_prepare(void);

lv_obj_t *ui_page_edit_firing_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_EDIT_FIRING_H
