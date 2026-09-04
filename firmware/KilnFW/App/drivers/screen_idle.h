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

#include "panel_spi.h"
#include "NS2009.h"
#include "display_power_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ILI9488Class *display;
    NS2009Class *touch; /* NULL if the touch controller never came up */

    /* Guards last_activity_tick/screen_on/policy_state/touch_held(_swallow)/
     * error_prev_active against the poll task and the UART bridge's INJECT
     * handler (via lvgl_port.c's touch_read_cb, screen_idle_touch_swallow())
     * touching them from two different tasks. */
    SemaphoreHandle_t lock;
    TickType_t last_activity_tick;
    bool screen_on;
    bool ready;

    /* 2026-09-04 owner request (docs/UI_PLAN.md "Display power"):
     * display_power_policy_step() (display_power_policy.h) is now the sole
     * decision-maker for screen_on -- this module supplies it the real
     * inputs (profile_executor's firing state, the dashboard's cached
     * safety-trip signal, display_power_cfg.c's persisted settings) and
     * applies its output. policy_state is the caller-fed-back state the
     * header documents -- DISPLAY_POWER_ON is correct at init, matching
     * screen_on's own true-at-init default below. */
    display_power_state_t policy_state;

    /* Touch-swallow edge tracking (rule 3/4/5 -- first touch after OFF or
     * ERROR_HOLD wakes but does not act on the UI underneath). A press is
     * reported to screen_idle_touch_swallow() on EVERY poll it stays down
     * (matching touch_read_cb's existing per-poll delivery), but
     * display_power_policy_step() must see touch_event=true on exactly the
     * first poll of a NEW press (the header's own calling contract) -- so
     * touch_held distinguishes "this is the edge" from "this is a held
     * repeat", and touch_held_swallow is the edge's swallow decision, held
     * for the rest of that one press/release gesture so a drag reads
     * consistently (never re-decided mid-gesture, never dropped for the
     * NEXT separate press once this one releases). */
    bool touch_held;
    bool touch_held_swallow;

    /* error_active is a LEVEL (dashboard_get_status()'s cached diag_state);
     * display_power_policy_step() needs the ENTERED-this-tick EDGE (see
     * display_power_policy.h's error_entered_this_tick comment) -- this is
     * the last-seen level, compared against the new read every time either
     * the poll task or a touch edge asks the policy a question, so the
     * edge is computed exactly once per real transition regardless of
     * which caller happens to observe it first. */
    bool error_prev_active;

    /* boot_guard.h RECOVERY MODE, captured once at screen_idle_init() and
     * never changed for the life of the boot (matches boot_guard_is_
     * recovery_mode()'s own "stable for the life of the boot" contract).
     * When true, screen_idle_run_policy_locked() (screen_idle.c) does NOT
     * call profile_executor_get_status()/autotune_engine_is_active()/
     * dashboard_get_status() at all -- recovery mode is main_control_
     * bringup.c's own promise that profile_executor_start()/autotune_
     * engine_start() were skipped this boot, and this module is started
     * UNCONDITIONALLY, before that skip decision even runs (main_boot_
     * early.c), on every poll tick for the life of the boot. Those three
     * accessors are already individually guarded against being called
     * before their subsystem's _start() (profile_executor_status.c,
     * autotune_engine.c's s_at.lock==NULL checks) -- this is a second,
     * belt-and-suspenders gate at the CALLER, following the exact
     * boot_guard.h precedent (2026-08-22, "the fix was not a list of call
     * sites to gate but making those modules safe to call before their
     * _start()") one 2026-09-04 bench crash later: this module now blocks
     * indefinitely on the bench, every boot, in recovery mode, at exactly
     * this call chain (screen_idle_task -> screen_idle_run_policy_locked
     * -> the producer reads), and gating here removes screen_idle from
     * the set of consumers recovery mode has to keep individually safe.
     * firing_active/error_active read as false while this is true -- the
     * correct, honest answer: recovery mode truly runs nothing that could
     * make either true. */
    bool recovery_mode;

    /* Snapshot of the two EXPENSIVE producer reads (firing_active from
     * profile_executor_get_status()+autotune_engine_is_active(),
     * error_active from dashboard_get_status()+PROFILE_EXEC_FAULTED),
     * refreshed by screen_idle_task ONLY, with this module's lock NOT
     * held, at most once every SCREEN_IDLE_INPUT_POLL_MS (screen_idle.c).
     *
     * 2026-09-04 opus review of the recovery-mode/stack fix: the policy's
     * inputs used to be read inline inside screen_idle_run_policy_locked(),
     * i.e. with idle->lock HELD, on every 50 ms poll tick. That is 20 Hz of
     * (a) five real MAX31856 SPI burst reads on the same SPI host the panel
     * flushes over, (b) a kiln_io_owner_command_read() round trip that
     * blocks up to KILN_IO_OWNER_WAIT_MS (200 ms) on another task's queue,
     * and (c) heap_caps_get_free_size()/_largest_free_block() TLSF pool
     * walks, which run with interrupts disabled -- all while holding a lock
     * that lvgl_port.c's task (screen_idle_get_state(), every LVGL tick and
     * every touch) and backlight_pwm.c's task both take, with a 1000 ms
     * timeout. It also directly contradicted this module's own documented
     * invariant ("Only ever held for a few variable reads/writes, never
     * across I2C or SPI traffic", screen_idle.c). dashboard_get_status()'s
     * own comment sizes that cost against "a browser's poll cadence" and
     * ui_page_diagnostics.c's 2 s LCD tick -- not 20 Hz forever.
     *
     * Correctness of the snapshot: every consumer of these is a
     * multi-second display timeout or a "raise the screen" decision, so a
     * value at most SCREEN_IDLE_INPUT_POLL_MS old is indistinguishable from
     * a fresh one to a human looking at the panel. The touch-edge path
     * (screen_idle_touch_swallow(), called from the LVGL task) reads the
     * snapshot rather than doing those reads itself -- which is also what
     * keeps the LVGL task off SPI/heap-walk work it never asked for.
     *
     * In RECOVERY MODE these are never refreshed at all, so they keep their
     * memset(0) false/false -- the honest answer (recovery mode starts
     * neither profile_executor nor autotune_engine) and the whole reason
     * the recovery gate lives at the refresh site. */
    bool cached_firing_active;
    bool cached_error_active;
    /* Written and read ONLY by screen_idle_task (see screen_idle.c's
     * refresh site) -- no other task touches them, so they need no lock of
     * their own; the cached_* values above are published under idle->lock. */
    uint32_t last_input_ms;
    bool inputs_valid;
} screen_idle_t;

