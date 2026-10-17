// recovery_health.c -- see recovery_health.h.
#include "recovery_health.h"

#include <stdbool.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "recovery_apply_esp.h"

#include "recovery_health_policy.h"

#define RHEALTH_APPLY_WAIT_MS (10 * 60 * 1000)
static const char *TAG = "recovery_health";

#define RESTART_MAGIC 0x52454331u
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_http_restarts;
static RTC_NOINIT_ATTR uint32_t s_wifi_restarts;
static volatile bool s_keep; // a failure-driven restart is in progress: keep the counts

// Every esp_restart() runs this. A restart this module did not ask for is a
// deliberate one (exit, sw_reset, OTA reboot): the next recovery session starts
// with fresh counters.
static void on_shutdown(void)
{
    if (!s_keep) {
        s_http_restarts = 0;
        s_wifi_restarts = 0;
    }
}

void recovery_health_boot_init(void)
{
    bool poweron = esp_reset_reason() == ESP_RST_POWERON;
    bool magic_ok = s_magic == RESTART_MAGIC;
    s_http_restarts = rhealth_restart_count_at_boot(poweron, magic_ok, s_http_restarts);
    s_wifi_restarts = rhealth_restart_count_at_boot(poweron, magic_ok, s_wifi_restarts);
    s_magic = RESTART_MAGIC;
    if (esp_register_shutdown_handler(on_shutdown) != ESP_OK) {
        ESP_LOGE(TAG, "shutdown handler not registered: restart counts may carry over");
    }
}

uint32_t recovery_health_http_restarts(void)
{
    return s_http_restarts;
}

uint32_t recovery_health_wifi_restarts(void)
{
    return s_wifi_restarts;
}

void recovery_health_clear_http(void)
{
    s_http_restarts = 0;
}

void recovery_health_clear_wifi(void)
{
    s_wifi_restarts = 0;
}

void recovery_health_restart_for_http(void)
{
    // RO-01: never cut a running install apply (same bounded wait as recovery_wifi.c).
    const int64_t wait_start_us = esp_timer_get_time();
    while (recovery_apply_busy()) {
        if (esp_timer_get_time() - wait_start_us > (int64_t)RHEALTH_APPLY_WAIT_MS * 1000) {
            ESP_LOGW(TAG, "apply still busy after %d ms; restarting for HTTP anyway", RHEALTH_APPLY_WAIT_MS);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    s_http_restarts++;
    s_keep = true;
    esp_restart();
    for (;;) {
    }
}

void recovery_health_restart_for_wifi(void)
{
    s_wifi_restarts++;
    s_keep = true;
    esp_restart();
    for (;;) {
    }
}
