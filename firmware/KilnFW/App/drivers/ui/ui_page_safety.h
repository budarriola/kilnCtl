#ifndef UI_PAGE_SAFETY_H
#define UI_PAGE_SAFETY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LCD Safety / Alarm page (TODO.md sec 0.5). kiln_ui_page_build_fn: built
 * once, on first kiln_ui_show("safety"). Viewing is dashboard-level (no PIN);
 * the Clear Trip button is gated on an LCD admin login. What is shown and
 * whether the button is offered is derived from live safety state on every
 * refresh -- see ui_page_safety_logic.h. */
lv_obj_t *ui_page_safety_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_SAFETY_H
