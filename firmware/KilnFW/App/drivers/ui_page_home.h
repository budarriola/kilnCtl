// The "home" page -- TODO.md 10.3's main/status page. Currently still just
// the pre-10.2 placeholder ("kilnCtl" label); this file is where 10.3's real
// zone list / graph / profile controls land, one page one file (see
// kiln_ui.h's header comment on why pages are split out this way).
#ifndef UI_PAGE_HOME_H
#define UI_PAGE_HOME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("home"). */
lv_obj_t *ui_page_home_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_HOME_H
