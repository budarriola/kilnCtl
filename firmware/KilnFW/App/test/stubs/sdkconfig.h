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

#endif // TEST_STUB_SDKCONFIG_H
