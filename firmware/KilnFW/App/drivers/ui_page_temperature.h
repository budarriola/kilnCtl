// The "temperature" page -- TODO.md 10.3's "Temperature" nav item
// destination (individual per-zone manual control, touchscreen equivalent of
// the web dashboard's manual relay override / per-zone target). Minimal stub
// for this pass: a title and a back-to-home button, same status as
// ui_page_config.h -- the real content is future work. One page one file,
// see kiln_ui.h's header comment.
#ifndef UI_PAGE_TEMPERATURE_H
#define UI_PAGE_TEMPERATURE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("temperature"). */
lv_obj_t *ui_page_temperature_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_TEMPERATURE_H
