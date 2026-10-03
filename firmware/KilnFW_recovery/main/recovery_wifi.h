// recovery_wifi.h -- Wi-Fi bring-up for the OTA recovery image.
//
// Owner decision 2026-10-02 (docs/RECOVERY_IMAGE_PLAN.md): the recovery image
// is unauthenticated. Its only access control is a WPA2 SoftAP whose random
// per-boot passphrase is shown on the LCD and nowhere else. There is no
// station mode (the routes are open, so only the LCD-guarded AP may reach
// them) and the only stored value read is the AP SSID, from the shared
// `wifi_nvs` partition (namespace "wifi_cfg", key "ap_ssid"; default
// "kilnctl-recovery"). Writes nothing to `wifi_nvs`.
#ifndef RECOVERY_WIFI_H
#define RECOVERY_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the WPA2 SoftAP with a fresh random passphrase and hands SSID,
// passphrase and IP to the LCD. Blocks until the AP is up or bring-up failed.
// Always returns and never aborts: any init failure is logged and shown as
// "NO NETWORK" on the LCD so HTTP startup does not depend on Wi-Fi.
void recovery_wifi_start(void);

// True once the SoftAP is active and accepting associations.
bool recovery_wifi_is_up(void);

// NULL while Wi-Fi bring-up has had no fatal error; otherwise a static string:
// "wifi_storage_fail" = esp_wifi_set_storage(RAM) failed, so the AP was NOT
// started (the passphrase would not be RAM-only). Shown on the LCD as
// "WIFI STORAGE FAIL" and in /api/recovery/status as "wifi_error".
const char *recovery_wifi_error(void);

// Counters from the Wi-Fi event handler, for /api/recovery/status: tells a real
// SoftAP stop apart from a PC-side scan artifact. `last_event_name` is a static
// string; last_event_age_s is meaningful only when last_event_seen.
typedef struct {
    uint32_t ap_start_count;
    uint32_t ap_stop_count;
    uint32_t ap_sta_connect_total;
    uint32_t ap_sta_disconnect_total;
    uint32_t ap_sta_now;
    const char *last_event_name;
    uint32_t last_event_age_s;
    bool last_event_seen;
} recovery_wifi_stats_t;

void recovery_wifi_get_stats(recovery_wifi_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_WIFI_H
