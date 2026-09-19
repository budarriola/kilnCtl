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
#include "lvgl.h" /* lv_indev_t, for lvgl_port_get_indev() */

#include "panel_spi.h"
#include "screen_idle.h"
#include "touch_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `display` must already be up (ILI9488_start succeeded) -- this is the only
 * hard requirement. `touch_dev` may be NULL (no touch hardware; the UI is
 * then view-only until something injects touches some other way) -- when
 * non-NULL it must already be built and its underlying controller started
 * (NS2009_start()/FT6336U_start()) by the caller; this module only reads
 * through it, never constructs it, so the same lvgl_port_start() works for
 * either controller (touch_dev.h's whole point -- see its header comment).
 * `idle` may be NULL (no auto-blank integration -- LVGL just draws
 * continuously); when non-NULL it must already be screen_idle_init'd with
 * touch = NULL, since this module becomes the sole touch-controller reader
 * and feeds it through screen_idle_inject_touch() instead.
 *
 * Brings up lv_init(), the display driver (partial-redraw, PSRAM buffers),
 * the pointer input device (if `touch_dev`), a 1ms lv_tick source, builds a
 * placeholder boot screen, and starts the task that drives
 * lv_timer_handler(). Everything after this call happens on that one task --
 * LVGL is not thread-safe and nothing else may call an lv_* function. */
esp_err_t lvgl_port_start(ILI9488Class *display, const touch_dev_t *touch_dev, screen_idle_t *idle);

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

/* Whether a user-run touch calibration is a meaningful thing to OFFER on the
 * controller actually wired in right now -- touch_dev.h's
 * touch_cal_support_t, applied to the live touch_dev_t this module copied at
 * lvgl_port_start().
 *
 * This is the one predicate every calibration surface consults: the LCD
 * config hub's nav cell (ui_page_config.c), kiln_ui.c's forced first-boot
 * gate, ui_page_touch_cal.c's own build(), and the web UI via /api/status's
 * touch_cal_supported field (dashboard_status_http.c). Do NOT add a fourth
 * surface that re-derives this from the touch_dev_t's `self_calibrating`
 * flag: that flag is one INPUT to the decision, not the decision, and is
 * `false` for a board with no touch controller at all (a zeroed
 * touch_dev_t) exactly as it is for a present resistive one. A public
 * lvgl_port_touch_is_self_calibrating() getter used to exist and was removed
 * for precisely that reason -- three surfaces each asked it separately and
 * all three got the failed-bring-up case wrong.
 *
 * Distinct from lvgl_port_touch_is_calibrated(): that says whether a fit HAS
 * been done, this says whether doing one is possible at all. A SUPPORTED
 * board that has never been calibrated is the normal first-boot state; the
 * two are independent facts and both are reported.
 *
 * Thread safety: identical to lvgl_port_touch_is_calibrated() above --
 * s_port.touch_dev is written once, by lvgl_port_start(), before any other
 * task can observe it, and never mutated afterward, so reading it from the
 * HTTP task is safe (strictly more stable than s_touch_cal, which at least
 * gets rewritten on a recalibration). */
touch_cal_support_t lvgl_port_touch_cal_support(void);

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

/* Requests the next lvgl_port_task loop iteration run
 * kiln_ui_log_tap_targets() on ITS OWN stack (8192 B, static, internal SRAM)
 * instead of the caller's. Callable from any task -- in particular from
 * touch_uart_bridge (uart_bridge_touch.c's TOUCH_CMD_LOG_TAP_TARGETS
 * handler), whose own stack is only 3072 B and does not have room for a
 * recursive LVGL tree walk plus the ESP_LOGI formatting it does at every
 * node (see lvgl_port.c's definition comment for the bench-reproduced
 * panic this replaced). Fire-and-forget, same contract as the command
 * handler it serves: the dump goes out as ESP_LOGI lines over
 * uart_log_bridge on whatever the next lvgl_port_task tick is (well under
 * its usual 50 ms poll), not as an immediate synchronous call. A second
 * request arriving before the first is serviced simply keeps the flag set
 * -- one dump still runs, no queue to overflow. This also fixes a
 * pre-existing thread-safety gap: kiln_ui_log_tap_targets() walks live LVGL
 * objects, and lvgl_port_task is the only task LVGL itself may be called
 * from (see this file's header comment); routing the walk through it makes
 * that true here too, not just for every other lv_* caller. */
void lvgl_port_request_tap_dump(void);

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

/* Exposes the touch indev for ui_lcd_lock.c's inactivity-lock activity hook
 * -- see lvgl_port.c's definition comment. NULL before lvgl_port_start(). */
lv_indev_t *lvgl_port_get_indev(void);

/* ONE-OFF root-cause probe -- see lvgl_port.c's definition comment. */
void lvgl_port_get_timer_handler_calls(uint32_t *calls);

/* DISPLAY_ST7796_PLAN.md 9.1's measurement, made reportable -- see
 * lvgl_port.c's definition comment. last_us/max_us are microseconds spent in
 * the flush's SPI work (ILI9488_blit_begin/data/end), NOT lvgl_port_task's
 * overall CPU use; max_us is a running high-water mark since boot. Any
 * out-param may be NULL if the caller doesn't want that one. */
void lvgl_port_get_flush_stats(uint32_t *last_us, uint32_t *max_us, uint32_t *count);

/* Extended form adding min_us/mean_us (HW_ABSTRACTION.md "Still open") --
 * see lvgl_port.c's definition comment. count == 0 means "never flushed
 * yet"; min_us/mean_us only meaningful once count > 0. Any out-param may be
 * NULL. */
void lvgl_port_get_flush_stats_ex(uint32_t *last_us, uint32_t *min_us, uint32_t *max_us,
                                  uint32_t *count, uint32_t *mean_us);

#ifdef __cplusplus
}
#endif

#endif // LVGL_PORT_H
