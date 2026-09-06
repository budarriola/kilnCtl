// LEDC-PWM backlight driver -- DISPLAY_ST7796_PLAN.md Phase 7 / section 3.4.1
// ("No backlight pin exists on the board, and no expander pin is free").
//
// This board's MSP4031/ST7796 module leaves its backlight LED input (module
// pin 8) unconnected today -- the plan's recommended fix is an 11th flying
// wire from a spare ESP32 GPIO (15 or 16 preferred) straight to that pin,
// through the 3.3V push-pull / 10k-pulled-up MOSFET gate the module already
// provides. That wire is NOT fitted on the bench board as of this writing
// (owner constraint: "the flying wire is not fitted"). This driver is
// therefore written ANTICIPATING that bodge, same posture as 9.3/9.4/9.6/9.7
// in DISPLAY_ST7796_PLAN.md: gated behind a default-OFF Kconfig flag
// (CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE) so an unmodified board's boot is
// bit-for-bit unaffected by this file existing. With the flag off,
// backlight_pwm_start() is a no-op that returns ESP_ERR_NOT_SUPPORTED and
// touches no GPIO/LEDC peripheral at all.
//
// Wiring to screen_idle.c: screen_idle_t already tracks screen_on (real or
// injected touch wakes it; CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS blanks it --
// see screen_idle.h). That module could not drive a backlight itself because
// this board had no backlight line; with the bodge fitted, this driver reads
// screen_idle_get_state() on its own poll and maps screen_on ->
// CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT / CONFIG_KILNCTL_BACKLIGHT_IDLE_PERCENT
// duty. screen_idle.c/.h are NOT modified by this file -- this driver is a
// read-only observer of screen_idle_t through its existing public API, same
// relationship uart_bridge_ext.c's touch-inject path already has with it.
//
// Pure/testable vs. hardware split (same convention as boot_button.c):
// backlight_duty_percent_for_state() is pure integer logic with no ESP-IDF
// dependency and is host-tested directly. backlight_pwm_init()/_start()/
// _task() own the actual ledc_* calls and are exercised only by the trivial
// driver/ledc.h stub (App/test/stubs/driver/ledc.h) at host-test link time,
// the same "compiles and links, values not asserted on" role
// stubs/driver/gpio.h already plays for boot_button.c.
#ifndef BACKLIGHT_PWM_H
#define BACKLIGHT_PWM_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Deliberately NOT #include "screen_idle.h" or "display_power_cfg.h" here,
// and backlight_pwm.c does not include them either any more
// (HW_ABSTRACTION_PLAN.md "drivers/ layering" item 5: this is a hw-layer
// driver and must not reach up into screen_idle.c (ui) or
// display_power_cfg.c (persist) itself). Instead the caller -- which already
// knows both modules, e.g. main_boot_early.c -- supplies a
// `backlight_pwm_query_fn` at init time that reads screen_idle's state and
// display_power_cfg's brightness setting on this driver's behalf. Keeps
// backlight_duty_percent_for_state() (the pure logic under host test)
// reachable without dragging the whole display stack into the host build --
// same reason boot_button.c/.h keep profile_executor.h out of the pure
// boot_button_step()/state_refuses_bypass() path's compile unit.

#ifdef __cplusplus
extern "C" {
#endif

// --- Pure logic (host-testable, no ESP-IDF types) ---------------------------

// Maps the screen_idle "screen is on" flag to a 0-100 backlight duty
// percent, using the two Kconfig-configured brightness levels. Separated out
// so the actual on/idle percentages and the on->duty mapping can be unit
// tested without a target build. `on_percent`/`idle_percent` are passed in
// (rather than read from CONFIG_ macros inside this function) purely so
// tests can sweep values the running Kconfig might not have -- the real
// caller always passes the CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT /
// CONFIG_KILNCTL_BACKLIGHT_IDLE_PERCENT values.
uint8_t backlight_duty_percent_for_state(bool screen_on, uint8_t on_percent, uint8_t idle_percent);

// --- Hardware driver ---------------------------------------------------------

// Supplied by the caller at init time (see backlight_pwm_init()). Fills
// *out_screen_on/*out_idle_ms/*out_brightness_pct from whatever the caller's
// own screen-state and brightness-setting modules currently report --
// exactly what backlight_pwm_task() used to read directly via
// screen_idle_get_state()/display_power_cfg_brightness_percent() before this
// was inverted (HW_ABSTRACTION_PLAN.md "drivers/ layering" item 5). Returning
// anything other than ESP_OK causes the poll tick to skip (same as a
// screen_idle_get_state() lock-timeout used to), retried next poll.
typedef esp_err_t (*backlight_pwm_query_fn)(void *ctx, bool *out_screen_on, uint32_t *out_idle_ms,
                                            uint8_t *out_brightness_pct);

typedef struct {
    backlight_pwm_query_fn query_fn; /* NULL until backlight_pwm_init(); read-only after that */
    void *query_ctx;                 /* opaque, passed back to query_fn verbatim */
    bool ready;
    bool last_screen_on;    /* last state actually written to the LEDC channel */
    bool have_last_screen_on;
    uint8_t last_pct;       /* last duty actually written, so a brightness
                             * change with no screen-state change still
                             * reaches the panel (the operator moving the
                             * slider is exactly that case) */
    bool have_last_pct;
} backlight_pwm_t;

// `query_fn`/`query_ctx`: the caller's screen-state + brightness accessor
// (see backlight_pwm_query_fn's doc comment above) -- typically a small
// shim in main_boot_early.c that closes over a `const screen_idle_t *` and
// calls screen_idle_get_state()/display_power_cfg_brightness_percent().
// When CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE is on, both query_fn and
// query_ctx must be non-NULL -- this is enforced by the enabled build path.
// The disabled branch below (bodge not fitted) deliberately does NOT
// validate either argument, since it returns before touching them.
// Whatever query_ctx points to must already be initialized (e.g.
// screen_idle_init()'d; screen_idle_start() need not have run yet, but
// usually has) before backlight_pwm_start() runs. Configures the LEDC
// timer/channel on CONFIG_KILNCTL_BACKLIGHT_GPIO and immediately drives
// ON-percent duty (matches screen_idle's own "on" boot state, panel_spi.c
// leaving the panel lit after bring-up). Returns ESP_ERR_NOT_SUPPORTED
// without touching any peripheral if CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE is
// off -- callers should treat that as "no bodge fitted, nothing to do"
// rather than an error to surface loudly, same as NS2009_start's
// ESP_ERR_NOT_FOUND convention.
esp_err_t backlight_pwm_init(backlight_pwm_t *bl, backlight_pwm_query_fn query_fn, void *query_ctx);

// Starts the poll task that keeps LEDC duty in sync with screen_idle's
// screen_on flag. No-op / ESP_ERR_INVALID_STATE if backlight_pwm_init() did
// not leave `bl->ready` true (covers both "the flag is off" and "LEDC setup
// failed").
esp_err_t backlight_pwm_start(backlight_pwm_t *bl);

#ifdef __cplusplus
}
#endif

#endif // BACKLIGHT_PWM_H
