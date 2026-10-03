// recovery_wifi.h -- Wi-Fi bring-up for the OTA recovery image.
//
// SCOPE OF THIS PASS (docs/OTA_SINGLE_SLOT_PLAN.md section 8 step 1): reads
// the legacy single-network station credential the main KilnFW image
// already writes to the shared `wifi_nvs` partition (namespace "wifi_cfg",
// keys "ssid"/"pass" -- App/drivers/net/wifi_prov_nvs.c's NVS_KEY_SSID/
// NVS_KEY_PASS) and the AP fallback SSID/password (keys "ap_ssid"/
// "ap_pass"). Deliberately does NOT read the multi-network "saved_nets"
// blob that wifi_prov_nvs.c also maintains -- joining the one network an
// operator has actually provisioned is enough for recovery's one job, and
// decoding that blob's private struct layout from a second, independent
// project would be exactly the kind of two-copies-of-one-format hazard
// CLAUDE.md's "reset one side of a pair" class warns about. If the legacy
// keys are ever removed from the writer side, this reader goes stale
// silently -- flagged here as a follow-up, not fixed in this pass.
//
// Writes nothing to `wifi_nvs`, per docs/OTA_SINGLE_SLOT_PLAN.md section 3.
#ifndef RECOVERY_WIFI_H
#define RECOVERY_WIFI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts Wi-Fi: attempts station mode against the stored credential for a
// bounded time, and falls back to SoftAP (using the stored AP SSID/password,
// or a fixed default if neither was ever set) if the station connection does
// not come up. Blocks until one of the two is confirmed up or both failed.
// Always returns and never aborts: any init failure is logged and degrades
// (STA, then AP-only, then "NO NETWORK" on the LCD) so HTTP startup does not
// depend on Wi-Fi. After an established station link drops, reconnects for a
// bounded window (30 s) and then falls back to the SoftAP.
void recovery_wifi_start(void);

// True once either the station link is up (has an IP) or the SoftAP is
// active and accepting associations.
bool recovery_wifi_is_up(void);

// Reads the AP password out of `wifi_nvs` (namespace "wifi_cfg", key
// "ap_pass") into `out`, NUL-terminated, up to `out_len` bytes. Returns
// false (and leaves `out` untouched) if no AP password was ever stored --
// callers must treat that as "challenge auth cannot be satisfied" rather
// than falling back to an empty-string key, since an empty key would make
// every board's recovery image share the same derived HMAC key.
bool recovery_wifi_get_ap_password(char *out, size_t out_len);

// The secret the AP passphrase and the HTTP auth key are derived from: the
// stored ap_pass when it is 8..63 chars, otherwise the eFuse-MAC fallback
// secret (recovery_auth.h). *fallback (optional) reports which. Returns false
// only if even the fallback cannot be derived (eFuse MAC unreadable).
bool recovery_wifi_get_auth_secret(char *out, size_t out_len, bool *fallback);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_WIFI_H
