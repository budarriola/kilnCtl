// The "tc_types" page -- TODO.md owner-report item ("in the thermocouples
// settings page i dont see anywhere i can select my thermocouple type") had
// two halves fixed the same day: the web zones page (zones_page.html)
// gained per-channel type selection, and zones_http.c gained persistence and
// a boot-time apply to the real MAX31856 register. Neither of those touched
// the LCD -- this page is that missing on-device equivalent, reached from
// the Configuration hub's third page (ui_page_config.c's
// UI_CONFIG_HUB_PAGE_COUNT went 2 -> 3 to make room for it; see that file's
// header comment for why both hub pages were already full).
//
// Deliberately its OWN page rather than a tab bolted onto
// ui_page_thermo_faults.c (also thermocouple-related): that page is a
// read-only live-fault monitor on a fast refresh timer, and this one is a
// config editor with no live-data refresh at all -- mixing the two would
// mean either running a refresh timer this page doesn't need or teaching
// the fault page to also own NVS writes, neither of which is a clean split.
// One page one file, see kiln_ui.h's header comment.
#ifndef UI_PAGE_TC_TYPES_H
#define UI_PAGE_TC_TYPES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("tc_types"). */
lv_obj_t *ui_page_tc_types_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_TC_TYPES_H
