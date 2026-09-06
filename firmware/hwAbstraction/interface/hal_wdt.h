/* hal_wdt.h -- watchdog control. Both ESP and pico, deliberately thin.
 * See docs/HW_ABSTRACTION_PLAN.md "hal_time / hal_wdt / hal_pwm /
 * hal_sysinfo" ("hal_wdt: esp task-WDT reconfigure (watchdog_cfg.c sole
 * user), esp_restart, and pico watchdog_enable/update/reboot. Thin.").
 *
 * Consumer census, both sides:
 *  - ESP task-WDT reconfigure: watchdog_cfg.c is the SOLE production caller
 *    of esp_task_wdt_* (re-verified: no other App/drivers/*.c includes
 *    esp_task_wdt.h). It persists a `panic_disabled` bool to NVS
 *    (independent of this header -- that stays hal_kv's job) and applies it
 *    by reconfiguring the task-WDT's panic behavior.
 *  - esp_restart(): real production call sites are factory_reset.c:90 (after
 *    a deliberate settle delay so the HTTP "ok" response ships first) and
 *    ota_http_recovery.c:98 (ota_recovery_exit_reboot_task, same delayed-
 *    reboot-task pattern). Both are unconditional, no-args, never-returns
 *    calls -- nothing else about esp_restart() varies at any real site.
 *  - Pico watchdog_enable(timeout_ms, pause_on_debug) -- main.c:230, the one
 *    arm-at-boot call.
 *  - Pico watchdog_update() -- watchdog_task.c:161, the recurring feed
 *    (this firmware's whole watchdog_task exists to call this on a
 *    schedule; the scheduling policy itself stays above this header,
 *    unchanged).
 *  - Pico watchdog_reboot() -- update_task.c:998, SaftyFW's sole call site,
 *    always as `watchdog_reboot(0, 0, 0)` (pc/sp/delay_ms all zero: a plain
 *    "reboot now", not the boot-vector handoff form). See hal_scratch.h's
 *    own header comment for the full stomp-hazard reasoning; this header's
 *    hal_wdt_reboot() below takes NO pc/sp/delay_ms parameters specifically
 *    so that hazardous form cannot be reached through the HAL at all.
 *
 * Disagreement with the plan: none. The plan's one-line sketch ("esp
 * task-WDT reconfigure ... esp_restart, and pico watchdog_enable/update/
 * reboot. Thin.") names exactly these five operations and no others; this
 * header covers all five and adds nothing beyond them.
 *
 * Threading/ownership contract:
 *  - hal_wdt_init/hal_wdt_feed/hal_wdt_reboot are boot-sequence /
 *    single-task operations on both real backends (armed once at boot,
 *    fed by one dedicated task, rebooted from one recovery path) -- no
 *    locking is added here because none of the real call sites need it.
 *  - hal_wdt_reboot() never returns on a real backend (pico watchdog_
 *    reboot() resets the RP2040 immediately; ESP hal_restart() equivalent
 *    of esp_restart() also never returns). Callers must treat any statement
 *    after this call as dead code on real hardware, matching update_task.c's
 *    own "defensive only" comment at its call site.
 *  - hal_wdt_set_panic_disabled()'s persisted-vs-live split (a live RAM
 *    value kept in sync with a separately-owned NVS record) stays entirely
 *    above this header, inside watchdog_cfg.c -- this call is the "apply to
 *    the live task-WDT" half only, not a persistence API.
 */
#ifndef KILNCTL_HAL_WDT_H
#define KILNCTL_HAL_WDT_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arms the watchdog for this backend.
 *   ESP:  esp_task_wdt_init()-shaped -- timeout_ms is the task-WDT period;
 *         `panic_disabled` selects whether an unfed WDT panics (false,
 *         the safe default) or merely warns (true), matching
 *         watchdog_cfg.c's persisted setting.
 *   pico: watchdog_enable(timeout_ms, pause_on_debug)-shaped;
 *         `panic_disabled` is unused (pico has no equivalent knob) and
 *         must be passed false by ESP-agnostic callers.
 * See hal_scratch.h for the scratch[4] side effect this causes on pico
 * (pico-sdk's watchdog_enable() writes that register itself; hal_scratch
 * refuses any caller-side write to it precisely because of this). */
hal_status_t hal_wdt_init(uint32_t timeout_ms, bool panic_disabled);

/* Re-applies the panic-disabled setting to an already-armed watchdog.
 * ESP-only operation in practice (watchdog_cfg.c's sole use); pico backend
 * may implement this as a no-op returning HAL_NOT_SUPPORTED since pico-sdk
 * exposes no equivalent post-init knob. */
hal_status_t hal_wdt_set_panic_disabled(bool panic_disabled);

/* Feeds/services the watchdog so it does not fire.
 *   ESP:  esp_task_wdt_reset()-shaped, called from the calling task's own
 *         registered check-in.
 *   pico: watchdog_update()-shaped, called from watchdog_task's own
 *         schedule (unchanged; this header does not alter that policy). */
hal_status_t hal_wdt_feed(void);

/* Unconditional, immediate reboot. NEVER RETURNS on a real backend --
 * see this header's threading contract above. Deliberately takes no
 * pc/sp/delay_ms parameters: pico-sdk's watchdog_reboot(pc, sp, delay)
 * stomps watchdog scratch[4..7] whenever pc != 0 (see hal_scratch.h's own
 * header comment for the full hazard), and the only real call site
 * (update_task.c:998) always passes zeros for all three -- so this
 * wrapper implements exactly that zeroed form on pico
 * (`watchdog_reboot(0, 0, 0)`) and esp_restart() on ESP, and offers no way
 * for a caller to request the non-zero boot-vector form through the HAL. */
void hal_wdt_reboot(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_WDT_H */
