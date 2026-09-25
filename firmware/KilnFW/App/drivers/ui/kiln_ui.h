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

#include <stdbool.h>
#include <stdint.h>

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

/* Re-emits the tap-target dump (widget rectangles, centre points and labels)
 * for whatever screen is currently loaded. kiln_ui_show() already does this
 * on every navigation, so this is only for the case where what is tappable
 * changes WITHOUT a page switch -- ui_page_config.c's paged hub swapping
 * which set of cells is visible being the reason it exists. Without it, a
 * Prev/Next press silently changes every tap target on the screen and the
 * last dump on record becomes wrong, which is worse than no dump at all.
 *
 * NOT safe to call from any task: this recurses the live LVGL tree and
 * calls ESP_LOGI at every node, which only fits on lvgl_port_task's 8192 B
 * stack (bench-reproduced IllegalInstruction panic, exc_task='touch_uart_
 * brid', 2026-09-19, when a caller on a smaller stack called this
 * directly). Callers on any other task must request the dump instead,
 * via lvgl_port_request_tap_dump(), which flags lvgl_port_task to run this
 * function on its own next loop tick. This function itself is only ever
 * meant to be called from lvgl_port_task (or another caller already known
 * to be running on that task's stack with sole LVGL ownership). Does
 * nothing if no screen is loaded yet. Intended for bench/diagnostic use,
 * not for anything on a hot path -- see the volume note on
 * kiln_ui_show()'s own dump. */
void kiln_ui_log_tap_targets(void);

/* Turns the AUTOMATIC tap-target dump inside kiln_ui_show() on/off -- off by
 * default. That automatic dump is a per-page-switch flood risk (see
 * kiln_ui.c's s_auto_tap_dump comment); kiln_ui_log_tap_targets() above is
 * unaffected by this flag and always dumps on request. Wired to a TOUCH
 * bridge subcommand in uart_bridge.c so a PC client can turn the automatic
 * dump on only while it is actually driving navigation and wants every
 * switch's targets logged without an explicit call after each one. */
void kiln_ui_set_auto_tap_dump(bool enable);

/* Pull-based kiln_ui_show() entry/exit counters, wired into
 * TOUCH_CMD_GET_STATE's reply by uart_bridge.c -- see the s_show_entries /
 * s_show_exits declaration comment in kiln_ui.c. Safe to call from any task
 * (single-word reads of counters written only from kiln_ui_show()'s own
 * task). Either argument may be NULL. */
void kiln_ui_get_show_diag(uint32_t *show_entries, uint32_t *show_exits);

/* One entry from a kiln_ui_collect_tap_targets() walk -- the same data
 * log_tap_targets()/log_all_tap_targets() (kiln_ui.c) already print, just
 * captured into an array instead of (or in addition to) ESP_LOGI, for
 * UART_TASK_ID_UI_TEST (uart_bridge.c) to hand a PC-side test harness. `name`
 * is the nearest child label text (a button's caption, or a buttonmatrix
 * key's text), truncated to fit; "" if the target has none. `cx`/`cy` are the
 * post-layout centre point an injected touch should aim at. `hidden` mirrors
 * the LV_OBJ_FLAG_HIDDEN check the log walk already applies. */
typedef struct {
    char name[32];
    int16_t cx;
    int16_t cy;
    bool hidden;
} kiln_ui_tap_target_t;

/* Tag wrapper for an optional tap-name override stashed in a button's own
 * lv_obj user data (ui_topbar.c's build_icon_named()). Deliberately not a
 * bare `const char *`: log_tap_targets() (kiln_ui.c) would otherwise have to
 * read user_data off every clickable object it walks, including non-button
 * ones whose user_data holds something else entirely -- ui_confirm.c's
 * msgbox stores a heap `ui_confirm_ctx_t *` there, and while the reader below
 * additionally *only* looks at this field on an object confirmed to be
 * `&lv_button_class` (the msgbox never is), the magic word is checked too as
 * defence in depth before `name` is ever trusted, in case a future button
 * reuses user_data for something else. Lives in this shared header, not
 * kiln_ui.c or ui_topbar.c alone, because both files must agree on the exact
 * layout. Instances live in rodata (`static const`), never .bss/.data --
 * zero RAM cost, unlike CONFIG_LV_USE_OBJ_NAME's per-lv_obj pointer, which
 * this deliberately avoids enabling. */
