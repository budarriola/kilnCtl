// LCD diagnostics/system-info page -- TODO.md's "Diagnostics / System info
// page" item, ESP-only half ("firmware version, ESP heap/flash-free, IC
// temperatures... buildable now" per that item's own text). The
// safety-link-stats half of that item is still blocked on M5 and not
// attempted here -- see this file's .c for exactly what is/isn't shown.
#ifndef UI_PAGE_DIAGNOSTICS_H
#define UI_PAGE_DIAGNOSTICS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first
 * kiln_ui_show("diagnostics"). */
lv_obj_t *ui_page_diagnostics_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_DIAGNOSTICS_H
