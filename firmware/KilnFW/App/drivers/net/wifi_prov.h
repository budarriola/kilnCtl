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
// THREADING (2026-08-19, TODO.md 10.14 Phase 4): this module has a single
// owning task. Every function below except wifi_prov_start() and the direct
// readers listed next is a thin PRODUCER -- it builds a command, posts it to
// the owner task's bounded queue, and blocks (bounded) for the answer, which
// is why every signature here is unchanged from before the conversion and why
// no caller needed an edit. Callers that must never block (lvgl_port_task)
// still have to keep these off that task themselves; the queue serializes the
// work, it does not make a blocking call non-blocking. See wifi_prov.c's
// "Owning task + command queue" comment for the full design, including why
// the Wi-Fi driver's own event handlers post here too.
//
// READERS THAT STAY DIRECT, and why -- same convention as thermo_owner.h
// leaving MAX31856_get_config() direct: routing a read that touches no
// hardware and no compound state through a task hop buys nothing and costs
// latency on paths that poll every UI tick.
//   - wifi_prov_get_state(), wifi_prov_get_mode(),
//     wifi_prov_is_sta_connected(), wifi_prov_get_sta_rssi(): each reads one
//     naturally-aligned word (an enum, an enum, the same enum compared, an
//     int8 next to a bool) that the owner task writes with a single store.
//     There is no read-modify-write and no multi-field invariant to observe
//     half-applied: the worst a racing reader can see is the value from just
//     before or just after a transition, which is exactly what it would see
//     through a queue anyway, one scheduling delay later.
//   - wifi_prov_get_saved_ssid()/get_ap_ssid()/get_ap_password(): these
//     return POINTERS into module storage, so a queue could not make them
//     safe even in principle -- the caller dereferences after any lock would
//     have been dropped. Unchanged, pre-existing, and bounded by the fact
//     that the buffers are fixed-size, always NUL-terminated, and only ever
//     rewritten in place.
//   - wifi_prov_get_ap_client_count(): reads no s_wifi field at all beyond
//     the started flag; it is an esp_wifi_ap_get_sta_list() call, and the
//     Wi-Fi driver's API is internally thread-safe.
//   - wifi_prov_get_saved_networks_cached() (2026-09-25, LCD freeze fix):
//     the one exception to "compound state goes through the queue" below --
//     it reads a dedicated short-spinlock-guarded MIRROR of the saved-list,
//     not s_wifi.saved_nets itself, so the torn-read hazard that rule exists
//     to avoid doesn't apply to it. See its own doc comment.
// Everything else reading COMPOUND state goes through the queue:
// wifi_prov_get_saved_networks(), wifi_prov_get_sta_ip(), wifi_prov_scan().
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

/* 2026-08-20 (web-GUI-only addition): how the STA interface gets its IP once
 * a home-network join succeeds. DHCP is the long-standing default behavior
 * (ESP-IDF's STA netif runs a DHCP client unless told otherwise) -- nothing
 * new happens for it. STATIC means the operator has supplied their own
 * ip/netmask/gateway; see wifi_prov_set_static_ip(). Deliberately orthogonal
 * to wifi_prov_mode_t (HOME vs AP): this only matters while in HOME mode and
 * is silently irrelevant in AP mode (the AP interface's own IP,
 * 192.168.4.1, is unrelated and not configurable here). */
typedef enum {
    WIFI_PROV_IP_MODE_DHCP = 0,
    WIFI_PROV_IP_MODE_STATIC,
} wifi_prov_ip_mode_t;

#define WIFI_PROV_IPV4_STR_MAX 16 /* "255.255.255.255" + NUL */

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

/* True exactly when wifi_prov_get_state() == WIFI_PROV_STATE_UNPROVISIONED AND
 * no STA network is saved (both checked, so an AP-fallback state that still has
 * credentials can never read as unprovisioned) --
 * home mode, AP-only, no STA credentials saved yet. Added 2026-09-28 for
 * http_auth_policy_iface.h's ROUTE_TIER_WIFI_SETUP predicate (owner decision:
 * /wifi, /networks and /scan stay open with no session ONLY during this
 * state, so the AP captive-portal first-time-setup flow works before any
 * credential exists). Deliberately narrower than "mode == WIFI_PROV_MODE_AP"
 * -- a user who chose permanent AP mode (WIFI_PROV_STATE_AP_MODE) despite
 * already having saved credentials is provisioned and must still require a
 * session; only the true zero-credential state opens these routes. */