#define KILN_UI_TAP_NAME_MAGIC 0x4B54414Eu /* "KTAN" */
typedef struct {
    uint32_t magic;
    const char *name;
} kiln_ui_tap_name_tag_t;

/* Walks the same tree log_all_tap_targets() does (active screen + top/sys
 * layers) and fills `out` with up to `max` targets, returning the number
 * written. If the walk finds more than `max` targets, `*truncated` (may be
 * NULL) is set true and the rest are dropped -- callers sizing `out` for a
 * wire reply should check it rather than assume `out` saw everything.
 * This function itself still reads live LVGL objects and so must still only
 * ever be CALLED from lvgl_port_task, same as any other lv_* reader -- but
 * as of the 2026-09-24 uart_bridge_ui_test owner-task fix, neither of its
 * two real callers (UI_TEST_CMD_LIST_TAP_TARGETS in uart_bridge_ui_test.c,
 * and kiln_ui_click_by_name() below) calls it directly from their own task
 * any more. Both now go through lvgl_port_collect_tap_targets()
 * (lvgl_port.h), which dispatches the call onto lvgl_port_task and blocks
 * the ORIGINAL caller (never lvgl_port_task itself) on a bounded-wait
 * completion -- the same shape lvgl_port_request_tap_dump() uses for
 * kiln_ui_log_tap_targets(), extended with a reply since these two callers
 * need the actual target list/match result back, not just a fire-and-forget
 * dump. This closes the pre-existing thread-safety gap the 3f86e899
 * TOUCH_CMD_LOG_TAP_TARGETS fix left open here (see that fix's own history
 * for the IllegalInstruction panic it fixed on the sibling path). This
 * function's own body is unchanged -- lvgl_port.c's dispatcher calls this
 * exact function, from lvgl_port_task, when servicing a request. */
size_t kiln_ui_collect_tap_targets(kiln_ui_tap_target_t *out, size_t max, bool *truncated);

typedef enum {
    KILN_UI_CLICK_OK,
    KILN_UI_CLICK_NOT_FOUND,
    KILN_UI_CLICK_AMBIGUOUS,
    KILN_UI_CLICK_HIDDEN,
    /* 2026-09-24: the injected press was actually delivered to LVGL but
     * screen_idle_touch_swallow() swallowed it (display was OFF, or an
     * ERROR_HOLD dismissal -- see screen_idle.h) -- so nothing UNDER the tap
     * target was acted on, even though the target itself was found and
     * visible. Distinct from KILN_UI_CLICK_OK precisely so a bench harness
     * can tell "this click landed and did nothing because it was a wake/
     * dismiss tap" apart from "this click landed and the page just didn't
     * change", which used to look identical (see the 20260924T233113Z_lcd
     * bench log's unexplained double swallow this was added to diagnose). */
    KILN_UI_CLICK_SWALLOWED,
    /* 2026-09-24 follow-up: the bounded wait below for lvgl_port_get_
     * inject_verdict() can itself time out (a slow LVGL flush -- a
     * full-screen redraw, an SPI stall -- can exceed the wait window even
     * though the press WAS delivered and will be reflected once the next
     * poll catches up). Before this result existed, that timeout fell back
     * to "not swallowed" and kiln_ui_click_by_name() reported plain
     * KILN_UI_CLICK_OK -- indistinguishable from a press that was
     * genuinely delivered and confirmed NOT swallowed. KILN_UI_CLICK_
     * VERDICT_UNKNOWN names that second case explicitly: the target was
     * found, visible, and a press+release WAS injected, but whether
     * screen_idle_touch_swallow() swallowed it could not be confirmed
     * within the wait window. A caller must treat this as neither a pass
     * nor a genuine defect -- see ui_test_client.py's result-name table. */
    KILN_UI_CLICK_VERDICT_UNKNOWN,
    /* 2026-09-24 follow-up: lvgl_port_inject_touch() itself returned 0 (its
     * documented "never queued" sentinel -- either lvgl_port_start() hasn't
     * run yet, or touch_inject_lock() timed out) for the PRESS half, before
     * any wait for a swallow verdict began. No press was ever queued, so
     * there is nothing to wait for and nothing to release -- unlike
     * KILN_UI_CLICK_VERDICT_UNKNOWN, this is not "a press was sent and we
     * couldn't confirm it," it is "no press was sent at all." A caller must
     * treat this the same as NOT_FOUND/AMBIGUOUS/HIDDEN: never a pass, and
     * never grounds to poll for a page change the same click could not have
     * caused. */
    KILN_UI_CLICK_INJECT_FAILED,
    /* 2026-09-25: the bench run in logs/bench_test/20260925T150107Z_lcd found
     * a tap target whose reported centre lay OUTSIDE the display (the old,
     * overflowing PIN keypad's title/footer -- see ui_lcd_keypad.c) still
     * came back KILN_UI_CLICK_OK: LVGL's own touch pipeline does not clamp
     * an injected point to the panel, and lv_indev_search_obj()'s hit test
     * simply fails silently for a point off every widget's box, so the
     * press was "delivered" without landing on anything -- a false pass, not
     * a caught defect. Distinct from KILN_UI_CLICK_HIDDEN (a real, on-screen
     * widget that LVGL has flagged not visible): this is a target whose
     * *coordinates* are off-panel regardless of its hidden flag. Checked
     * before the hidden/visible split in kiln_ui_click_by_name() so an
     * off-screen widget is reported as off-screen even if it also happens to
     * be hidden. */
    KILN_UI_CLICK_OFFSCREEN,
} kiln_ui_click_result_t;

