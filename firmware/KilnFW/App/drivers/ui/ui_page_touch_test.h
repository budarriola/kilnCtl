// Touch calibration verification page. Shown immediately after
// ui_page_touch_cal.c finishes and saves a calibration (see kiln_ui.c's
// registration and ui_page_touch_cal.c's finish_calibration()) -- draws a
// square guide and lets the user trace it with a finger, drawing a line
// that follows the drag in real time. The drawn line uses the SAME
// calibrated raw-to-screen mapping every other page's touch handling uses
// (lvgl_port.c's touch_read_cb), so a line that visibly strays from the
// square is the calibration reading back wrong, not a separate check path
// that could pass while the real mapping is still off.
#ifndef UI_PAGE_TOUCH_TEST_H
#define UI_PAGE_TOUCH_TEST_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first
 * kiln_ui_show("touch_test"). */
lv_obj_t *ui_page_touch_test_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_TOUCH_TEST_H
