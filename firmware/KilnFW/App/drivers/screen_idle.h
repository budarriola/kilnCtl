// Screen idle/blank state machine: the reason this exists is panel life --
// the ILI9488 datasheet doesn't rate an LCD's on-hours, but running one lit
// continuously wastes it for no reason on a kiln that is watched only a
// fraction of a firing. There is no backlight control line on this board
// (docs/HARDWARE.md: "backlight apparently hardwired on with no control
// pin"), so a real "lights out" is not achievable in firmware -- "blank"
// here means painting the frame solid black (ILI9488_clear) rather than
// ILI9488_set_power(false): on this panel, display-off + sleep-in blanks to
// a bright WHITE page (bench finding, 2026-08-17), which is the opposite of
// what this feature is for. Black content blocks far more of the
// (unswitchable) backlight than white does, without needing a hardware
// change; it does not power the panel down.
//
// Two things feed "the screen was just touched", treated identically:
//   - a real press read off the NS2009 touch controller (NS2009.h), polled
//     by screen_idle_task;
//   - a synthetic touch injected over the UART bridge's TOUCH_CMD_INJECT
//     (uart_task_ids.h), via screen_idle_inject_touch() -- this is what lets
//     the PC-side MCP drive/test the UI without the physical glass, and it
//     resets the idle timer and wakes a blanked screen exactly like a real
//     touch would.
//
// Works with no touch hardware at all: NS2009_start failing at boot (an
// absent chip, or the SDA/SCL-swap question docs/HARDWARE.md still has open)
// leaves `touch` NULL in screen_idle_init, and screen_idle_task simply never
// sees a real press -- the idle timer still counts down and the screen still
// blanks, and injected touches still wake it. Auto-blank and MCP injection
// are not contingent on the touch chip answering.
#ifndef SCREEN_IDLE_H
#define SCREEN_IDLE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ILI9488.h"
#include "NS2009.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ILI9488Class *display;
    NS2009Class *touch; /* NULL if the touch controller never came up */

    /* Guards last_activity_tick/screen_on against the poll task and the UART
     * bridge's INJECT handler touching them from two different tasks. */
    SemaphoreHandle_t lock;
    TickType_t last_activity_tick;
    bool screen_on;
    bool ready;
} screen_idle_t;

/* `display` must already be up (ILI9488_start succeeded); `touch` may be
 * NULL. Does not start the task -- see screen_idle_start. */
esp_err_t screen_idle_init(screen_idle_t *idle, ILI9488Class *display, NS2009Class *touch);

/* Starts screen_idle_task, which polls `touch` (if not NULL) roughly every
 * NS2009 conversion's worth of time, wakes the screen on any press edge or
 * injected touch, and blanks it after CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS
 * of no activity from either source. */
esp_err_t screen_idle_start(screen_idle_t *idle);

/* Feeds the idle timer and wakes the screen if it is currently blanked, same
 * as a real press would -- called from the UART bridge's TOUCH_CMD_INJECT
 * handler. `pressed` mirrors the wire command but only a press (true) resets
 * the timer or wakes the screen; a release (false) is accepted (so the host
 * can send a matching up-event) but changes nothing here. */
esp_err_t screen_idle_inject_touch(screen_idle_t *idle, uint16_t x, uint16_t y, bool pressed);

/* Current state for TOUCH_CMD_GET_STATE: whether the screen is on right now
 * and how many milliseconds have elapsed since the last touch activity
 * (real or injected). idle_ms saturates at UINT32_MAX rather than wrapping
 * if the board has been idle long enough to overflow it (roughly 49 days),
 * which is already how the tick count itself is compared -- see
 * screen_idle.c. */
esp_err_t screen_idle_get_state(const screen_idle_t *idle, bool *out_screen_on,
                                uint32_t *out_idle_ms);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_IDLE_H
