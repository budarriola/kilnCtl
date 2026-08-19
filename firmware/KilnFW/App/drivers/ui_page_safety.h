// The "safety" page -- ROADMAP.md M6's "GUI shows safety temperature,
// enclosure temperature and power" card, moved off ui_page_home.c during
// the 2026-08-18 no-scroll rewrite (see ui_page_home.c's header comment):
// the home page's ~264px content budget (480x320 landscape, this codebase's
// actual runtime canvas per Kconfig's default startup rotation) had no room
// left for this card once zones + run-state + action row were sized to
// fit, so it now lives on its own page, reachable from
// ui_page_config.c's "Safety Processor" nav item -- same page-manager
// pattern as ui_page_board_health.c/ui_page_network.c (TODO.md 10.1).
//
// Still fed by dashboard_get_status() (TODO.md 10.1a's shared-backend
// rule), same call ui_page_home.c made for this card before the move --
// nothing here reimplements safety_link_get_status()'s cache. One page one
// file, see kiln_ui.h's header comment.
#ifndef UI_PAGE_SAFETY_H
#define UI_PAGE_SAFETY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("safety"). */
lv_obj_t *ui_page_safety_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_SAFETY_H