bool wifi_prov_is_unprovisioned(void);

/* NULL-terminated. There is no longer one canonical "the" saved network --
 * this now returns the active/most-recently-attempted SSID (the one
 * apply_sta_config() last configured for a join), or if nothing is currently
 * active, the first entry in the saved-network list (list order, index 0) if
 * any are saved, or an empty string if none are. Existing callers (e.g. the
 * status page's "which network") keep working unchanged -- this is still a
 * single best-guess answer to "which network", just sourced from a list
 * instead of a single slot. Points at static storage; valid for the process
 * lifetime, no ownership transfer. */
const char *wifi_prov_get_saved_ssid(void);

/* NULL-terminated. The fallback AP's own SSID as currently in effect --
 * either the runtime override set via wifi_prov_set_ap_ssid(), or the
 * compile-time KILNCTL_WIFI_AP_SSID Kconfig default if no override has ever
 * been saved. Points at static storage; valid for the process lifetime, no
 * ownership transfer. */
const char *wifi_prov_get_ap_ssid(void);

/* NULL-terminated. The fallback AP's own password as currently in effect --
 * either the runtime override set via wifi_prov_set_ap_password(), or the
 * compile-time KILNCTL_WIFI_AP_DEFAULT_PASSWORD Kconfig default. Empty
 * string means an open (unsecured) AP. Points at static storage; valid for
 * the process lifetime, no ownership transfer.
 *
 * 2026-08-13, deliberately DIFFERENT from every saved *station* network's
 * password (see wifi_prov_get_saved_networks(), which structurally cannot
 * return one): this is the board's OWN access point identity, which an
 * operator standing in front of the board's setup page needs to see in
 * order to know what they just configured -- there is no "forget and
 * re-enter" workflow for the board's own AP the way there is for a joined
 * network. wifi_provision_http.c's /status handler exposes this in plain
 * text; a *saved network's* password must never be added there or
 * anywhere else. */
const char *wifi_prov_get_ap_password(void);

/* Saves ssid/password to NVS and (if in home mode) starts a join attempt.
 * ssid_len/password_len are the number of bytes at ssid/password, NOT
 * counting a NUL -- the caller (the HTTP handler, parsing an untrusted
 * request body) is expected to have already bounded them; this function
 * re-validates against WIFI_PROV_SSID_MAX_LEN/WIFI_PROV_PASSWORD_MAX_LEN and
 * returns ESP_ERR_INVALID_SIZE rather than truncating silently.
 * password_len may be 0 (open network). Submitting credentials while in AP
 * mode is treated as an explicit choice to switch to home mode -- see the
 * .c file for why (mirrors the old "local_only" exit path, now generalized
 * to the mode concept).
 *
 * (2026-08-13, TODO.md 8.4: this is now a thin wrapper around
 * wifi_prov_add_network() -- kept because other code still calls it under
 * this name. Prefer wifi_prov_add_network() for new callers.) */
esp_err_t wifi_prov_set_credentials(const char *ssid, size_t ssid_len, const char *password,
                                    size_t password_len);

/* Adds ssid/password to the bounded list of saved networks (see
 * WIFI_PROV_MAX_SAVED_NETWORKS in the .c file), or updates the password of an
 * already-saved network with the same SSID (exact string match). ssid_len/
 * password_len follow the same rules as wifi_prov_set_credentials() above --
 * NOT counting a NUL, re-validated here, ESP_ERR_INVALID_SIZE rather than
 * truncating. Returns ESP_ERR_NO_MEM without changing anything if the list is
 * already full (WIFI_PROV_MAX_SAVED_NETWORKS entries) and ssid is not already
 * one of them. On success, persists the whole list to NVS and, if currently
 * in home mode, starts (or restarts) a join attempt -- reusing whatever
 * network the auto-join tie-break in the .c file currently prefers, which is
 * not necessarily the network just added. Submitting a network while in AP
 * mode switches to home mode first, same as wifi_prov_set_credentials()
 * always has. */
