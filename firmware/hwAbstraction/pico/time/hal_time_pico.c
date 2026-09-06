/* hal_time_pico.c -- pico-sdk backend for interface/hal_time.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against pico-sdk 2.x
 * (pico_time/include/pico/time.h). Not wired into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_pico_backends.ps1 for the syntax-only
 * compile check that stands in for that until Phase 1a's real move lands.
 *
 * Consumer census (docs/HW_ABSTRACTION_PLAN.md "hal_time / hal_wdt / hal_pwm
 * / hal_sysinfo"): SaftyFW calls time_us_64()/get_absolute_time() (19+6
 * sites) and to_ms_since_boot() (18 sites) for monotonic elapsed-time
 * measurement, plus pico-sdk's sleep_ms() for blocking delay -- the same two
 * purposes hal_time.h's three primitives cover. Grepped directly against
 * firmware/SaftyFW/src: time_us_64() in tasks/watchdog_task.c,
 * tasks/link_task.c, tasks/discrete_task.c, tasks/log_task.c and others for
 * "how long since the last X"; to_ms_since_boot(get_absolute_time()) in the
 * same files for millisecond-granularity comparisons against configured
 * timeouts (link_timeout_s, estop_debounce_ms, etc.); sleep_ms() in
 * tasks/console_uart.c and startup/bring-up code for a fixed blocking delay.
 *
 * Mapping is 1:1, no adaptation needed:
 *   hal_time_now_us()    -> time_us_64()
 *   hal_time_now_ms()    -> to_ms_since_boot(get_absolute_time())
 *   hal_time_delay_ms()  -> sleep_ms()
 *
 * No INTERFACE MISMATCH found. hal_time.h's three primitives (monotonic us,
 * monotonic ms, blocking ms delay) are exactly the three operations every
 * real SaftyFW time call site needs; nothing observed requires a tick-count
 * type distinct from milliseconds, ISR-context delay, or sub-millisecond
 * delay. time_us_64()/get_absolute_time() are documented pico-sdk-safe to
 * call from either core and from within an IRQ handler (they read a
 * hardware timer register, no lock, no RTOS call), matching hal_time.h's
 * "stateless, callable from any task or ISR context" contract; sleep_ms()
 * must not be called from an ISR (it yields to the scheduler), matching
 * hal_time_delay_ms()'s documented blocking-task-only contract.
 */
#include "hal_time.h"

#include "pico/time.h"

uint64_t hal_time_now_us(void) {
    return time_us_64();
}

uint64_t hal_time_now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

hal_status_t hal_time_delay_ms(uint32_t ms) {
    sleep_ms(ms);
    return HAL_OK;
}
