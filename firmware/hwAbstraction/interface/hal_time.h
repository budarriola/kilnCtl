/* hal_time.h -- portable monotonic time interface. Both ESP and pico.
 * See docs/HW_ABSTRACTION_PLAN.md "hal_time / hal_wdt / hal_pwm / hal_sysinfo".
 *
 * Consumer census: ESP side calls esp_timer_get_time() (microsecond
 * monotonic since boot) at 44 sites across 19 files, plus the ubiquitous
 * FreeRTOS pdMS_TO_TICKS/xTaskGetTickCount/vTaskDelay millisecond-tick idiom
 * (90/52/44 call sites respectively) used for both "how long has elapsed"
 * and "sleep this task". Pico side calls get_absolute_time/time_us_64
 * (microsecond monotonic since boot, 19+6 sites) and to_ms_since_boot
 * (18 sites) for the same two purposes, plus pico-sdk's sleep_ms for
 * blocking delay. This header covers all of it with three primitives
 * (monotonic microseconds, monotonic milliseconds, blocking millisecond
 * delay) rather than mirroring each platform's native units one-for-one --
 * hal_time_now_ms() is the tick-counter replacement, hal_time_now_us() is
 * the esp_timer_get_time()/time_us_64() replacement, and hal_time_delay_ms()
 * is the vTaskDelay(pdMS_TO_TICKS(x))/sleep_ms(x) replacement. No consumer
 * on either side was found needing sub-millisecond delay or a tick-count
 * type distinct from milliseconds, so no separate "ticks" type is exposed.
 *
 * Disagreement with the plan: none found. The plan's hal_time entry is a
 * one-line sketch ("monotonic us/ms. Host fake = controllable clock with
 * advance-by-N and injectable jumps") with no concrete function list; the
 * three primitives above are the minimum that covers every distinct
 * operation in the census (elapsed-time measurement in us, elapsed-time
 * measurement in ms, and blocking delay) without inventing anything the
 * plan didn't ask for.
 *
 * Threading/ownership contract:
 *  - hal_time_now_us/ms are stateless, callable from any task or ISR
 *    context on both backends (esp_timer_get_time and time_us_64 both are;
 *    FreeRTOS xTaskGetTickCount is ISR-safe). No handle, no init call --
 *    both clocks run from boot unconditionally on real hardware.
 *  - hal_time_delay_ms blocks the calling task; it must never be called
 *    from an ISR or from a context that itself must not block (e.g. inside
 *    a lock also taken by the flash worker -- see the documented
 *    flash-worker re-entrancy and executor/autotune lock-order hazards in
 *    project memory, which this call does not fix and must not be used to
 *    paper over).
 *  - The host fake (Phase 2, fake_time) replaces the free-running clock with
 *    an explicit, test-driven advance and must support injectable jumps --
 *    a fake that only ever advances by uniform, idealized steps reintroduces
 *    the documented idealized-input bug class and is not an acceptable
 *    implementation of this contract.
 */
#ifndef KILNCTL_HAL_TIME_H
#define KILNCTL_HAL_TIME_H

#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic time since boot. Never goes backward on real hardware; the host
 * fake may inject jumps (forward only) for test scenarios per its spec. */
uint64_t hal_time_now_us(void);
uint64_t hal_time_now_ms(void);

/* Blocking delay of at least ms milliseconds. See threading contract above
 * for where this must not be called. */
hal_status_t hal_time_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_TIME_H */