esp_err_t wifi_prov_add_network(const char *ssid, size_t ssid_len, const char *password,
                                 size_t password_len);

/* Removes the saved network matching ssid (exact string match, ssid_len NOT
 * counting a NUL) from the list, compacts the remaining entries, and persists
 * the change. Explicitly allowed to bring the list to zero entries -- that is
 * not a special case here, it is just what "no saved networks" (formerly
 * has_creds == false) looks like. If ssid is not found, this is a no-op that
 * still returns ESP_OK (nothing to remove is not a failure). If the removed
 * network was the one currently active/connected, the existing disconnect/
 * reconnect event handlers discover there is nothing left to reconnect to on
 * their own -- this function does not itself force a disconnect. */
esp_err_t wifi_prov_forget_network(const char *ssid, size_t ssid_len);

typedef struct {
    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
} wifi_prov_saved_network_t;

/* Writes up to max_results currently-saved SSIDs (list order) into out and
 * the actual count into *out_count. SSID only -- there is deliberately no way
 * to get a saved password back out through this or any other function; the
 * password never leaves NVS/RAM once saved. */
esp_err_t wifi_prov_get_saved_networks(wifi_prov_saved_network_t *out, size_t max_results,
                                        size_t *out_count);

/* Non-blocking. Copies out the last-known saved-networks list from a short-
 * spinlock-guarded mirror (never the queue), refreshed by the owner task
 * whenever the real list changes and once at boot. Prefer this over
 * wifi_prov_get_saved_networks() from ANY task that must never block --
 * lvgl_port_task above all: that queued call is sized to wait behind a
 * worst-case scan already in flight (WIFI_OWNER_WAIT_MS, 12s in
 * wifi_prov_api.c), which is fine for an HTTP handler's own worker task but
 * freezes the whole LCD if called directly from the UI's timer/event
 * callbacks. The list this returns can be one add/forget behind the true
 * state for at most as long as the next owner-task command takes to run --
 * never seconds. Always succeeds; *out_count is 0 if wifi_prov_start()
 * hasn't run yet or nothing is saved. */
void wifi_prov_get_saved_networks_cached(wifi_prov_saved_network_t *out, size_t max_results,
                                          size_t *out_count);

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

/* 2026-09-28: true while home Wi-Fi is back up (CONNECTED) but the fallback
 * AP is deliberately being kept alive because a user is logged in to the
 * website (or, with web auth off, a client is still associated to the AP) --
 * see ap_teardown_should_defer() in wifi_prov_link.c. Direct, non-blocking
 * read, same convention as wifi_prov_is_sta_connected() above. */
bool wifi_prov_get_ap_pending_teardown(void);

/* Writes the station IP as dotted-quad text into out (out_cap must be at
 * least 16). Writes an empty string and returns ESP_ERR_INVALID_STATE if not
 * currently connected -- there is nothing to report, not a failure of this
 * call. */
esp_err_t wifi_prov_get_sta_ip(char *out, size_t out_cap);

/* Same round trip as wifi_prov_get_sta_ip() but also returns the STA
 * interface's CURRENT netmask (out_cap/netmask_cap must each be at least
 * 16) -- the actual DHCP- or static-assigned mask, not merely the
 * configured static_netmask (wifi_prov_get_static_netmask() below), which
 * reads back nothing useful in DHCP mode. Added for the login backoff
 * subnet check (web_auth_login_http.c): telling a LAN client from an
 * off-subnet one needs the real, currently-active mask regardless of IP
 * mode. Same "not connected" contract as wifi_prov_get_sta_ip(). */
esp_err_t wifi_prov_get_sta_ip_netmask(char *ip_out, size_t ip_cap, char *netmask_out, size_t netmask_cap);

