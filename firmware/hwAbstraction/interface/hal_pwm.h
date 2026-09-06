/* hal_pwm.h -- PWM output. ESP LEDC only today. See
 * docs/HW_ABSTRACTION.md "hal_time / hal_wdt / hal_pwm / hal_sysinfo"
 * ("hal_pwm: backlight LEDC only. Thin, last.").
 *
 * Consumer census: backlight_pwm.c is the ONLY LEDC user in this tree
 * (its own header comment records grepping App/ for ledc_/LEDC_TIMER/
 * LEDC_CHANNEL before adding it -- no other hits). Real call sequence,
 * re-verified against backlight_pwm.c line by line:
 *   ledc_timer_config(&{speed_mode, duty_resolution, timer_num, freq_hz,
 *                       clk_cfg=LEDC_AUTO_CLK})   -- once, at init
 *   ledc_channel_config(&{gpio_num, speed_mode, channel, timer_sel, duty=0,
 *                          hpoint=0})              -- once, at init
 *   ledc_set_duty(speed_mode, channel, duty)        -- every re-sync (200 ms
 *                                                       poll, backlight_pwm.c)
 *   ledc_update_duty(speed_mode, channel)           -- immediately after
 *                                                       every set_duty
 * No other LEDC call (fade, pause/resume, bind-to-interrupt) is used
 * anywhere. This header's three calls (init, set_duty, deinit) cover 100%
 * of the observed surface; set_duty intentionally folds ledc_set_duty +
 * ledc_update_duty into one call since backlight_pwm.c never calls one
 * without the other (they are always adjacent, same duty value).
 *
 * Disagreement with the plan: none -- the plan's one-line sketch
 * ("backlight LEDC only. Thin, last.") names no concrete API; the shape
 * above is the minimum covering backlight_pwm.c's real, sole use.
 *
 * ESP mode/timer/channel selection stays a backend concern, not part of
 * this interface: backlight_pwm.c hardcodes LEDC_TIMER_0/LEDC_CHANNEL_0/
 * LEDC_LOW_SPEED_MODE today precisely because nothing else in the tree
 * uses LEDC, so there is only ever one channel to abstract. A future
 * second PWM consumer would need this header widened with an explicit
 * timer/channel selector -- not attempted here since no such consumer
 * exists (matching hal_adc.h's stance on not widening ahead of a real
 * second use).
 *
 * Threading/ownership contract:
 *  - hal_pwm_init() is called once, at backlight_pwm_init() time, from
 *    boot/init context.
 *  - hal_pwm_set_duty() is called only from backlight_pwm_task's own poll
 *    loop (200 ms cadence) -- single-writer, no concurrent caller exists
 *    today, so no locking is added here.
 *  - No pico backend exists (SaftyFW has no backlight or other PWM
 *    consumer); a host fake simply records the last duty set, matching
 *    every other Phase-2 fake's "observable record" shape.
 */
#ifndef KILNCTL_HAL_PWM_H
#define KILNCTL_HAL_PWM_H

#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      gpio_num;         /* backlight MOSFET gate pin */
    uint32_t freq_hz;          /* backlight_pwm.c: 5000 */
    uint8_t  duty_resolution_bits; /* backlight_pwm.c: 13 (LEDC_TIMER_13_BIT) */
    uint8_t  start_duty_percent; /* 0..100, applied at hal_pwm_init() time --
                                   * see below. */
} hal_pwm_cfg_t;

/* Configures the timer and channel (ledc_timer_config + ledc_channel_config,
 * then applies start_duty_percent -- matching backlight_pwm.c's init order:
 * timer first, then channel). HAL_INVALID_ARG if duty_resolution_bits would
 * make (1 << duty_resolution_bits) - 1 the wrong scale for a later
 * hal_pwm_set_duty() percent argument (backend validates against its own
 * hardware's max resolution, e.g. LEDC's per-speed-mode timer bit-width
 * ceiling), or if start_duty_percent > 100.
 *
 * start_duty_percent MUST NOT default to 0: production's
 * backlight_pwm_init() (App/drivers/hw/backlight_pwm.c) configures the
 * channel with `.duty = duty_for_percent(CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT)`
 * directly, not a separate post-init hal_pwm_set_duty() call -- the panel
 * must not start dark if the flying wire IS fitted. A caller passes the
 * same ON_PERCENT value here; the backend applies it as part of channel
 * config (ESP: chan_cfg.duty), matching that real call exactly rather than
 * starting at 0 and relying on a follow-up set_duty(). */
hal_status_t hal_pwm_init(const hal_pwm_cfg_t *cfg);

/* Sets duty as a 0..100 percentage and applies it immediately -- folds
 * ledc_set_duty()+ledc_update_duty() into one call since backlight_pwm.c
 * never calls one without the other. Percent-to-raw-duty scaling against
 * duty_resolution_bits is the backend's job (matching duty_for_percent()'s
 * existing pct * DUTY_MAX / 100 logic, moved here verbatim). */
hal_status_t hal_pwm_set_duty(uint8_t duty_percent);

/* Releases the timer/channel. No real caller exists today (backlight PWM
 * runs for the life of the board once armed), provided for symmetry with
 * every other hal_*_init()/_deinit() pair in this directory and for the
 * host fake's teardown-between-tests need. */
hal_status_t hal_pwm_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_PWM_H */
