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

/* Added for gpio_probe.c's host test (test_gpio_probe.c), which #includes
 * gpio_probe.c directly with CONFIG_KILNCTL_ENABLE_GPIO_PROBE forced on
 * (see that file's own comment) to reach gpio_probe_is_denied() -- every one
 * of these is expanded by settings.h into the deny-list array gpio_probe.c
 * builds, so unlike the "never expanded, can stay undefined" macros this
 * file's top comment describes, these need real, DISTINCT integer values --
 * the test proves specific GPIO numbers are refused, so two deny-list pins
 * silently aliasing the same test value would hide a missing entry. Values
 * are arbitrary test doubles, not meant to match this board's real pinout
 * (that lives in the real, gitignored sdkconfig, per CLAUDE.md/
 * feedback_gitignored_config_hides_mismatch.md). */
#define CONFIG_KILNCTL_ENABLE_GPIO_PROBE 1
#define CONFIG_KILNCTL_SPI_SCLK_IO 10
#define CONFIG_KILNCTL_SPI_MOSI_IO 11
#define CONFIG_KILNCTL_SPI_MISO_IO 12
#define CONFIG_KILNCTL_THERMO_CS0_IO 13
#define CONFIG_KILNCTL_THERMO_CS1_IO 14
#define CONFIG_KILNCTL_THERMO_CS2_IO 15
#define CONFIG_KILNCTL_THERMO_FAULT0_IO 16
#define CONFIG_KILNCTL_THERMO_FAULT1_IO 21
#define CONFIG_KILNCTL_THERMO_FAULT2_IO 47
#define CONFIG_KILNCTL_I2C_SCL_IO 19
#define CONFIG_KILNCTL_I2C_SDA_IO 20
/* Needed once settings.h is included by a host test that pulls in MAX31856.c
 * (THERMO_SPI_CLOCK_HZ, KILN_SPI_HOST) -- first hit by
 * test_max31856_hal_spi.c's HAL Phase 1b host test. 4000000 matches
 * MAX31856.c's own MAX31856_SPI_MAX_CLOCK_HZ cap. CONFIG_KILNCTL_SPI_HOST_SPI3
 * left undefined (0/false), same as every #if CONFIG_KILNCTL_* boolean below
 * that this stub leaves unset -- KILN_SPI_HOST resolves to SPI2_HOST. */
#define CONFIG_KILNCTL_THERMO_SPI_CLOCK_HZ 4000000
/* Needed once settings.h is included by a host test that pulls in an I2C
 * device driver (FT6336U.c/NS2009.c/SX1509.c reference I2C_MASTER_FREQ_HZ
 * unconditionally, not just under an `#if`) -- first hit by
 * test_ft6336u.c's HAL-migration host test. */
#define CONFIG_I2C_MASTER_FREQUENCY 100000
#define CONFIG_KILNCTL_SX1509_IRQ_IO 33
#define CONFIG_KILNCTL_SX1509_RESET_IO 34
#define CONFIG_KILNCTL_DISPLAY_CS_IO 35
#define CONFIG_KILNCTL_UART_TX_IO 43
#define CONFIG_KILNCTL_UART_RX_IO 44

/* Forced on for test_safety_cfg_http.c's host coverage of bench_preset_job()/
 * bench_preset_post_handler() (docs/HTTP_POST_OWNER_MIGRATION_PLAN.md slice
 * A2), which safety_cfg_http.c compiles only under `#if CONFIG_KILNCTL_DEV_TOOLS`.
 * Same convention as CONFIG_KILNCTL_ENABLE_GPIO_PROBE above: the real bench
 * board's sdkconfig has this OFF (CLAUDE.md), so this is a host-test-only
 * override to reach code that would otherwise never be exercised, not a
 * statement about any real build's Kconfig. */
#define CONFIG_KILNCTL_DEV_TOOLS 1

#endif // TEST_STUB_SDKCONFIG_H
