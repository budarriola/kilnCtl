// The "simplified Tkinter" app-layer TODO.md 10.1 asks for, on top of raw
// LVGL: a page registry and an active-page switch, so 10.3's real pages
// (main/status, settings, temperature, config) each get to be one
// self-contained build function instead of every caller of lvgl_port.c
// hand-rolling lv_obj_t trees and lv_screen_load() calls.
//
// ONE PAGE, ONE FILE. kiln_ui.c/.h (this module) is the registry/switcher
// only -- it must never itself contain a kiln_ui_page_build_fn body. Every
// page gets its own ui_page_<name>.c/.h pair (see ui_page_home.c/.h for the
// pattern: a single `lv_obj_t *ui_page_<name>_build(void)` exported from the
// header, registered from kiln_ui_init() or wherever else needs it). This is
// deliberate, not a style nit: a kiln with several pages (10.3 lists at
// least home/settings/temperature/config) growing them all inside kiln_ui.c
// would turn one file into the entire UI's merge-conflict surface and make
// "which page does what" a grep through one huge file instead of a file
// listing.
//
// Deliberately small. What this does NOT do yet, left for 10.3 to add as it
// builds real pages: global chrome (a persistent nav bar / back button
// shared across pages, per TODO.md 10.1's "the screen manager swaps the
// active page and handles global chrome" line), page teardown/memory reuse
// (pages are built once on first show and kept alive forever -- fine for a
// handful of pages on an 8 MB-PSRAM board, revisit only if the page count
// grows enough to matter), and any transition animation (lv_screen_load, not
// lv_screen_load_anim).
//
// Single-threaded, like everything else touching LVGL: every function here
// must only be called from the same task lvgl_port.c runs lv_timer_handler()
// on (see lvgl_port.h's header comment) -- there is no lock here because
// there is deliberately only one caller.
#ifndef KILN_UI_H
#define KILN_UI_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds and returns a page's root screen object (an lv_obj_t created with
 * lv_obj_create(NULL) or similar, NOT attached to anything yet -- kiln_ui
 * owns calling lv_screen_load() on it). Called exactly once per page, the
 * first time that page is shown; the returned object is cached and reused
 * for every later kiln_ui_show() of the same page. */
typedef lv_obj_t *(*kiln_ui_page_build_fn)(void);

/* Resets the page registry (empty) and registers/shows a built-in "home"
 * placeholder page -- the same "kilnCtl" label lvgl_port.c used to build
 * directly, now living here as page 0 so there's always something on screen
 * even before 10.3 registers anything real. Call once, from lvgl_port_start,
 * after the display and indev are attached but before lvgl_port_task starts. */
esp_err_t kiln_ui_init(void);

/* Adds a page under `name` (must be unique; not copied, so pass a string
 * literal or something that outlives the UI). Does not build or show it --
 * building is deferred to the first kiln_ui_show() of this name, so
 * registering every page up front at boot doesn't pay for building the ones
 * never visited this session. */
esp_err_t kiln_ui_register_page(const char *name, kiln_ui_page_build_fn build);

/* Builds `name`'s page if this is the first time it's been shown, then
 * lv_screen_load()s it. ESP_ERR_NOT_FOUND if no page was registered under
 * that name. */
esp_err_t kiln_ui_show(const char *name);

/* NULL until the first kiln_ui_show() (or kiln_ui_init()'s own "home"
 * show) succeeds. */
const char *kiln_ui_current_page(void);

#ifdef __cplusplus
}
#endif

#endif // KILN_UI_H
