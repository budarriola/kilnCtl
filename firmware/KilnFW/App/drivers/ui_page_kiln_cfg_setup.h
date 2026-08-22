// ui_page_kiln_cfg_setup -- the "Kiln Config" management screen, reached
// from ui_page_kiln_setup.c's "Kiln Config" cell.
//
// A KILN CONFIG is a named, saved snapshot of the WHOLE zones configuration
// (relay wiring, thermocouple assignment, PID gains, guard thresholds,
// limits -- everything, see kiln_cfg_store.h's header comment). It is NOT a
// firing profile (a temperature-vs-time schedule) -- the two are named and
// kept visually distinct everywhere on this screen precisely because
// "profile" already means the other thing in this codebase.
//
// This page: an "Active: <name>" readout, a scrollable list of saved
// configs (tap a row to select it), and the management actions that operate
// on the current selection -- Apply, Save Current As, Clone, Rename,
// Delete. See ui_page_kiln_cfg_setup.c's header comment for the full safety
// UX (confirm dialogs, apply-refusal surfacing, duplicate/length name
// checks).
#ifndef UI_PAGE_KILN_CFG_SETUP_H
#define UI_PAGE_KILN_CFG_SETUP_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_kiln_cfg_setup_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_KILN_CFG_SETUP_H
