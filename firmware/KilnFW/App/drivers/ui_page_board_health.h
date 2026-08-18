// The "board_health" page -- TODO.md 10.7's "onboard IC temperature" LCD
// side, linked from ui_page_config.c's Configuration nav hub. New this
// pass; 10.7 built the GET /api/board_temps JSON endpoint (board_temps.c/.h)
// but explicitly left "the LCD/LVGL nav item this section also asks for" as
// open work -- this page is that nav item.
//
// Deliberately its own page, not folded into the main dashboard
// (ui_page_home.c) or Configuration hub itself -- 10.7's own reasoning is
// that board-health diagnostic data (silicon temperature) mixed into the
// same page as kiln-process data (thermocouple readings) makes the main
// page harder to read at a glance, and the web side already made the same
// call (a separate route, not folded into the main dashboard). See
// ui_page_board_health.c's header comment for the data source and the
// null/unavailable handling. One page one file, see kiln_ui.h's header
// comment.
#ifndef UI_PAGE_BOARD_HEALTH_H
#define UI_PAGE_BOARD_HEALTH_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first
 * kiln_ui_show("board_health"). */
lv_obj_t *ui_page_board_health_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_BOARD_HEALTH_H
