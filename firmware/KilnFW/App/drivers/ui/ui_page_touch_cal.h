// Touch calibration page for the NS2009. Shows a 3x3 grid of targets one at
// a time; ANY press while a target is showing counts as that target's
// sample (see ui_page_touch_cal.c's overlay_press_cb comment for why this
// doesn't rely on normal button hit-testing, which would need an already-
// working calibration to do accurately -- exactly what doesn't exist yet).
// After the last point, fits a 2D affine raw-to-screen transform
// (touch_cal_store.h) and persists it to NVS; kiln_ui.c boots straight into
// this page instead of "home" until that persisted calibration exists.
// Kept as a permanent page (reachable from Configuration, same as Board
// Health/Network/Safety/History) so re-calibration is possible again later,
// e.g. after a panel swap.
#ifndef UI_PAGE_TOUCH_CAL_H
#define UI_PAGE_TOUCH_CAL_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first
 * kiln_ui_show("touch_cal"). */
lv_obj_t *ui_page_touch_cal_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_TOUCH_CAL_H
