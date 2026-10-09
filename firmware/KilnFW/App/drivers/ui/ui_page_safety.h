#ifndef UI_PAGE_SAFETY_H
#define UI_PAGE_SAFETY_H

#include <stdbool.h>

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

/* Show the Safety page. from_config selects where its Back button goes:
 * true only from the (PIN-gated) config hub; false from the home trip strip,
 * which is reachable WITHOUT a login. A page an unauthenticated viewer can
 * open must never link onward to a gated page (2026-09-28 owner decision:
 * dashboards only without login), so from the strip Back returns "home". */
void ui_page_safety_open(bool from_config);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_SAFETY_H
