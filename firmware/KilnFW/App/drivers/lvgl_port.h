// LVGL bring-up for the ILI9488/NS2009 touchscreen (TODO.md section 10.1).
//
// This is the on-device screen/page/widget owner. Per the 2026-08-17
// decision recorded in TODO.md 10.1, LVGL REPLACES the UART-remote-control
// display path (uart_bridge_start_display_task/DISPLAY_CMD_*) rather than
// coexisting with it -- two owners issuing draw calls to the same
// ILI9488Class at the same time was never going to end well, and the
// on-device UI is the one that actually has to run whether or not a PC is
// attached. main.c no longer calls uart_bridge_start_display_task when this
// module is started.
//
// Touch ownership: NS2009 is read from exactly one place once this module
// starts -- the LVGL input-device callback in lvgl_port.c -- not from
// screen_idle_task's own polling loop. screen_idle keeps its documented
// blank/wake contract (screen_idle.h: "wake is just a flag flip ... whatever
// owns the UI is responsible for repainting real content once it sees
// screen_on go back to true") by being handed touch = NULL at init and fed
// real presses through screen_idle_inject_touch() from here instead. This
// module is the "whatever owns the UI" screen_idle.h refers to: its flush
// callback watches for the off->on edge and invalidates the active screen so
// the repaint screen_idle expects actually happens (see lvgl_port.c).
//
// Draw buffers live in PSRAM (heap_caps_malloc(..., MALLOC_CAP_SPIRAM)),
// per the 2026-08-17 reversal of TODO.md 9.1a's "PSRAM stays off" decision --
// this is the exact trigger 9.1a itself named ("a locally-rendered UI on the
// ILI9488"). Safe to do because ILI9488_blit_data() reads its caller buffer
// with the CPU and stages the RGB565->RGB666 conversion into the driver's
// own internal DMA-capable scratch (ILI9488.c) -- the LVGL buffer itself
// never has to be DMA-capable, so none of 9.1a's "every DMA buffer needs
// auditing" concern applies to it.
#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "panel_spi.h"
#include "NS2009.h"
#include "screen_idle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `display` must already be up (ILI9488_start succeeded) -- this is the only
 * hard requirement. `touch` may be NULL (no touch hardware; the UI is then
 * view-only until something injects touches some other way). `idle` may be
 * NULL (no auto-blank integration -- LVGL just draws continuously); when
 * non-NULL it must already be screen_idle_init'd with touch = NULL, since
 * this module becomes the sole NS2009 reader and feeds it through
 * screen_idle_inject_touch() instead.
 *
 * Brings up lv_init(), the display driver (partial-redraw, PSRAM buffers),
 * the pointer input device (if `touch`), a 1ms lv_tick source, builds a
 * placeholder boot screen, and starts the task that drives
 * lv_timer_handler(). Everything after this call happens on that one task --
 * LVGL is not thread-safe and nothing else may call an lv_* function. */
esp_err_t lvgl_port_start(ILI9488Class *display, NS2009Class *touch, screen_idle_t *idle);

/* Raw NS2009 reading (pre swap/invert transform) behind the press that most
 * recently drove an LVGL event -- ui_page_touch_cal.c's whole point is
 * pairing a known on-screen target with the raw ADC counts that produced it,
 * and correlating that from the general device log (interleaved with
 * whatever else is logging, timestamp-matched by eye) has proven unreliable
 * in practice. Reading this from a widget's LV_EVENT_CLICKED handler is
 * safe: both run on lvgl_port_task, so there's no concurrent writer. */
void lvgl_port_get_last_raw_touch(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1);

/* Re-reads the persisted calibration from NVS and swaps it in for
 * touch_read_cb() to use on the very next press -- called by
 * ui_page_touch_cal.c right after touch_cal_store_save() so a freshly
 * completed calibration takes effect immediately, no reboot needed. */
void lvgl_port_reload_touch_cal(void);

/* True once a real per-board calibration is loaded and touch_read_cb() is
 * running touch_cal_apply() against it; false while it is still on the
 * Kconfig swap/invert bootstrap guess (see touch_read_cb()'s "else" branch,
 * lvgl_port.c). That guess is known-inaccurate and was only ever meant to
 * get a finger onto ui_page_touch_cal.c's full-screen "any press counts"
 * capture during the forced first-run flow -- small edge controls (the
 * topbar back/home icons, 35x25 px in the corner) are exactly what misses
 * first under it while big central buttons still roughly land, so a board
 * stuck uncalibrated reads to an operator as "some buttons are dead", not
 * as "touch is uncalibrated". 2026-08-27: this state was previously visible
 * nowhere except one INFO log line inside lvgl_port_reload_touch_cal(),
 * itself only reachable by finishing a calibration run -- a board that had
 * NEVER been calibrated logged nothing about it, ever. This getter plus its
 * boot-time log line and /api/status field close that hole; see
 * lvgl_port.c's s_touch_cal declaration comment for the mirrored detail.
 *
 * Thread safety: matches lvgl_port_get_last_raw_touch() above -- s_touch_cal
 * is written only from lvgl_port_task (lvgl_port_start() at boot,
 * lvgl_port_reload_touch_cal() after a calibration run, both on that task),
 * so reading the single bool here from another task (dashboard_http's HTTP
 * task, in particular) is safe: worst case is one stale read behind the
 * most recent write, same staleness any other pull-based status field in
 * this codebase accepts. */
bool lvgl_port_touch_is_calibrated(void);

