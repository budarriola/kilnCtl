// recovery_health.c -- see recovery_health.h.
#include "recovery_health.h"

#include <stdbool.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

#include "recovery_health_policy.h"

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
