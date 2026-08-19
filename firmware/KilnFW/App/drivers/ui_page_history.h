// The "history" page -- TODO.md 10.3's desired-vs-actual temperature chart,
// moved off ui_page_home.c during the 2026-08-18 no-scroll rewrite (see
// ui_page_home.c's header comment): the home page's ~264px content budget
// (480x320 landscape, this codebase's actual runtime canvas) had no room
// left for a 140px-tall lv_chart plus legend once zones + run-state +
// action row were sized to fit, so the chart now lives on its own page,
// reachable from ui_page_config.c's "Temperature History" nav item -- same
// page-manager pattern as ui_page_board_health.c/ui_page_network.c
// (TODO.md 10.1).
//
// Still fed by profile_executor_get_history()/_get_history_count()
// (TODO.md 10.1a's shared-backend rule), same calls ui_page_home.c made for
// this chart before the move. One page one file, see kiln_ui.h's header
// comment.
#ifndef UI_PAGE_HISTORY_H
#define UI_PAGE_HISTORY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("history"). */
lv_obj_t *ui_page_history_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_HISTORY_H
