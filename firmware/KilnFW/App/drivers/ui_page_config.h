// The "config" page -- TODO.md 10.3's "Configuration" nav item destination:
// a navigation hub to the deeper config/settings menus (section 3's Settings
// pages: Thermocouples & Zones, Relays & Rules, Network), not a settings
// editor in itself. Replaces the pre-this-pass title+Back stub (see git
// history). See ui_page_config.c's header comment for exactly which items
// are real navigation and which are honest "not built yet" placeholders --
// this pass built one real destination (Board Health, see
// ui_page_board_health.c/.h) and left the other three as labeled
// placeholders rather than half-building a settings editor. One page one
// file, see kiln_ui.h's header comment.
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
