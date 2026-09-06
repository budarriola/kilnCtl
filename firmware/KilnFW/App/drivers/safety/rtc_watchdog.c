#include "rtc_watchdog.h"

#include "esp_log.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h" /* rtc_clk_slow_freq_get_hz() -- RWDT ticks off the RTC slow clock,
                      * not a fixed frequency, so the ms->ticks conversion must read it
                      * live rather than assume a constant (see esp-idf's own
                      * cpu_start.c/panic.c, which do the same conversion the same way
                      * for this exact HAL). */

static const char *TAG = "rtc_watchdog";

static wdt_hal_context_t s_rwdt = RWDT_HAL_CONTEXT_DEFAULT();

void rtc_watchdog_start(void)
{
    uint32_t stage_timeout_ticks =
        (uint32_t)((uint64_t)RTC_WATCHDOG_TIMEOUT_MS * rtc_clk_slow_freq_get_hz() / 1000ULL);

    wdt_hal_write_protect_disable(&s_rwdt);
    wdt_hal_config_stage(&s_rwdt, WDT_STAGE0, stage_timeout_ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    /* Only one stage armed: this watchdog's whole job is "reset the chip if
     * nobody feeds it for RTC_WATCHDOG_TIMEOUT_MS", not a staged
     * interrupt-then-reset escalation like int_wdt.c's -- there is no
     * software left to usefully warn if the scheduler itself is the thing
     * that stopped. */
    wdt_hal_enable(&s_rwdt);
    wdt_hal_write_protect_enable(&s_rwdt);

    ESP_LOGI(TAG, "RTC watchdog armed: %d ms, RESET_SYSTEM on expiry (independent of the "
                  "scheduler/interrupt path -- see rtc_watchdog.h)",
             RTC_WATCHDOG_TIMEOUT_MS);
}

void rtc_watchdog_feed(void)
{
    wdt_hal_write_protect_disable(&s_rwdt);
    wdt_hal_feed(&s_rwdt);
    wdt_hal_write_protect_enable(&s_rwdt);
}
