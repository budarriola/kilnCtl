// wifi_prov -- Wi-Fi bring-up, AP-fallback / station state machine, and NVS
// persistence of the saved network and the fallback AP's own identity.
//
// Storage (2026-08-12): the saved network, the mode, and the AP identity
// overrides live in their OWN flash partition, `wifi_nvs` (see partitions.csv
// and the WIFI_NVS_PARTITION comment in the .c file), not in the default `nvs`
// partition that holds zones, rules, profiles and the run-state breadcrumb.
// NVS recovery is partition-wide, so co-locating them meant a corrupt kiln
// config wiped the credentials needed to go fix it. The two partitions are now
// initialized and recovered strictly independently. Note that this survives a
// firmware flash and an erase of the default `nvs`, but nothing survives
// `esptool erase_flash` -- that clears the whole chip, this partition
// included.
//
// This is a THIRD client of the board, alongside the UART PC link and the
// safety processor link -- not a replacement for either. Per TODO.md
// section 1's two hard requirements:
//   - losing Wi-Fi must never stop the control loop, relay safety behavior,
//     or the UART/safety link. This module never touches kiln_io, the
//     relay-authority gate, or safety_link. Wi-Fi state is reported only
//     over the existing log link (uart_log_bridge) -- there is deliberately
//     no new SAFETY_FAULT_SRC_* bit for it (see the plan this shipped
//     under): the safety processor doesn't need to know about a link whose
//     entire purpose is "may or may not exist."
//   - a bad request or dropped client must never crash or restart the
//     firmware. See wifi_provision_http.c for the request-handling side of
//     that; this file's job is just not to feed it anything unbounded.
//
// Mode model (2026-08-11 redesign): a single explicit choice, not three
// overlapping flags. WIFI_PROV_MODE_HOME means "try to join the saved home
// network, fallback AP available meanwhile/on failure"; WIFI_PROV_MODE_AP
// means "AP only, forever, by user choice" -- the old "local_only" concept,
// renamed to match what it actually does now that the AP itself has its own
// editable identity (see wifi_prov_set_ap_ssid()/wifi_prov_set_ap_password())
// rather than being a fixed fallback nobody configures.
//
// Mode/state behavior:
//   mode == WIFI_PROV_MODE_AP    -> AP only, forever. Never attempts a
//                                    station join, never prompts for one.
//                                    state reports WIFI_PROV_STATE_AP_MODE.
//   mode == WIFI_PROV_MODE_HOME,
//     has saved credentials      -> AP+STA while attempting to join (so a
//                                    phone always has a way back in), STA-only
//                                    once the join is confirmed (got an IP).
//                                    A later disconnect brings the AP back
//                                    after WIFI_STA_CONNECT_TIMEOUT_MS of
//                                    failed reconnect attempts. state cycles
//                                    through CONNECTING/CONNECTED/
//                                    RECONNECTING.
//   mode == WIFI_PROV_MODE_HOME,
//     no credentials             -> AP only (first-boot provisioning state).
//                                    state reports WIFI_PROV_STATE_UNPROVISIONED.
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROV_SSID_MAX_LEN 32     /* 802.11 SSID limit */
#define WIFI_PROV_PASSWORD_MAX_LEN 64 /* WPA2-PSK limit */

typedef enum {
    WIFI_PROV_MODE_HOME = 0, /* attempt to join the saved home network; AP is
                              * a fallback, not a destination. */
    WIFI_PROV_MODE_AP,       /* AP only, permanent, by explicit user choice. */
} wifi_prov_mode_t;

typedef enum {
    WIFI_PROV_STATE_AP_MODE = 0,   /* AP only, mode == WIFI_PROV_MODE_AP */
    WIFI_PROV_STATE_UNPROVISIONED, /* home mode, AP only, no credentials saved yet */
    WIFI_PROV_STATE_CONNECTING,    /* home mode, AP+STA, first join attempt in flight */
    WIFI_PROV_STATE_CONNECTED,     /* home mode, STA only, has an IP */
    WIFI_PROV_STATE_RECONNECTING,  /* home mode, AP+STA, was connected, retrying */
} wifi_prov_state_t;

/* Brings up esp_netif/esp_wifi, initializes BOTH the `wifi_nvs` partition
 * (this module's own storage) and the default `nvs` partition (which the rest
 * of the firmware -- zones, rules, profiles, run_state, relay_cycles -- opens
 * without initializing itself, and which nothing else calls nvs_flash_init for;
 * app_main must therefore keep calling this before them), loads the saved
 * wifi_cfg, and starts AP/STA per the mode behavior above. Non-blocking: the station join (if any) happens on the Wi-Fi
 * driver's own event/task, not on the caller's stack, so this returns as
 * soon as the driver is configured and told to start -- it does not wait for
 * association or an IP.
 *
 * Also starts the provisioning HTTP server (wifi_provision_http.c). Safe to
 * call with no other board hardware present; failure is logged and returned,
 * never asserted -- same convention as every other bring-up step in
 * app_main. */
esp_err_t wifi_prov_start(void);

/* Current state, for the provisioning page's /status poll and for logging.
 * Not a device read -- this is the module's own tracked state, updated from
 * the Wi-Fi/IP event handlers. */