/* `display` must already be up (ILI9488_start succeeded); `touch` may be
 * NULL. `recovery_mode` is boot_guard_is_recovery_mode()'s answer for THIS
 * boot -- see the struct field's own comment for why screen_idle must know
 * it. Does not start the task -- see screen_idle_start. */
esp_err_t screen_idle_init(screen_idle_t *idle, ILI9488Class *display, NS2009Class *touch,
                           bool recovery_mode);

/* Starts screen_idle_task, which polls `touch` (if not NULL) roughly every
 * NS2009 conversion's worth of time, wakes the screen on any press edge or
 * injected touch, and blanks it after CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS
 * of no activity from either source. CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS ==
 * 0 (the default) disables the blank step entirely -- activity tracking and
 * wake-on-touch still run, the screen just never goes black. */
esp_err_t screen_idle_start(screen_idle_t *idle);

/* Feeds the idle timer and wakes the screen if it is currently blanked, same
 * as a real press would -- called from the UART bridge's TOUCH_CMD_INJECT
 * handler. `pressed` mirrors the wire command but only a press (true) resets
 * the timer or wakes the screen; a release (false) is accepted (so the host
 * can send a matching up-event) but changes nothing here. */
esp_err_t screen_idle_inject_touch(screen_idle_t *idle, uint16_t x, uint16_t y, bool pressed);

/* THE touch-swallow decision (rule 3/4/5) -- called from lvgl_port.c's
 * touch_read_cb() for BOTH the physical-NS2009 path and the LVGL-side
 * injected-touch path, on every poll a press reads down (not just the
 * press edge -- see this header's touch_held field comment for why the
 * caller does not need to pre-filter that itself). Does everything
 * screen_idle_inject_touch() does (marks activity, wakes a blanked
 * screen_on) PLUS runs display_power_policy_step() on the press edge and
 * returns whether THIS delivery must be swallowed -- caller's job is to
 * report the press to screen_idle either way (so the wake happens) but
 * skip delivering it to whatever LVGL widget is underneath when
 * *out_swallow comes back true (report LV_INDEV_STATE_RELEASED instead of
 * PRESSED for that poll -- see lvgl_port.c's touch_read_cb).
 *
 * `pressed` mirrors the real/injected press state, same as
 * screen_idle_inject_touch(); x/y are not used for the decision (matches
 * that function too) and exist only for symmetry/future use.
 * `*out_swallow` is left false on a lock timeout or when `pressed` is
 * false (a release is never itself swallowed -- see the touch_held field
 * comment: a swallowed release simply continues reporting RELEASED,
 * which is what the caller was already going to do). */
esp_err_t screen_idle_touch_swallow(screen_idle_t *idle, uint16_t x, uint16_t y, bool pressed,
                                    bool *out_swallow);

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