/* Finds the tap target whose name exactly matches `name` (kiln_ui_collect_
 * tap_targets() above) and, on a clean single visible match, injects a
 * press then a release at its centre through the same lvgl_port_inject_
 * touch() path UART_TASK_ID_TOUCH's INJECT subcommand uses (uart_bridge.c) --
 * so a click-by-name test step exercises exactly the same LVGL input pipeline
 * a coordinate-based injection does, not a shortcut around it.
 *
 * `out_cx`/`out_cy` (either may be NULL) are always filled with the first
 * match's centre when one is found, even for a non-OK result, so a caller
 * can report where the ambiguous/hidden match actually is:
 *   KILN_UI_CLICK_NOT_FOUND  -- no target has this name; out_cx/out_cy unset
 *   KILN_UI_CLICK_AMBIGUOUS -- more than one VISIBLE target has this name;
 *                              nothing is injected
 *   KILN_UI_CLICK_HIDDEN    -- the (first) match is hidden; nothing injected
 *   KILN_UI_CLICK_SWALLOWED -- exactly one visible match; press+release sent,
 *                              but screen_idle swallowed the press (a wake or
 *                              error-hold dismissal) -- it never reached the
 *                              widget underneath
 *   KILN_UI_CLICK_OK        -- exactly one visible match; press+release sent
 *   KILN_UI_CLICK_VERDICT_UNKNOWN -- exactly one visible match; press+release
 *                              sent, but the bounded wait for screen_idle's
 *                              swallow verdict timed out before it could be
 *                              read -- neither confirmed delivered-clean nor
 *                              confirmed swallowed
 *   KILN_UI_CLICK_INJECT_FAILED -- exactly one visible match, but
 *                              lvgl_port_inject_touch() itself refused the
 *                              press (returned 0) -- nothing was ever
 *                              queued, so no wait was attempted and no
 *                              release was sent
 *   KILN_UI_CLICK_OFFSCREEN -- the (first) match's centre lies outside the
 *                              display; nothing is injected, checked ahead of
 *                              the hidden/visible split above
 * Called directly from the UART bridge task, same as lvgl_port_inject_
 * touch() itself and TOUCH_CMD_INJECT's handler -- see that function's
 * thread-safety note (lvgl_port.h). */
kiln_ui_click_result_t kiln_ui_click_by_name(const char *name, int16_t *out_cx, int16_t *out_cy);

#ifdef __cplusplus
}
#endif

#endif // KILN_UI_H