/* True if the touch controller wired in at lvgl_port_start() is
 * self-calibrating (touch_dev.h) -- i.e. it reports panel coordinates
 * directly and never runs through touch_cal_store.h's per-board affine fit,
 * so kiln_ui.c's boot-time forced-calibration gate must not send it into a
 * 3x3 grid it can never complete (DISPLAY_ST7796_PLAN.md section 7) and
 * ui_page_touch_cal.c should say so rather than present that grid. Always
 * false on every board that exists today (this signature only ever receives
 * an NS2009) -- true only once something wires in an FT6336U-backed
 * touch_dev_t (FT6336U.h), which nothing in this firmware does yet. */
bool lvgl_port_touch_is_self_calibrating(void);

/* Ignores every touch while disabled -- kiln_ui.c wraps a page switch in
 * this (disable, load + force a synchronous render/flush, re-enable) so a
 * tap landing during the switch can't be read against the outgoing screen's
 * stale layout and fire a click on whatever widget happens to occupy that
 * same pixel on the incoming page. Only lvgl_port_task calls lv_*, so this
 * is safe to call from an LV_EVENT_CLICKED handler (same task). */
void lvgl_port_set_input_enabled(bool enabled);

/* Feeds a synthetic touch into LVGL's input pipeline from the UART bridge's
 * TOUCH_CMD_INJECT handler (uart_bridge.c) -- the fix for the long-standing
 * gap where injected x/y went nowhere (previously only
 * screen_idle_inject_touch() saw them, and only to reset the idle timer).
 *
 * Coordinate space: SCREEN PIXELS, post-calibration -- i.e. exactly what
 * touch_read_cb() would hand LVGL after applying touch_cal_apply() to a real
 * NS2009 reading. This is a deliberate choice, not the raw-ADC space the
 * physical path starts from: a test harness wants to say "tap the button at
 * (240,160)" against the same coordinate system every ui_page_*.c already
 * lays widgets out in, not against a board-specific, orientation-dependent
 * ADC range that only touch_cal_store.h's transform knows how to interpret
 * (and that varies with a calibration the harness has no reason to run
 * through first). Bypassing the transform also means injection keeps working
 * identically whether or not this particular board has been calibrated yet.
 *
 * Thread safety: this may be called from ANY task (the UART bridge task,
 * specifically) -- it only ever writes a small lock-protected struct, never
 * an lv_* API. touch_read_cb() (lvgl_port.c), which runs exclusively on
 * lvgl_port_task, is the only reader/consumer, matching every other
 * cross-task data handoff in this codebase (thermo_owner.c / kiln_io_owner.c
 * style: one owner task, lock-guarded writes from outside it).
 *
 * Press/release lifecycle: `pressed = true` latches an active injected press
 * that touch_read_cb() will keep reporting, at the given (x, y), on every
 * poll until either a `pressed = false` call arrives (a clean release -- one
 * press + one release yields exactly one LVGL click, same as a real tap) or
 * INJECTED_TOUCH_AUTO_RELEASE_MS elapses with no follow-up call at all (see
 * lvgl_port.c) -- a safety net against a test script that injects a press
 * and then crashes, disconnects, or simply forgets the matching release,
 * which would otherwise wedge the UI in a permanently-pressed state (a stuck
 * button, or an unreleased drag) until the board is reset. A drag is just a
 * press followed by however many more `pressed = true` calls with updated
 * (x, y) the caller wants (each one keeps the press alive and moves LVGL's
 * tracked point, exactly like a finger sliding), then one final release.
 *
 * Interaction with a real finger: an injected press takes priority over the
 * NS2009 for as long as it is active (see touch_read_cb()) -- deliberate, so
 * an automated test run isn't fighting stray physical touches on the bench
 * for control of the same indev. The physical path resumes automatically the
 * moment there is no active injected press (never pressed, cleanly released,
 * or auto-released). Wake/idle-timer behavior is unaffected either way: both
 * the physical and injected paths still call screen_idle_inject_touch()
 * (uart_bridge.c calls it directly for the injected path, alongside this
 * function, since it needs to fire even before the LVGL side has resolved a
 * hit-test) -- and neither path checks screen_idle's blanked/awake state
 * before hit-testing, matching the existing physical-touch behavior: a wake
 * tap also activates whatever it lands on underneath, intentionally (see
 * touch_read_cb's comment for why this isn't gated). */
void lvgl_port_inject_touch(uint16_t x, uint16_t y, bool pressed);

/* Pull-based touch/input diagnostics -- see the s_input_enabled /
 * s_touch_read_cb_count / s_injected_delivered_count declaration comment in
 * lvgl_port.c for what each counter means and why it replaced push-based
 * (log-line) evidence. Any argument may be NULL. Safe to call from any task:
 * each field is a single word, written from exactly one task apiece, so
 * there is nothing here that needs a lock beyond that single-writer
 * guarantee. Wired into TOUCH_CMD_GET_STATE's reply by uart_bridge.c. */
void lvgl_port_get_touch_diag(bool *input_enabled, uint32_t *touch_read_cb_count,
                               uint32_t *injected_delivered_count);

/* ONE-OFF root-cause probe -- see lvgl_port.c's definition comment. Not part
 * of the permanent counter set the task asked for; kept only long enough to
 * settle whether lv_indev_create() itself failed. */
bool lvgl_port_indev_exists(void);

/* ONE-OFF root-cause probe -- see lvgl_port.c's definition comment. */
void lvgl_port_get_timer_handler_calls(uint32_t *calls);

#ifdef __cplusplus
}
#endif

#endif // LVGL_PORT_H
