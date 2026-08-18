// The "config" page -- TODO.md 10.3's "Configuration" nav item destination.
// Minimal stub for this pass: a title and a back-to-home button, just enough
// for ui_page_home.c's nav item to have somewhere real to go and to prove
// kiln_ui_show() actually switches between more than one page. The real
// content (Thermocouples & Zones / Relays & Rules / Network, mirroring
// section 3's web Settings pages) is future work -- one page one file, see
// kiln_ui.h's header comment.
#ifndef UI_PAGE_CONFIG_H
#define UI_PAGE_CONFIG_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("config"). */
lv_obj_t *ui_page_config_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_CONFIG_H
