// ui_page_zones -- LCD equivalent of the web zones page's per-zone
// thermocouple-mapping / relay-mapping / calibration / temperature-limits
// editor (TODO.md section 3 / 10.8), for the hub cell that has read
// "Zones & Thermocouples (not built yet)" until now (ui_page_config.c).
//
// Two pages, two files' worth of content in one TU (they are one feature,
// not two independently reachable destinations -- "zones_detail" is only
// ever reached via ui_page_zones_detail_prepare() + kiln_ui_show(), never
// linked to directly from the hub):
//
//   "zones"        -- ui_page_zones_build(): a list of the currently
//                      configured zones (zones_config_get_thermo_count()),
//                      one row each, tap to open that zone's detail.
//   "zones_detail" -- ui_page_zones_detail_build(): the selected zone's
//                      editor, itself split into two paged sub-screens
//                      (Assignment; Calibration & Limits) via the shared
//                      top bar's Prev/Next icons, same pattern
//                      ui_page_config.c's hub uses for its own paging.
//
// Deliberately OUT of scope, and left exactly where they already live:
//   - Thermocouple TYPE selection -- ui_page_tc_types.c already owns this
//     (a separate hub page, built same day). This page does not repeat it;
//     the Assignment sub-page's chip row is WHICH channels feed a zone, not
//     what type each channel is.
//   - Control mode, PID gains, heater timing, guard thresholds, autotune --
//     TODO.md's own "still missing" list for this page names these as
//     separate, larger, undesigned-or-partially-designed pieces (bang-bang
//     hysteresis, per-band PID/gain scheduling, most guard thresholds).
//     This pass covers exactly what was asked: thermo_mask, relay_mask,
//     cal_offset_c, max_temp_c/min_temp_c -- the four zones_http.h-backed
//     fields the web zones_page.html already exposes that had no LCD
//     equivalent at all.
#ifndef UI_PAGE_ZONES_H
#define UI_PAGE_ZONES_H

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_zones_build(void);

/* Selects which zone "zones_detail" edits and resets it to its first
 * sub-page (Assignment). Must be called before every kiln_ui_show(
 * "zones_detail") -- including the first, matching
 * ui_page_profile_builder_segment_prepare()'s "prepare, then show"
 * convention, since kiln_ui.c builds a page exactly once and this is the
 * only hook available to change which zone an already-built page reflects. */
void ui_page_zones_detail_prepare(uint8_t zone_index);

lv_obj_t *ui_page_zones_detail_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_ZONES_H
