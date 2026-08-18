// The "home" page -- TODO.md 10.3's main/status page: per named zone
// (zones_config_get_name(), falling back to "Zone N") its temp/heater
// status, running profile name/state, segment elapsed/remaining time (text +
// progress bar), a profile picker, a desired-vs-actual temperature chart,
// Start/Stop, and Configuration/Temperature nav buttons. Data and actions go
// through the same plain-C getters/actions dashboard_http.c's HTTP handlers
// use (TODO.md 10.1a) -- see ui_page_home.c's header comment. One page one
// file (see kiln_ui.h's header comment on why pages are split out this way).
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
