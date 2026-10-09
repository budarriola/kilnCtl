/* hal_time_esp.c -- ESP-IDF backend for interface/hal_time.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2 (C:\esp\v6.0.2\esp-idf), grounded in the real KilnFW consumers
 * named in hal_time.h's own census: esp_timer_get_time() at 44 sites across
 * 19 files, plus the FreeRTOS pdMS_TO_TICKS/xTaskGetTickCount/vTaskDelay
 * millisecond-tick idiom used throughout drivers/ (e.g. SX1509.c:991,
 * autotune_engine.c:727, backlight_pwm.c:83, boot_button.c:141 --
 * vTaskDelay(pdMS_TO_TICKS(ms)), the one rounding form used at every grepped
 * call site). Not wired into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1 for the syntax-only
 * compile check standing in for that until Phase 1a's real move lands.
 *
 * No interface mismatch found: hal_time.h's three primitives
 * (hal_time_now_us, hal_time_now_ms, hal_time_delay_ms) cover every
 * distinct operation in the census with a 1:1 mapping onto
 * esp_timer_get_time()/pdMS_TO_TICKS()+vTaskDelay() -- no ESP consumer
 * needs a tick-count type distinct from milliseconds or sub-millisecond
 * delay (same conclusion the header's own comment already states).
 *
 * hal_time_now_ms() derivation: esp_timer_get_time() returns microseconds
 * since boot as an int64_t; dividing by 1000 gives milliseconds without a
 * second, independent hardware counter (xTaskGetTickCount() is NOT used
 * here even though it is the idiom's other half at 52 call sites --
 * xTaskGetTickCount() wraps at portMAX_DELAY-ish tick counts and is a
 * 32-bit count on this port, while esp_timer_get_time()'s 64-bit
 * microsecond counter is the one primitive already exposed as
 * hal_time_now_us(), so deriving ms from the SAME clock keeps
 * hal_time_now_us()/hal_time_now_ms() mutually consistent -- a caller
 * mixing both never sees two clocks that can disagree on elapsed time by
 * more than sub-millisecond truncation).
 *
 * hal_time_delay_ms() rounding: vTaskDelay(pdMS_TO_TICKS(ms)) verbatim --
 * pdMS_TO_TICKS() is the FreeRTOS-supplied ms-to-tick conversion already
 * used at every grepped vTaskDelay call site in drivers/; this backend does
 * not re-derive its own rounding.
 */
#include "hal_time.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint64_t hal_time_now_us(void) {
    return (uint64_t)esp_timer_get_time();
}

uint64_t hal_time_now_ms(void) {
    /* See the derivation note above: same clock as hal_time_now_us(), not
     * xTaskGetTickCount(), so the two primitives cannot disagree. */
    return hal_time_now_us() / 1000ULL;
}

hal_status_t hal_time_delay_ms(uint32_t ms) {
    /* Same rounding as every real call site: vTaskDelay(pdMS_TO_TICKS(ms)).
     * vTaskDelay() has no failure return on this port -- it always
     * completes -- so this always reports HAL_OK, matching the fact that
     * no existing caller checks a return value from the raw idiom either. */
    vTaskDelay(pdMS_TO_TICKS(ms));
    return HAL_OK;
}
