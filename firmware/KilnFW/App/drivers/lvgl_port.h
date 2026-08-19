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

#include "esp_err.h"

#include "ILI9488.h"
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

/* Ignores every touch while disabled -- kiln_ui.c wraps a page switch in
 * this (disable, load + force a synchronous render/flush, re-enable) so a
 * tap landing during the switch can't be read against the outgoing screen's
 * stale layout and fire a click on whatever widget happens to occupy that
 * same pixel on the incoming page. Only lvgl_port_task calls lv_*, so this
 * is safe to call from an LV_EVENT_CLICKED handler (same task). */
void lvgl_port_set_input_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // LVGL_PORT_H
