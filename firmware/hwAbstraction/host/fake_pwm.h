/* fake_pwm.h -- host fake backend for hal_pwm.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION.md "hal_time / hal_wdt / hal_pwm /
 * hal_sysinfo" and the real backend this fake mirrors (esp/pwm/
 * hal_pwm_esp.c): one timer/channel, init validates duty_resolution_bits
 * (1..20, matching LEDC_TIMER_BIT_MAX on this port's low-speed timers --
 * the exact ceiling hal_pwm_esp.c checks) and start_duty_percent (0..100),
 * and computes a duty_max ceiling from duty_resolution_bits; hal_pwm_init()
 * applies start_duty_percent as the first recorded duty (matching
 * backlight_pwm_init()'s real channel config, which sets the panel's duty
 * at init time rather than starting at 0 and relying on a later
 * hal_pwm_set_duty() -- must not start dark if the flying wire IS fitted);
 * hal_pwm_set_duty() takes a 0..100 percentage, rejects anything over 100,
 * and folds ledc_set_duty()+ledc_update_duty() into one immediately-applied
 * call -- backlight_pwm.c never calls one without the other, so this fake
 * has no separate "staged, not yet applied" duty state to model.
 *
 * Recorded state: the last init cfg (for a test to assert against, e.g.
 * "backlight was configured for the pin main.c expects") and a bounded
 * history of every accepted hal_pwm_set_duty() call, in call order, so a
 * test can assert a specific re-sync sequence (e.g. "went to 0 before the
 * board reported low-battery, then ramped back up") rather than only the
 * latest value.
 */
#ifndef KILNCTL_FAKE_PWM_H
#define KILNCTL_FAKE_PWM_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_PWM_MAX_DUTY_HISTORY 64u

/* Resets everything: un-initializes the fake, clears the recorded init
 * cfg, and clears the duty history. Call between test cases. */
void fake_pwm_reset_all(void);

/* True once hal_pwm_init() has succeeded (and not undone by
 * hal_pwm_deinit() or fake_pwm_reset_all()). */
bool fake_pwm_is_initialized(void);

/* The exact hal_pwm_cfg_t the last successful hal_pwm_init() call was
 * given. Returns false (out untouched) if hal_pwm_init() has never
 * succeeded. */
bool fake_pwm_get_init_cfg(hal_pwm_cfg_t *out);

/* Number of successful hal_pwm_set_duty() calls recorded since the last
 * reset, capped at FAKE_PWM_MAX_DUTY_HISTORY (oldest entries are NOT
 * evicted -- a test that overflows this should either reset between
 * assertions or treat the cap as a test-writing bug, matching this
 * directory's other bounded-history fakes). */
uint32_t fake_pwm_get_duty_history_count(void);

/* The duty_percent argument of the call at `index` (0-based, call order).
 * Returns 0xFF (an impossible duty-percent value, since valid input is
 * 0..100) for an out-of-range index rather than asserting. */
uint8_t fake_pwm_get_duty_history(uint32_t index);

/* The most recently applied duty percent. Returns 0xFF (see above) if
 * hal_pwm_set_duty() has never been called successfully. */
uint8_t fake_pwm_get_last_duty(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_PWM_H */