wifi_prov_state_t wifi_prov_get_state(void);

/* Current mode -- the one explicit toggle the provisioning page renders.
 * Distinct from wifi_prov_get_state(): mode is the persisted user choice,
 * state is where the state machine currently is while acting on it (e.g.
 * mode == HOME can be UNPROVISIONED, CONNECTING, CONNECTED, or
 * RECONNECTING depending on what's happened since). */
wifi_prov_mode_t wifi_prov_get_mode(void);

/* NULL-terminated. Empty string if nothing saved. Points at static storage;
 * valid for the process lifetime, no ownership transfer. */
const char *wifi_prov_get_saved_ssid(void);

/* NULL-terminated. The fallback AP's own SSID as currently in effect --
 * either the runtime override set via wifi_prov_set_ap_ssid(), or the
 * compile-time KILNCTL_WIFI_AP_SSID Kconfig default if no override has ever
 * been saved. Points at static storage; valid for the process lifetime, no
 * ownership transfer. */
const char *wifi_prov_get_ap_ssid(void);

/* Saves ssid/password to NVS and (if in home mode) starts a join attempt.
 * ssid_len/password_len are the number of bytes at ssid/password, NOT
 * counting a NUL -- the caller (the HTTP handler, parsing an untrusted
 * request body) is expected to have already bounded them; this function
 * re-validates against WIFI_PROV_SSID_MAX_LEN/WIFI_PROV_PASSWORD_MAX_LEN and
 * returns ESP_ERR_INVALID_SIZE rather than truncating silently.
 * password_len may be 0 (open network). Submitting credentials while in AP
 * mode is treated as an explicit choice to switch to home mode -- see the
 * .c file for why (mirrors the old "local_only" exit path, now generalized
 * to the mode concept). */
esp_err_t wifi_prov_set_credentials(const char *ssid, size_t ssid_len, const char *password,
                                    size_t password_len);

/* Sets and persists the Wi-Fi mode (the provisioning page's single toggle).
 * Switching to WIFI_PROV_MODE_AP: any in-progress or future station join is
 * abandoned/skipped and the AP is (re)started unconditionally. Switching to
 * WIFI_PROV_MODE_HOME: does not by itself require new credentials -- if a
 * network is already saved, a join attempt starts immediately (AP stays up
 * alongside it until the join is confirmed, per the "don't strand the
 * phone" guarantee); with nothing saved, this just leaves state at
 * WIFI_PROV_STATE_UNPROVISIONED (still AP-only) until credentials are
 * submitted via wifi_prov_set_credentials(). */
esp_err_t wifi_prov_set_mode(wifi_prov_mode_t mode);

/* Sets and persists the fallback AP's OWN SSID (distinct from
 * wifi_prov_set_credentials(), which submits credentials for joining a
 * *different*, existing network). ssid_len is 1-32 bytes, NOT counting a
 * NUL -- an empty AP SSID is refused with ESP_ERR_INVALID_SIZE, since the AP
 * always needs to be reachable by something. Takes effect immediately if
 * the AP radio is currently up -- already-associated clients are kicked and
 * must rejoin under the new SSID -- and persists across reboots. Overrides
 * KILNCTL_WIFI_AP_SSID from the moment it is first called. */
esp_err_t wifi_prov_set_ap_ssid(const char *ssid, size_t ssid_len);

/* Sets and persists the fallback AP's OWN password (the one needed to join
 * the board's own network itself) -- distinct from
 * wifi_prov_set_credentials(), which submits credentials for joining a
 * *different*, existing network. password_len may be 0 (open AP) or 8-63
 * (WPA2-PSK, per the part's requirements); anything else is rejected with
 * ESP_ERR_INVALID_SIZE. Takes effect immediately if the AP radio is
 * currently up -- already-associated clients are kicked and must rejoin
 * with the new password -- and persists across reboots. Overrides
 * KILNCTL_WIFI_AP_DEFAULT_PASSWORD from the moment it is first called. */
esp_err_t wifi_prov_set_ap_password(const char *password, size_t password_len);

/* True once the station interface has an IP (== WIFI_PROV_STATE_CONNECTED). */
bool wifi_prov_is_sta_connected(void);

/* Writes the station IP as dotted-quad text into out (out_cap must be at
 * least 16). Writes an empty string and returns ESP_ERR_INVALID_STATE if not
 * currently connected -- there is nothing to report, not a failure of this
 * call. */
esp_err_t wifi_prov_get_sta_ip(char *out, size_t out_cap);

typedef struct {
    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    int8_t rssi;
    bool secure; /* false = open network */
} wifi_prov_scan_result_t;

/* Blocking Wi-Fi scan (runs on the caller's task/thread -- call this from an
 * HTTP handler, never from anything relay/safety-adjacent). Writes up to
 * max_results entries into results and the actual count into *out_count.
 * Refused with ESP_ERR_NOT_SUPPORTED while mode == WIFI_PROV_MODE_AP: that
 * mode's whole point is no station-radio activity at all, and a scan (even
 * though it isn't a join) still means bringing the STA interface up. */
esp_err_t wifi_prov_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROV_H
