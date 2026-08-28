// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests (reached via the real settings.h, which
// wifi_prov.c #includes). Deliberately empty: every CONFIG_KILNCTL_* macro
// settings.h aliases is either behind an #if (undefined reads as 0, no
// error) or aliased into a macro that wifi_prov.c never actually expands
// (e.g. I2C/SPI/display pin settings) -- so nothing needs a real value here.
#ifndef TEST_STUB_SDKCONFIG_H
#define TEST_STUB_SDKCONFIG_H

/* These four ARE actually expanded by wifi_prov.c (WIFI_AP_SSID,
 * WIFI_AP_DEFAULT_PASSWORD, WIFI_AP_CHANNEL, WIFI_STA_CONNECT_TIMEOUT_MS --
 * settings.h's plain-name aliases for them), unlike the I2C/SPI/display pin
 * macros above, so unlike those they need real values. Values are arbitrary
 * test defaults, not meant to match any real Kconfig default. */
#define CONFIG_KILNCTL_WIFI_AP_SSID "kilnctl-test"
#define CONFIG_KILNCTL_WIFI_AP_DEFAULT_PASSWORD "test1234"
#define CONFIG_KILNCTL_WIFI_AP_CHANNEL 6
#define CONFIG_KILNCTL_WIFI_STA_CONNECT_TIMEOUT_MS 15000

/* Actually expanded by watchdog_cfg.c's build_twdt_config() -- values match
 * sdkconfig's real CONFIG_ESP_TASK_WDT_TIMEOUT_S=5 /
 * CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=y so the host test's expected
 * numbers double as a sanity check that this stub hasn't drifted from the
 * real build's Kconfig. */
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S 5
#define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 1
#define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 1

/* Added for safety_link.c's host build -- values arbitrary test defaults,
 * same convention as the WIFI_AP_* block above; nothing asserts against
 * them, only that safety_link.c's init/poll paths compile and link. */
#define CONFIG_KILNCTL_SAFETY_POLL_PERIOD_MS 100
#define CONFIG_KILNCTL_SAFETY_BAUD_RATE 230400
#define CONFIG_KILNCTL_SAFETY_FAULT_IO 4
#define CONFIG_KILNCTL_SAFETY_TX_IO 17
#define CONFIG_KILNCTL_SAFETY_RX_IO 18
#define CONFIG_KILNCTL_UART_OWNER_QUEUE_LEN 16
#define CONFIG_KILNCTL_UART_OWNER_TASK_PRIORITY 5
#define CONFIG_KILNCTL_UART_OWNER_STACK_SIZE 4096
#define CONFIG_KILNCTL_UART_PROTOCOL_TASK_PRIORITY 5
#define CONFIG_KILNCTL_UART_PROTOCOL_STACK_SIZE 4096

#endif // TEST_STUB_SDKCONFIG_H
