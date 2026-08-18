// The "temperature" page -- TODO.md 10.3's "Temperature" nav item
// destination: real per-zone manual relay control, the touchscreen
// equivalent of section 2's web dashboard manual relay override
// (dashboard_http.c's POST /api/relay). Per TODO.md 10.1a, every write here
// goes through dashboard_set_relay() -- the exact same ownership/safety gate
// and kiln_io write POST /api/relay uses, extracted from relay_post_handler()
// this pass -- not a reimplementation. See ui_page_temperature.c's header
// comment for what's real vs. still a known gap. One page one file, see
// kiln_ui.h's header comment.
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