/* Review fix (2026-09-21, finding 4 on the login backoff commit):
 * wifi_prov_get_sta_ip_netmask() above round-trips through the owner-task
 * queue and can block its caller for up to WIFI_OWNER_WAIT_MS (12s) -- never
 * safe to call while holding another module's lock, which is exactly what
 * web_auth_login_http.c's login_backoff_slot_for() used to do. This is the
 * non-blocking replacement for that one caller: a plain spinlocked copy of
 * a cache kept fresh by wifi_prov_update_sta_ip_cache() (owner_task() only,
 * refreshed on every GOT_IP event and every direct wifi_prov_get_sta_ip_
 * netmask()/wifi_prov_get_sta_ip() call). Always returns ESP_OK and writes
 * empty strings for both if the STA interface has no lease yet or the cache
 * has not been populated since boot -- that empty-string case is the same
 * "no lease" fallback the blocking getter already returns, so callers that
 * already tolerate that (login_ip_scope_classify()) need no special case. */
esp_err_t wifi_prov_get_cached_sta_ip_netmask(char *ip_out, size_t ip_cap, char *netmask_out, size_t netmask_cap);

/* Returns RSSI (signal strength in dBm) of the currently-connected station, or
 * -127 if not connected. RSSI is negative; -30 is excellent, -90 is weak. */
int8_t wifi_prov_get_sta_rssi(void);

/* Returns the number of clients currently connected to the AP (when in AP mode
 * or AP+STA fallback). Returns 0 if AP is not currently active. */
uint8_t wifi_prov_get_ap_client_count(void);

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

/* ---- Static IP (2026-08-20, web-GUI-only) -------------------------------
 * Web-only feature: there is deliberately no LCD UI and no UART-bridge
 * subcommand for any of this -- wifi_provision_http.c/wifi_provision_page.html
 * are the only callers. */

/* Current IP mode for the STA interface -- direct read, same reasoning as
 * wifi_prov_get_state()/wifi_prov_get_mode() (one aligned enum word, single
 * store from the owner task, no read-modify-write to observe half-applied). */
wifi_prov_ip_mode_t wifi_prov_get_ip_mode(void);

/* NUL-terminated dotted-quad strings, valid only (i.e. non-empty) when
 * wifi_prov_get_ip_mode() == WIFI_PROV_IP_MODE_STATIC; empty strings
 * otherwise. Points at static storage, same lifetime/ownership contract as
 * wifi_prov_get_ap_ssid() etc. Used by /status to prefill the web page's
 * static-IP fields on load. */
const char *wifi_prov_get_static_ip(void);
const char *wifi_prov_get_static_netmask(void);
const char *wifi_prov_get_static_gateway(void);

/* Switches the STA interface back to DHCP (the default/original behavior).
 * Persists the choice and, if a station join is currently active or in
 * progress, forces a disconnect+reconnect so the netif picks the DHCP
 * client back up rather than sitting on a stale static IP. Safe to call
 * repeatedly / while already in DHCP mode (a no-op re-apply). */
esp_err_t wifi_prov_set_dhcp(void);

/* Validates ip/netmask/gateway as dotted-quad IPv4 strings (rejects anything
 * else with ESP_ERR_INVALID_ARG -- malformed input never reaches
 * esp_netif_set_ip_info()), persists them plus IP mode == STATIC, and -- if
 * a station join is currently active or in progress -- applies it
 * immediately: esp_netif_dhcpc_stop() + esp_netif_set_ip_info() before the
 * next esp_wifi_connect(), forcing a disconnect+reconnect if already
 * connected so the new address takes effect right away rather than only on
 * the next boot. Each string must be non-NULL and NUL-terminated.
 *
 * A bad-but-well-formed config (unreachable gateway, wrong subnet) is NOT
 * caught here -- IPv4 syntax is the only thing validated, same as any real
 * router's static-IP form. The 802.11 association itself doesn't depend on
 * L3 correctness, so the join can still reach WIFI_PROV_STATE_CONNECTED
 * (GOT_IP fires locally once the static IP is applied, not from a DHCP
 * handshake) even though the resulting address is unreachable.
 *
 * 2026-08-21 fix (TODO.md section 1's "known gap"): that GOT_IP alone no
 * longer tears down the fallback AP for a static join -- see wifi_prov.c's
 * apply_sta_config() comment and s_wifi.static_ip_confirmed. The AP stays up
 * until wifi_prov_note_possible_static_reachability() (below) reports a real
 * HTTP request actually reached the board at the static address, so a bad
 * config is reachable via the AP indefinitely rather than only until the
 * next power cycle. */
