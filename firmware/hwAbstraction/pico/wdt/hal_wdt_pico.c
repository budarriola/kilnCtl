/* hal_wdt_pico.c -- pico-sdk backend for interface/hal_wdt.h (pico half only).
 *
 * Phase 1b ("adapt"): implements the pico-sdk watchdog_enable/watchdog_update/
 * watchdog_reboot half of the Phase-0 interface, grounded in the real
 * SaftyFW consumers hal_wdt.h's own header comment names: main.c:230
 * (watchdog_enable at boot), watchdog_task.c:161 (watchdog_update, the
 * recurring feed), update_task.c:998 (watchdog_reboot(0, 0, 0), SaftyFW's
 * sole call site). The ESP half of this header (esp_task_wdt_*, esp_restart)
 * is out of scope here -- see hal_wdt_esp.c.
 *
 * INTERFACE MISMATCH: none. hal_wdt_init()'s panic_disabled has no pico-sdk
 * equivalent knob (per this header's own doc comment: "must be passed false
 * by ESP-agnostic callers") -- this backend ignores the argument entirely
 * rather than fabricating behavior pico-sdk does not have, matching
 * hal_wdt_set_panic_disabled()'s documented HAL_NOT_SUPPORTED no-op for this
 * backend. hal_wdt_reboot() takes no pc/sp/delay_ms parameters, so the only
 * form reachable through this backend is watchdog_reboot(0, 0, 0) -- the
 * exact call SaftyFW's one real site already makes. Because pc == 0 always,
 * pico-sdk's watchdog_reboot() never writes scratch[4..7] through this HAL
 * (hardware_watchdog/watchdog.c:90-112 only touches those registers when
 * pc != 0); see hal_scratch.h's own header comment for the full hazard this
 * sidesteps structurally, not just incidentally.
 */
#include "hal_wdt.h"

#include "hardware/watchdog.h"

hal_status_t hal_wdt_init(uint32_t timeout_ms, bool panic_disabled) {
    /* pico-sdk has no equivalent knob -- see this file's own header comment
     * and hal_wdt.h's doc comment on this parameter for pico. */
    (void)panic_disabled;
    watchdog_enable(timeout_ms, false);
    return HAL_OK;
}

hal_status_t hal_wdt_set_panic_disabled(bool panic_disabled) {
    (void)panic_disabled;
    /* pico-sdk exposes no post-init knob -- see hal_wdt.h's own doc comment
     * on this function for the pico backend. */
    return HAL_NOT_SUPPORTED;
}

hal_status_t hal_wdt_feed(void) {
    watchdog_update();
    return HAL_OK;
}

void hal_wdt_reboot(void) {
    /* Deliberately the zeroed form only -- see this file's own header
     * comment on the scratch[4..7] stomp hazard this sidesteps. Never
     * returns on real hardware. */
    watchdog_reboot(0, 0, 0);
    for (;;) {
        /* unreachable on real hardware; keeps this a well-formed noreturn-
         * shaped function for the syntax-only compile check. */
    }
}
