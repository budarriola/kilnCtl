// Host-test stub -- see esp_err.h's own header comment for why these exist.
// Added 2026-08-22 for crash_report.c's host tests. Only the reset-reason
// enum/function crash_report_init() reads; esp_reset_reason() always
// reports POWERON since host tests never exercise crash_report_init()
// itself (see esp_core_dump.h stub's header comment).
#ifndef TEST_STUB_ESP_SYSTEM_H
#define TEST_STUB_ESP_SYSTEM_H

typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

/* Added 2026-08-27 for ota_http.c's/factory_reset.c's host tests
 * (test_ota_http.c) -- both call the real esp_restart() from a reboot task;
 * declared here (matching real esp_system.h), defined in whichever test file
 * first needs it to link, same "declared once, defined per-executable" split
 * stubs/esp_http_server.h already uses. Never actually invoked by any host
 * test (the reboot tasks that call it are only ever created via a
 * FreeRTOS task-creation stub that never runs its argument). */
void esp_restart(void);

#endif // TEST_STUB_ESP_SYSTEM_H
