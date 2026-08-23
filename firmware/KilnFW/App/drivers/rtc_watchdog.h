// rtc_watchdog -- last-resort hardware reset, independent of the scheduler.
//
// WHY THIS EXISTS ON TOP OF CONFIG_ESP_TASK_WDT_PANIC / CONFIG_ESP_INT_WDT
// -----------------------------------------------------------------------
// The task watchdog (5 s, now CONFIG_ESP_TASK_WDT_PANIC) and the interrupt
// watchdog (300 ms, CONFIG_ESP_INT_WDT) both rely on the panic handler
// actually running -- which itself needs interrupts to still be dispatched.
// A lockup that disables interrupts entirely (a tight spin in a critical
// section, a hardware fault that wedges the interrupt controller itself) can
// starve both of them: no task ever gets to feed the task WDT, but also no
// interrupt ever fires to let the int WDT's own ISR detect it.
//
// The chip's RTC watchdog (RWDT) is the one timer on this SoC that is NOT
// gated on the CPU dispatching interrupts at all -- it runs off the RTC slow
// clock and, once configured to RESET_SYSTEM on expiry, its hardware timeout
// path does not go through any interrupt handler the locked-up CPU could be
// blocking. It is fed periodically by ordinary application code (see
// rtc_watchdog_feed()); if that feed ever stops arriving -- because the
// scheduler itself is wedged, not just one task -- the RWDT resets the chip
// on its own schedule regardless of what the CPU is or isn't doing.
//
// wdt_hal.h (chip-generic HAL, works on ESP32-S3 unlike the older ESP32/S2-
// only rtc_wdt.h driver) is used directly rather than a higher-level
// wrapper: ESP-IDF does not expose an app-level "arm the RTC watchdog for me"
// call outside of the bootloader's own (bootloader-phase-only)
// CONFIG_BOOTLOADER_WDT_ENABLE.
#ifndef RTC_WATCHDOG_H
#define RTC_WATCHDOG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Comfortably longer than any legitimate blocking operation observed or
 * documented anywhere in this codebase (the longest logged/expected blocks
 * are the flash-safe executor's NVS/OTA writes, on the order of hundreds of
 * ms to low seconds -- see uart_bridge_ext.c/ota_http.c), and longer than
 * CONFIG_ESP_TASK_WDT_TIMEOUT_S (5 s) so an ordinary task-WDT panic+reset
 * always wins the race first when the scheduler itself is still alive. This
 * is the pure last-resort backstop for the case where even that panic path
 * cannot run. */
#define RTC_WATCHDOG_TIMEOUT_MS 20000

/* Arms the RTC watchdog with a single RESET_SYSTEM stage at
 * RTC_WATCHDOG_TIMEOUT_MS and feeds it once so the timer starts fresh.
 * Call exactly once, early in app_main() (after boot_guard_init() has
 * counted this boot, so a reset caused by this watchdog is itself counted
 * toward recovery mode like any other reset). Safe to call more than once
 * (re-arms and re-feeds; harmless). */
void rtc_watchdog_start(void);

/* Resets the RWDT's internal counter back to stage 0 / elapsed 0. Call this
 * periodically from a task that only runs when the scheduler is genuinely
 * still servicing normal-priority work -- monitor_task.c's existing
 * heartbeat cadence is used for this (see monitor_task.c), rather than a
 * dedicated task, so a hang that stops that heartbeat is exactly the
 * lockup this watchdog exists to catch. */
void rtc_watchdog_feed(void);

#ifdef __cplusplus
}
#endif

#endif // RTC_WATCHDOG_H
