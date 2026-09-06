/* fake_wdt.h -- host fake backend for hal_wdt.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "hal_time / hal_wdt / hal_pwm /
 * hal_sysinfo" and both real backends this fake mirrors
 * (pico/wdt/hal_wdt_pico.c, esp/wdt/hal_wdt_esp.c): hal_wdt_init() arms the
 * watchdog with a timeout and a panic-disabled flag, hal_wdt_feed()
 * services it on a schedule, hal_wdt_set_panic_disabled() re-applies just
 * that bit to an already-armed watchdog, and hal_wdt_reboot() is an
 * unconditional immediate reboot that never returns on real hardware.
 *
 * Time model: the host has no real clock backing the watchdog, so this
 * fake exposes an explicit virtual-time knob, fake_wdt_advance_ms(), mirror-
 * ing fake_time.c's own manually-advanced-clock stance (never a real sleep;
 * a test drives elapsed time by hand). Internally this fake tracks
 * milliseconds elapsed since the last hal_wdt_feed() (or since
 * hal_wdt_init(), if never fed); fake_wdt_advance_ms() adds to that
 * counter and, if it is armed (hal_wdt_init() called) and the accumulated
 * elapsed time exceeds the configured timeout_ms, LATCHES fake_wdt_fired()
 * true -- matching a real unfed watchdog's sticky reset/panic outcome
 * (nothing un-fires a fired watchdog except fake_wdt_reset_all() or a new
 * hal_wdt_init()/hal_wdt_feed() call, both of which clear the latch since
 * they represent a fresh boot or a check-in respectively).
 *
 * pause_on_debug: pico honours this (watchdog_enable's second arg); ESP's
 * TWDT has no equivalent and ignores it. This fake mirrors the pico shape
 * and records it verbatim via fake_wdt_get_pause_on_debug().
 *
 * Reboot: hal_wdt_reboot() is documented NEVER TO RETURN on a real backend
 * (see hal_wdt.h's threading contract). This fake instead LATCHES a
 * reboot-requested flag (fake_wdt_reboot_requested()) and returns normally,
 * so a host test can observe that the call happened without hanging the
 * test process the way an accurate never-returns implementation would.
 *
 * hal_wdt_set_panic_disabled(): both real backends require hal_wdt_init()
 * to have run first in practice (ESP reconfigures an armed task-WDT; pico
 * has no such knob at all and returns HAL_NOT_SUPPORTED unconditionally).
 * This fake picks the ESP shape (the one real production caller,
 * watchdog_cfg.c, is ESP-only) -- HAL_NOT_READY if hal_wdt_init() has not
 * been called yet, otherwise updates the recorded panic_disabled flag and
 * returns HAL_OK. A test exercising the pico NOT_SUPPORTED contract talks
 * to the real hal_wdt_pico.c backend, not this fake -- this fake models the
 * generic "watchdog exists" surface both backends share, not either one's
 * ESP/pico-specific quirks (mirroring hal_time.c's stance: one host model,
 * not two backend-specific fakes).
 */
#ifndef KILNCTL_FAKE_WDT_H
#define KILNCTL_FAKE_WDT_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_wdt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resets everything: un-arms the watchdog, clears feed count, elapsed time,
 * the fired latch, and the reboot-requested latch. Call between test
 * cases. */
void fake_wdt_reset_all(void);

/* True once hal_wdt_init() has been called (and not un-armed by
 * fake_wdt_reset_all()). */
bool fake_wdt_is_initialized(void);

/* Recorded init configuration -- the exact arguments the last successful
 * hal_wdt_init() call was given. Undefined (zeroed) before the first
 * hal_wdt_init(). */
uint32_t fake_wdt_get_timeout_ms(void);
bool     fake_wdt_get_panic_disabled(void);
bool     fake_wdt_get_pause_on_debug(void);

/* Number of successful hal_wdt_feed() calls since the last
 * fake_wdt_reset_all(). */
uint32_t fake_wdt_get_feed_count(void);

/* Advances virtual time since the last feed (or since init, if never fed)
 * by `ms`. If the watchdog is armed and the accumulated elapsed time now
 * exceeds the configured timeout_ms, latches fake_wdt_fired() true. A
 * no-op (does not fire) if the watchdog was never armed. */
void fake_wdt_advance_ms(uint32_t ms);

/* True once a lapsed feed has been detected by fake_wdt_advance_ms().
 * Sticky until fake_wdt_reset_all() or the next successful hal_wdt_init()/
 * hal_wdt_feed() call. */
bool fake_wdt_fired(void);

/* True once hal_wdt_reboot() has been called. Sticky until
 * fake_wdt_reset_all(). See this header's top comment on why this fake
 * latches instead of accurately never returning. */
bool fake_wdt_reboot_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_WDT_H */