esp_err_t wifi_prov_set_static_ip(const char *ip, const char *netmask, const char *gateway);

/* True when `ip` parses as IPv4 and falls inside 192.168.4.0/24, the fallback
 * AP's own subnet -- an address there is never a legitimate static-IP choice
 * (see wifi_prov_set_static_ip()'s own refusal of it). Returns false for a
 * string that doesn't parse at all; callers needing to distinguish
 * "malformed" from "in the AP subnet" run their own parse first. Exported so
 * wifi_provision_http.c's /ip_config handler can report a distinct message
 * for this specific refusal rather than the generic dotted-quad one. */
bool wifi_prov_ip_in_ap_subnet(const char *ip);

/* 2026-08-21: called by wifi_provision_http.c's handlers with the fd of the
 * socket that just served an HTTP request (httpd_req_to_sockfd(req)), so
 * this module can tell whether that request arrived on the STA static
 * address specifically -- the one piece of ground truth GOT_IP alone can't
 * provide. A no-op unless ip_mode is STATIC and not yet confirmed; matching
 * the static IP drops the fallback AP (see do_ev_got_ip()/
 * do_confirm_static_reachable() in wifi_prov.c). Cheap and safe to call
 * unconditionally on every request -- most calls are a few direct-read
 * comparisons and return immediately. */
void wifi_prov_note_possible_static_reachability(int sockfd);

/* True when the request served on `sockfd` arrived on the board's own SoftAP
 * interface rather than over the home network. Pass httpd_req_to_sockfd(req).
 *
 * Exists so GET /status can decide whether echoing the AP password back is a
 * disclosure: a client associated to the AP already had to know it, whereas
 * the same field served over the STA interface hands it to every device on
 * the LAN. Fails closed -- an uninspectable socket reports false. */
bool wifi_prov_request_arrived_on_ap(int sockfd);

/* 2026-09-17 disclosure fix: GET /status (ROUTE_TIER_OPEN -- an
 * unprovisioned board must answer it before anyone can authenticate, same
 * reasoning as wifi_prov_request_arrived_on_ap() above) used to
 * unconditionally emit the saved home network's SSID and, when configured,
 * the STA static-IP topology (static_ip/static_netmask/static_gateway) to
 * any unauthenticated caller on the LAN. The route stays OPEN; the fix is
 * in the payload. Pure formatter (facts in, JSON fragment out) for the same
 * host-testability reason readiness_http.h's readiness_*_status()
 * predicates are pure: given the caller's disclosure eligibility and an
 * already-prepared value (already JSON-escaped by the caller if it needs
 * escaping -- an SSID can contain '"'/'\\'; a dotted-quad IP string never
 * does and needs none), emits either `"value"` or JSON `null` -- never
 * omits the key, so key presence itself never becomes a signal, and the
 * existing web UI keeps parsing the same shape either way.
 *
 * Callers pass `may_disclose = !http_auth_policy_web_enabled() ||
 * http_auth_caller_is_admin(req)` -- a disjunction, not `&&`, because
 * http_auth_caller_is_admin() already returns true when web auth is OFF
 * (that's what lets an operator set the first admin password on a fresh
 * board); `&&` here would hide these fields from that operator and from
 * this project's own commissioning tooling on an unprovisioned board.
 * sta_ip is deliberately NOT run through this redaction anywhere it's used:
 * a caller able to reach this route at all already used an address to get
 * here, so withholding it narrows nothing an unauthenticated LAN caller
 * doesn't already have. out_cap must be at least strlen(value)+3 (two
 * quotes plus NUL) for a full, non-truncated value when may_disclose is
 * true -- same "truncation is fine, overrun is not" convention as
 * wifi_provision_http.c's json_escape(). */
static inline void wifi_prov_status_redact_field(bool may_disclose, const char *value, char *out,
                                                  size_t out_cap)
{
    if (may_disclose) {
        snprintf(out, out_cap, "\"%s\"", value ? value : "");
    } else {
        snprintf(out, out_cap, "null");
    }
}

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROV_H
