// The "config" page -- TODO.md 10.3's "Configuration" nav item destination:
// a navigation hub to the deeper config/settings menus, not a settings
// editor in itself. Replaces the pre-this-pass title+Back stub (see git
// history). See ui_page_config.c's header comment for the current list of
// destinations -- it has changed several times since this file was written
// (most recently 2026-08-22: Zones & Thermocouples, Relays & Rules, and
// Temperature History were removed outright, per owner request, as the LCD
// shed config duties now covered by the web GUI). One page one file, see
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

/* Rewinds the hub to its first page. The hub is built once and kept, so its
 * paging position otherwise persists for the life of the boot -- which is
 * right when you press Back from a sub-page (you return to the page you left
 * from) and wrong when you press Menu from the home page (you expect the top
 * of the menu, not wherever you happened to be last time). Call this before
 * kiln_ui_show("config") for the Menu case only. Safe before the page has
 * been built. */
void ui_page_config_reset_to_first_page(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_CONFIG_H
