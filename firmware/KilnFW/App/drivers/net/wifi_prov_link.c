/* wifi_prov.c split (2026-09-04, ROADMAP.md M15 A3) -- Wi-Fi driver config
 * application, the Wi-Fi/IP event handlers, the AP-fallback and periodic
 * rescan timers, ground-truth reconciliation, and the captive-portal DNS
 * hijack task. See wifi_prov_internal.h's top-of-file comment for the full
 * split rationale and file map. Move-only: no logic, ordering, naming or
 * visibility change beyond widening former `static` symbols this split's
 * sibling files now call directly.
 *
 * As documented at length in wifi_prov.c (unchanged by this split): every
 * function here that is reachable from an esp_event handler or an esp_timer
 * callback runs on owner_task(), never on the Wi-Fi driver's own
 * default-event-loop task or the timer service task -- those two only ever
 * post_event() and return. s_wifi has exactly one writer no matter which of
 * these four files a given piece of that writer's logic now lives in. */

#include "wifi_prov_internal.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "settings.h"
#include "time_sync.h"

/* 2026-09-28 owner request: "reconnect to wifi ... when there are no users
 * logged in to the website". Both http_session_iface.h and
 * http_auth_policy_iface.h are flat INCLUDE_DIRS entries of the SAME
 * `drivers` component as everything under net/, so this cross-subdirectory
 * include needs no path tricks -- confirmed against
 * firmware/KilnFW/App/drivers/CMakeLists.txt before adding it. */
#include "http_auth_policy_iface.h"
#include "http_session_iface.h"

/* ---- Wi-Fi driver config helpers -------------------------------------- */

void apply_ap_config(void)
{
    wifi_config_t ap_cfg = { 0 };
    const char *ssid = s_wifi.has_ap_ssid_override ? s_wifi.ap_ssid : WIFI_AP_SSID;
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    const char *pw = s_wifi.has_ap_password_override ? s_wifi.ap_password : WIFI_AP_DEFAULT_PASSWORD;
    if (strlen(pw) >= 8) {
        strncpy((char *)ap_cfg.ap.password, pw, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        /* WPA2-PSK requires an 8-63 char password; a shorter configured
         * default would otherwise fail esp_wifi_set_config outright. Open is
         * safer than silently refusing to start the AP a phone needs to
         * reach in order to provision the board at all. */
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        /* wifi_prov_set_ap_password() itself refuses a 1-7 char override, so
         * this only fires for a misconfigured KILNCTL_WIFI_AP_DEFAULT_PASSWORD
         * (Kconfig isn't validated the same way) or a deliberate empty
         * override (open AP). */
        ESP_LOGW(WIFI_PROV_TAG, "AP password is shorter than 8 chars -- AP is OPEN");
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
    }
}

/* 2026-08-20, web-GUI-only: parses a dotted-quad IPv4 string via lwIP's
 * ip4addr_aton() (lwip/ip4_addr.h) into an esp_ip4_addr_t. Both types are a
 * single uint32_t addr in network byte order under the hood, hence the
 * plain .addr copy rather than a cast -- they are deliberately different
 * *types* (lwIP's own vs. esp_netif's public one) even though the layout
 * matches. Returns false (out left untouched) on anything that doesn't
 * parse, including empty/NULL -- callers must not feed that to
 * esp_netif_set_ip_info(). */
bool parse_ipv4(const char *s, esp_ip4_addr_t *out)
{
    if (!s || !*s) {
        return false;
    }
    ip4_addr_t addr;
    if (ip4addr_aton(s, &addr) == 0) {
        return false;
    }
    out->addr = addr.addr;
    return true;
}

/* Configures the STA driver for whatever is currently in s_wifi.active_ssid/
 * active_password -- callers are responsible for having set those first (see
 * select_and_apply_join_candidate() and the direct nets[0] assignment in
 * wifi_prov_start()).
 *
 * 2026-08-20, web-GUI-only static-IP addition: also applies s_wifi.ip_mode to
 * the STA netif, BEFORE returning to every caller's subsequent
 * esp_wifi_connect() -- start_sta_join(), do_rescan_tick(), and
 * wifi_prov_start()'s own initial-config branch all call this and only then
 * call/trigger esp_wifi_connect() (the latter via WIFI_EVENT_STA_START once
 * esp_wifi_start() runs). ESP-IDF's own static-IP examples set this up
 * BEFORE the STA associates, not after -- doing it here, inside the one
 * function every join path already funnels through, is what makes that
 * ordering automatic instead of something each call site has to remember.
 *
 * KNOWN L2/L3 GAP, still true: 802.11 association does not depend on L3
 * correctness. A wrong-but-well-formed static config (bad gateway, wrong
 * subnet) still lets the join reach WIFI_PROV_STATE_CONNECTED -- esp_netif
 * fires IP_EVENT_STA_GOT_IP locally once a static IP is set and the link
 * associates, it does not wait on a DHCP handshake that would otherwise
 * time out and let ap_fallback_timer notice a failure. This module cannot
 * make GOT_IP itself prove reachability -- that would need an active probe
 * (ARP/ping the gateway) this file doesn't have the dependencies for.
 *
 * 2026-08-21 FIX (TODO.md section 1's "known gap"): what changed is what
 * do_ev_got_ip() DOES with an unconfirmed static GOT_IP -- it no longer
 * drops the fallback AP on trust alone. See s_wifi.static_ip_confirmed's
 * comment: the AP now stays up alongside the (possibly-bad) static join
 * until an actual HTTP request arrives addressed to the static IP itself,
 * proving something on the LAN can actually reach it. A bad static config
 * therefore now self-heals to "AP still up, reachable, no power-cycle
 * needed" instead of the old "reports connected, actually stranded until a
 * saved network rejoins or someone power-cycles it." A GOOD static config
 * still ends up STA-only, just one HTTP round-trip later than before. */
void apply_sta_config(void)
{
    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, s_wifi.active_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_wifi.active_password, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = strlen(s_wifi.active_password) > 0 ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
    }

    if (!s_wifi.sta_netif) {
        return;
    }
    if (s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC) {
        esp_ip4_addr_t ip, netmask, gw;
        if (parse_ipv4(s_wifi.static_ip, &ip) && parse_ipv4(s_wifi.static_netmask, &netmask) &&
            parse_ipv4(s_wifi.static_gateway, &gw)) {
            esp_err_t dhcp_err = esp_netif_dhcpc_stop(s_wifi.sta_netif);
            if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                ESP_LOGW(WIFI_PROV_TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(dhcp_err));
            }
            esp_netif_ip_info_t ip_info = { .ip = ip, .netmask = netmask, .gw = gw };
            esp_err_t set_err = esp_netif_set_ip_info(s_wifi.sta_netif, &ip_info);
            if (set_err != ESP_OK) {
                ESP_LOGE(WIFI_PROV_TAG, "esp_netif_set_ip_info failed: %s -- static IP not applied for this join",
                         esp_err_to_name(set_err));
            }
        } else {
            /* Stored config that no longer parses (shouldn't happen --
             * wifi_prov_set_static_ip() validates before ever persisting
             * this -- but NVS is not immune to bit rot). Fail toward
             * reachability rather than an unconfigured/zero IP. */
            ESP_LOGE(WIFI_PROV_TAG, "stored static IP config failed to parse -- falling back to DHCP for this join");
            esp_netif_dhcpc_start(s_wifi.sta_netif);
        }
    } else {
        esp_err_t dhcp_err = esp_netif_dhcpc_start(s_wifi.sta_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(WIFI_PROV_TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(dhcp_err));
        }
    }
}

/* Selects which saved network to attempt next and configures the STA driver
 * for it (active_ssid/active_password + apply_sta_config()). Only called
 * from home-mode join paths (start_sta_join() and the disconnect branch of
 * on_wifi_event()) where s_wifi.saved_nets.count > 0 is already guaranteed
 * by the caller.
 *
 * Tie-break policy (2026-08-13, TODO.md 8.4): scan first and prefer the
 * HIGHEST-RSSI saved network actually in range. Iterating the saved list in
 * order and only replacing the current best on a STRICT improvement means
 * equal RSSI favors the earlier entry in the saved list -- list order is a
 * secondary preference, applied automatically by this iteration order rather
 * than as an explicit second comparison. If the scan finds NONE of the saved
 * SSIDs in range (scan failed, everything out of range, or a saved network is
 * hidden and doesn't show up in an active scan), fall back to the saved list
 * in order starting at index 0 -- today's effective single-network behavior,
 * generalized. This never blocks or skips connecting just because the scan
 * came up empty of matches.
 *
 * 2026-08-19, TODO.md 10.14 Phase 4 -- the CAUTION that used to stand here is
 * RESOLVED, and the resolution is worth stating because it was the single
 * largest reason this function was left half-finished by TODO.md 8.4. The old
 * warning was: this does a genuinely blocking scan, and on_wifi_event() runs
 * on the Wi-Fi driver's own default-event-loop task, so a disconnect-triggered
 * reconnect stalled the driver's event processing for the scan's duration.
 * That can no longer happen: on_wifi_event() does nothing but post a command
 * now, and this function only ever runs on owner_task(), a task of this
 * module's own whose whole job is to be the thing that blocks. A scan here
 * delays other *Wi-Fi commands* (intended -- they contend for one radio), not
 * the driver's event delivery.
 *
 * It calls do_scan() rather than the public wifi_prov_scan() for the deadlock
 * reason given at do_scan()'s forward declaration. */
static void select_and_apply_join_candidate(void)
{
    if (s_wifi.saved_nets.count == 0) {
        return;
    }

    int best_idx = -1;
    int8_t best_rssi = INT8_MIN;

    static wifi_prov_scan_result_t scan_results[20];
    size_t scan_count = 0;
    esp_err_t scan_err =
        do_scan(scan_results, sizeof(scan_results) / sizeof(scan_results[0]), &scan_count);
    if (scan_err == ESP_OK) {
        for (uint8_t i = 0; i < s_wifi.saved_nets.count; i++) {
            for (size_t s = 0; s < scan_count; s++) {
                if (strcmp(scan_results[s].ssid, s_wifi.saved_nets.nets[i].ssid) != 0) {
                    continue;
                }
                if (best_idx < 0 || scan_results[s].rssi > best_rssi) {
                    best_idx = i;
                    best_rssi = scan_results[s].rssi;
                }
                break; /* saved net i found in the scan; move to the next saved net */
            }
        }
    } else if (scan_err != ESP_ERR_NOT_SUPPORTED) {
        /* ESP_ERR_NOT_SUPPORTED (AP-only mode) shouldn't happen here since
         * this only runs from home-mode join paths, but isn't worth logging
         * as a warning if it somehow does -- anything else genuinely is. */
        ESP_LOGW(WIFI_PROV_TAG, "auto-join scan failed: %s -- falling back to saved-list order", esp_err_to_name(scan_err));
    }

    if (best_idx < 0) {
        best_idx = 0; /* nothing in range matched -- try list order, starting at 0 */
    }

    strncpy(s_wifi.active_ssid, s_wifi.saved_nets.nets[best_idx].ssid, sizeof(s_wifi.active_ssid) - 1);
    s_wifi.active_ssid[sizeof(s_wifi.active_ssid) - 1] = '\0';
    strncpy(s_wifi.active_password, s_wifi.saved_nets.nets[best_idx].password,
            sizeof(s_wifi.active_password) - 1);
    s_wifi.active_password[sizeof(s_wifi.active_password) - 1] = '\0';

    apply_sta_config();
}

/* 2026-09-28 owner request predicate: should the fallback AP's teardown be
 * DEFERRED right now? Two signals, chosen for honesty over convenience (the
 * owner's own wording: "pick the most honest signal"):
 *
 *   - Web auth ON: ask http_auth_any_session_active() -- the actual session
 *     table, which already excludes expired/idle sessions
 *     (web_auth_session_is_valid()). An idle-timed-out browser tab must NOT
 *     hold the AP up forever; only a genuinely still-valid session defers.
 *   - Web auth OFF: there is no session table to ask (nothing ever creates a
 *     session when auth is off), so the honest signal is "is anything
 *     actually associated to the AP radio right now" --
 *     wifi_prov_get_ap_client_count() (esp_wifi_ap_get_sta_list(), already
 *     used by /status's ap_clients field). HTTP-request-recency was
 *     considered and rejected: a client can be mid-page-load or about to
 *     poll again with no request in flight at the exact instant this runs,
 *     whereas a station that is still associated to the radio is
 *     unambiguous ground truth requiring no arbitrary "recent enough"
 *     window.
 *
 * Runs on owner_task() only (called from do_ev_got_ip()/
 * do_confirm_static_reachable()/do_rescan_tick(), same as every other
 * function in this file) -- http_auth_any_session_active() and
 * wifi_prov_get_ap_client_count() are both safe to call from any task
 * (neither touches s_wifi), so this adds no new cross-task hazard. */
static bool ap_teardown_should_defer(void)
{
    if (http_auth_policy_web_enabled()) {
        return http_auth_any_session_active();
    }
    return wifi_prov_get_ap_client_count() > 0;
}

void cancel_ap_fallback_timer(void)
{
    if (s_wifi.ap_fallback_timer) {
        /* esp_timer_stop on an already-stopped one-shot timer is a no-op
         * error we don't care about. */
        esp_timer_stop(s_wifi.ap_fallback_timer);
    }
}

/* ---- Ground-truth reconciliation ---------------------------------------
 *
 * 2026-08-20, bench-observed bug this exists to fix. After an AP -> home
 * switch (wifi_prov_set_mode(HOME) -> do_set_mode() -> start_sta_join()),
 * the station really did join and stayed joined -- the board answered
 * `GET http://192.168.1.156/api/status` with a 200 and a valid JSON body,
 * and the host ARP table mapped 192.168.1.156 to 1c-db-d4-92-f4-7c, the
 * board's STA MAC (its AP BSSID is ...:7d, i.e. STA = AP-1), so it held a
 * real DHCP lease on the LAN. Meanwhile this module reported
 * state=WIFI_PROV_STATE_RECONNECTING(4), sta_connected=false, sta_ip="",
 * rssi=-127, and stayed that way indefinitely (30+ s of polling).
 *
 * Mechanism, confirmed by reading the paths rather than guessing: state is
 * only ever set to WIFI_PROV_STATE_CONNECTED in do_ev_got_ip(), i.e. only
 * on IP_EVENT_STA_GOT_IP. A WIFI_EVENT_STA_DISCONNECTED that lands AFTER
 * that GOT_IP -- a queued retry/auth failure from the join attempt itself,
 * or the AP interface teardown do_ev_got_ip() performs with
 * esp_wifi_set_mode(WIFI_MODE_STA) -- runs do_ev_sta_disconnected(), which
 * unconditionally forces state back to RECONNECTING and calls
 * esp_wifi_connect(). Since the station never actually dropped, that
 * connect returns ESP_ERR_WIFI_CONN (already connected) and NO further
 * GOT_IP is ever generated, so nothing exists that can put the state back.
 * The bad state is then permanent, and it also re-arms the AP fallback
 * timer (which raises the AP again) and keeps do_rescan_tick() scanning
 * and re-connecting on a link that is fine.
 *
 * The repair is to stop trusting the event stream as the sole source of
 * truth. esp_wifi_sta_get_ap_info() (are we associated?) plus
 * esp_netif_get_ip_info() (do we hold a non-zero lease?) are authoritative
 * and do not depend on having caught, or not caught, any particular event.
 * Both helpers below run ONLY on owner_task() -- they are called from
 * do_*() bodies, never from an event handler or a caller thread, so the
 * file's "s_wifi has exactly one writer" rule is unchanged. */
static bool sta_link_is_live(int8_t *out_rssi, esp_netif_ip_info_t *out_ip_info)
{
    if (!s_wifi.sta_netif) {
        return false;
    }
    wifi_ap_record_t ap_info = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false; /* not associated with anything */
    }
    esp_netif_ip_info_t ip_info = { 0 };
    if (esp_netif_get_ip_info(s_wifi.sta_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false; /* associated but no lease -- a join genuinely in progress */
    }
    if (out_rssi) {
        *out_rssi = ap_info.rssi;
    }
    if (out_ip_info) {
        *out_ip_info = ip_info;
    }
    return true;
}

/* Runs on owner_task(). Returns true if the station is demonstrably up
 * (association + non-zero IP), repairing s_wifi.state/sta_rssi if they
 * disagreed. Callers use the return value to decide whether a "recover the
 * link" action is needed at all.
 *
 * Coordinator review follow-up (2026-09-25): this is also the only place
 * left that notices a live station on a tick basis -- eec35084 removed
 * do_get_sta_ip()'s per-tick refresh of the cheap-read IP cache
 * (wifi_prov_get_cached_sta_ip_netmask()) in favor of a non-blocking read
 * from the LVGL task. If post_event() (wifi_prov.c) drops a queued GOT_IP
 * because the owner mailbox is full, reconcile_sta_state() is what next
 * notices the station is actually up and flips state back to CONNECTED --
 * but it never refreshed the IP cache itself, so a display could show
 * CONNECTED with a stale or empty IP indefinitely after a dropped event.
 * sta_link_is_live() already fetches ip_info to prove there's a lease;
 * reuse that same read here instead of calling esp_netif_get_ip_info()
 * again. */
bool reconcile_sta_state(void)
{
    if (s_wifi.mode != WIFI_PROV_MODE_HOME) {
        return false; /* AP mode: the station is intentionally not in use */
    }
    int8_t rssi = -127;
    esp_netif_ip_info_t ip_info = { 0 };
    if (!sta_link_is_live(&rssi, &ip_info)) {
        return false;
    }
    s_wifi.sta_rssi = rssi;
    char ip_str[16];
    char mask_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, (uint32_t)sizeof(ip_str));
    esp_ip4addr_ntoa(&ip_info.netmask, mask_str, (uint32_t)sizeof(mask_str));
    wifi_prov_update_sta_ip_cache(ip_str, mask_str);
    if (s_wifi.state != WIFI_PROV_STATE_CONNECTED) {
        ESP_LOGW(WIFI_PROV_TAG,
                 "state said %d but the station is associated with a live IP -- correcting to CONNECTED",
                 (int)s_wifi.state);
        s_wifi.state = WIFI_PROV_STATE_CONNECTED;
        cancel_ap_fallback_timer();
    }
    return true;
}

/* Runs on owner_task(), posted for by ap_fallback_timer_cb() below (which runs
 * on the esp_timer service task and does nothing but post). */
void do_ap_fallback_tick(void)
{
    if (s_wifi.state == WIFI_PROV_STATE_CONNECTED) {
        return; /* reconnected before the timer fired */
    }
    /* 2026-08-20: and don't raise the AP over a state that merely LOOKS
     * unconnected -- same ground-truth check as do_rescan_tick(), for the
     * same reason (reconcile_sta_state()'s header). */
    if (reconcile_sta_state()) {
        return;
    }
    s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
    /* 2026-09-28 review fix: if the AP is already running (boot join and
     * start_sta_join() both start in APSTA, and a deferred teardown leaves
     * it up), leave it alone -- re-running set_mode/apply_ap_config() on a
     * live AP is the same "re-apply to force clients to re-pick it up" move
     * do_set_ap_password() makes deliberately, and must not happen here as a
     * side effect of a timer. See s_wifi.ap_fallback_active's comment. */
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur_mode) == ESP_OK && (cur_mode == WIFI_MODE_APSTA || cur_mode == WIFI_MODE_AP)) {
        ESP_LOGW(WIFI_PROV_TAG, "station join did not land within the timeout -- fallback AP already up, "
                                "retrying on the rescan cadence");
        s_wifi.ap_fallback_active = true;
        return;
    }
    ESP_LOGW(WIFI_PROV_TAG, "station join did not land within the timeout -- bringing the fallback AP up");
    /* Mode first, then config -- the current mode here can be STA-only (see
     * on_ip_event()), which does not include the AP interface; the same
     * ESP_ERR_WIFI_MODE ordering bug fixed in wifi_prov_start(). */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
    } else {
        apply_ap_config();
        s_wifi.ap_fallback_active = true;
    }
}

void ap_fallback_timer_cb(void *arg)
{
    (void)arg;
    post_event(CMD_TMR_AP_FALLBACK);
}

/* TODO.md 8.4: periodically look for a saved network while the board is
 * sitting in AP fallback that the OPERATOR did not choose -- i.e. mode is
 * still WIFI_PROV_MODE_HOME (a deliberate switch to WIFI_PROV_MODE_AP is
 * left alone; that AP is intentional, permanent, and this must never
 * second-guess it) but state isn't WIFI_PROV_STATE_CONNECTED, meaning
 * either no join has landed yet or a previous one dropped and is being
 * retried.
 *
 * 2026-08-19, Phase 4: this now runs on owner_task() (posted for by
 * rescan_timer_cb() below), not on the esp_timer service task. The original
 * reason this path existed at all -- "the timer service task is a safe place
 * to block on a scan, the Wi-Fi event-loop task is not" -- is subsumed by
 * there now being ONE task where every blocking Wi-Fi operation happens. The
 * periodic cadence is still worth keeping for its own sake (it re-picks a
 * better candidate rather than retrying the same one), so nothing about the
 * policy changed, only which task executes it. */
void do_rescan_tick(void)
{
    /* 2026-09-28 owner request: home Wi-Fi already came back but the AP
     * teardown was deferred (do_ev_got_ip()/do_confirm_static_reachable())
     * because someone was logged in / a client was on the AP at that moment.
     * Reuse this same 30s cadence to recheck rather than adding a second
     * timer -- once nobody is logged in (or, with auth off, no client is
     * associated to the AP), complete the teardown that was put off. This
     * must run even though state == CONNECTED, which is why it is checked
     * and returned from before the early-return below that skips everything
     * else while connected. */
    if (s_wifi.state == WIFI_PROV_STATE_CONNECTED && s_wifi.ap_pending_teardown) {
        if (ap_teardown_should_defer()) {
            return; /* still logged in / still a client on the AP -- try again next tick */
        }
        s_wifi.ap_pending_teardown = false;
        s_wifi.ap_fallback_active = false;
        ESP_LOGI(WIFI_PROV_TAG, "no user logged in (or no AP client) any more -- dropping deferred fallback AP");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (s_wifi.mode != WIFI_PROV_MODE_HOME || s_wifi.state == WIFI_PROV_STATE_CONNECTED ||
        s_wifi.saved_nets.count == 0) {
        return;
    }
    /* 2026-08-20: second half of the fix described at reconcile_sta_state().
     * Suppressing the one known bad event path is not enough -- ANY missed or
     * mis-ordered event could leave state disagreeing with the radio, and
     * nothing else in this file ever re-checks. This periodic tick is the
     * natural self-heal point: if the station is in fact associated with a
     * live IP, repair the state and skip the retry entirely rather than
     * scanning and re-connecting a link that is already up. */
    if (reconcile_sta_state()) {
        return;
    }
    ESP_LOGI(WIFI_PROV_TAG, "periodic rescan: looking for a saved network while in AP fallback");
    select_and_apply_join_candidate();
    esp_wifi_connect();
}

void rescan_timer_cb(void *arg)
{
    (void)arg;
    post_event(CMD_TMR_RESCAN);
}

void start_ap_fallback_timer(void)
{
    if (!s_wifi.ap_fallback_timer) {
        return;
    }
    cancel_ap_fallback_timer();
    esp_err_t err =
        esp_timer_start_once(s_wifi.ap_fallback_timer, (uint64_t)WIFI_STA_CONNECT_TIMEOUT_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

/* Common to wifi_prov_add_network() and wifi_prov_set_mode(HOME): starts (or
 * restarts) a station join attempt, AP staying up alongside it until the
 * join is confirmed. Only meaningful when s_wifi.saved_nets.count > 0 already
 * and mode is already WIFI_PROV_MODE_HOME -- callers are responsible for
 * having gotten there first. This is the "don't strand the phone" guarantee:
 * the AP is never torn down before IP_EVENT_STA_GOT_IP confirms the join
 * actually worked. Picks which saved network to try via
 * select_and_apply_join_candidate()'s scan-based tie-break -- see that
 * function's comment for the policy and the blocking-scan caveat. */
void start_sta_join(void)
{
    select_and_apply_join_candidate();
    cancel_ap_fallback_timer();
    /* Operator-initiated join: fast immediate retries again for this
     * attempt's own timeout window (see s_wifi.ap_fallback_active). */
    s_wifi.ap_fallback_active = false;
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(mode_err));
    }
    start_ap_fallback_timer();
    esp_wifi_connect();
}

/* ---- Event handlers ----------------------------------------------------
 * The two esp_event handlers below run on the default event loop's own task,
 * and the two esp_timer callbacks above run on the timer service task. As of
 * 2026-08-19 (TODO.md 10.14 Phase 4) NONE of the four touches s_wifi: each is
 * a single post_event() call, and the real work happens in the do_*() bodies
 * here, on owner_task(). That is the whole point of Phase 4 -- with these left
 * mutating state directly, "s_wifi has one writer" would have been false no
 * matter what the public API did.
 *
 * Nothing in any of this touches kiln_io/safety_link (unchanged requirement,
 * see this module's header). */

/* Runs on owner_task(). */
void do_ev_sta_start(void)
{
    if (s_wifi.saved_nets.count > 0 && s_wifi.mode == WIFI_PROV_MODE_HOME) {
        esp_wifi_connect();
    }
}

/* Runs on owner_task(). */
void do_ev_sta_disconnected(void)
{
    {
        s_wifi.sta_rssi = -127; /* lost connection */
        if (s_wifi.mode == WIFI_PROV_MODE_AP || s_wifi.saved_nets.count == 0) {
            return; /* not attempting station at all */
        }
        /* 2026-08-20: a disconnect event is NOT proof the station is down.
         * See reconcile_sta_state()'s header for the bench evidence -- a
         * stale disconnect arriving after GOT_IP (queued join retry, or the
         * AP teardown do_ev_got_ip() does) used to pin state at RECONNECTING
         * forever on a link that was serving HTTP. Ask the driver instead:
         * if we are still associated AND still hold a lease, this event
         * describes something that already healed (or never applied to the
         * current association), so leave the working link alone. Touching
         * the radio here would be actively harmful -- esp_wifi_connect() on
         * an established link just returns ESP_ERR_WIFI_CONN, and the
         * fallback-timer re-arm below would raise the AP for no reason. */
        if (reconcile_sta_state()) {
            ESP_LOGI(WIFI_PROV_TAG, "ignoring stale STA disconnect -- association and IP are both still live");
            return;
        }
        bool was_connected = (s_wifi.state == WIFI_PROV_STATE_CONNECTED);
        s_wifi.state = was_connected ? WIFI_PROV_STATE_RECONNECTING : WIFI_PROV_STATE_CONNECTING;
        /* The station is really down, so a deferred AP teardown (which only
         * means anything while CONNECTED) is no longer pending; the next
         * GOT_IP re-decides it. */
        s_wifi.ap_pending_teardown = false;
        if (s_wifi.ap_fallback_active) {
            /* 2026-09-28 review fix: the fallback AP is already up and may
             * have clients on it. No immediate retry and no timer re-arm --
             * do_rescan_tick()'s 30 s cadence retries the join instead. See
             * s_wifi.ap_fallback_active's comment for why. */
            s_wifi.state = WIFI_PROV_STATE_RECONNECTING;
            ESP_LOGI(WIFI_PROV_TAG, "station disconnected while the fallback AP is up -- next join attempt on the "
                                    "rescan cadence");
            return;
        }
        ESP_LOGI(WIFI_PROV_TAG, "station disconnected, retrying join");
        /* Still deliberately NOT re-running select_and_apply_join_candidate()'s
         * scan-based tie-break here, but for a DIFFERENT reason than the
         * 2026-08-13 fix this replaces. That reason was "this runs on the
         * Wi-Fi driver's event-loop task and must not block it" -- no longer
         * true after Phase 4 (this is owner_task() now). The reason it stays
         * is behavioral: a flapping link produces disconnect after
         * disconnect, and running a multi-second scan on each one would keep
         * the owner task -- and therefore every operator-initiated Wi-Fi
         * command behind it -- busy scanning instead of reconnecting. Retry
         * the SAME active_ssid/active_password (fast); let do_rescan_tick()'s
         * fixed cadence be the thing that re-picks a better candidate. */
        esp_wifi_connect();
        if (!s_wifi.ap_fallback_timer) {
            return;
        }
        /* Only (re)arm the fallback timer if it isn't already counting down
         * from a previous disconnect -- esp_timer_start_once on an already
         * running timer returns ESP_ERR_INVALID_STATE, which is fine to
         * ignore, but re-arming would keep pushing the AP fallback out on a
         * flapping link instead of ever bringing it back. */
        esp_timer_start_once(s_wifi.ap_fallback_timer,
                              (uint64_t)WIFI_STA_CONNECT_TIMEOUT_MS * 1000);
    }
}

void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_START) {
        post_event(CMD_EV_STA_START);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        post_event(CMD_EV_STA_DISCONNECTED);
    }
}

/* Runs on owner_task(). */
void do_ev_got_ip(void)
{
    cancel_ap_fallback_timer();
    s_wifi.state = WIFI_PROV_STATE_CONNECTED;

    /* Arm SNTP now that the state has actually settled into CONNECTED
     * (the assignment immediately above, not merely the GOT_IP event this
     * function is handling) -- see time_sync.h's header comment for why
     * this is fully asynchronous and safe to call on owner_task(). SNTP
     * only needs outbound UDP, so this is deliberately NOT gated behind the
     * static-IP-confirmed check below: that check exists to protect the
     * fallback AP teardown decision, which SNTP has nothing to do with. */
    time_sync_notify_got_ip();

    /* Capture RSSI of the connected network */
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_wifi.sta_rssi = ap_info.rssi;
    }

    /* Review fix (2026-09-21, finding 4): refresh the cheap-read STA ip/
     * netmask cache right here on the GOT_IP event, on owner_task(), where
     * an esp_netif_get_ip_info() call is already cheap and expected -- this
     * is the "refresh on the GOT_IP event path" wifi_prov_get_cached_sta_ip_
     * netmask()'s header comment promises, so a login backoff classification
     * moments after a fresh join sees the new lease without any caller
     * having to block on the owner task for it. */
    if (s_wifi.sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_wifi.sta_netif, &ip_info) == ESP_OK) {
            char ip_str[16];
            char mask_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, (uint32_t)sizeof(ip_str));
            esp_ip4addr_ntoa(&ip_info.netmask, mask_str, (uint32_t)sizeof(mask_str));
            wifi_prov_update_sta_ip_cache(ip_str, mask_str);
        }
    }

    /* 2026-08-21 fix, TODO.md section 1: GOT_IP under a STATIC config proves
     * only L2 association, not that the configured gateway/subnet are
     * actually correct (see apply_sta_config()'s comment). Do NOT drop the
     * fallback AP on that alone -- stay APSTA until
     * wifi_prov_note_possible_static_reachability() posts
     * CMD_CONFIRM_STATIC_REACHABLE, proving a request actually reached this
     * board at the static address. Until then the AP is the ONLY thing
     * standing between a bad static config and a stranded operator, so it
     * must not be torn down here. */
    if (s_wifi.ip_mode == WIFI_PROV_IP_MODE_STATIC && !s_wifi.static_ip_confirmed) {
        ESP_LOGW(WIFI_PROV_TAG,
                 "static IP join reached L2 (GOT_IP) but is NOT YET CONFIRMED reachable -- "
                 "keeping the fallback AP up until a request arrives via %s",
                 s_wifi.static_ip);
        return;
    }

    /* 2026-09-28 owner request: never cut a logged-in operator off by
     * dropping the AP out from under them the instant home Wi-Fi comes back.
     * Deferred here means do_rescan_tick()'s existing 30s cadence retries
     * the teardown -- no new timer, no new task. */
    if (ap_teardown_should_defer()) {
        s_wifi.ap_pending_teardown = true;
        ESP_LOGI(WIFI_PROV_TAG,
                 "station joined but a user is logged in (or a client is on the AP) -- "
                 "keeping fallback AP up until they're gone");
        return;
    }
    s_wifi.ap_pending_teardown = false;
    s_wifi.ap_fallback_active = false;

    ESP_LOGI(WIFI_PROV_TAG, "station joined, dropping fallback AP");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
    }
}

/* Runs on owner_task(), posted for by wifi_prov_note_possible_static_
 * reachability() below. Idempotent: a second confirmation (e.g. two page
 * loads racing) is a harmless no-op once static_ip_confirmed is already
 * true. */
void do_confirm_static_reachable(void)
{
    if (s_wifi.ip_mode != WIFI_PROV_IP_MODE_STATIC || s_wifi.static_ip_confirmed) {
        return;
    }
    s_wifi.static_ip_confirmed = true;
    ESP_LOGI(WIFI_PROV_TAG, "static IP %s confirmed reachable by an incoming HTTP request -- dropping fallback AP",
             s_wifi.static_ip);
    if (s_wifi.state != WIFI_PROV_STATE_CONNECTED) {
        return; /* nothing to tear down -- state changed again since GOT_IP */
    }
    /* Same logged-in-user defer as do_ev_got_ip() above -- a static-IP join
     * reaching this function has already proven L3 reachability, but that is
     * an orthogonal condition to "is anyone logged in right now", and both
     * must hold before the AP actually comes down. */
    if (ap_teardown_should_defer()) {
        s_wifi.ap_pending_teardown = true;
        ESP_LOGI(WIFI_PROV_TAG,
                 "static IP confirmed reachable but a user is logged in (or a client is on the AP) -- "
                 "keeping fallback AP up until they're gone");
        return;
    }
    s_wifi.ap_pending_teardown = false;
    s_wifi.ap_fallback_active = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
    }
}

/* Public entry point for confirming a STATIC join is actually reachable --
 * called from wifi_provision_http.c's handlers with the fd of the socket
 * that just served a request. Deliberately does the getsockname()/string
 * compare HERE, on the caller's (http worker) thread, rather than posting
 * the raw fd across to owner_task(): the socket is only valid for the
 * duration of this one request, and by the time owner_task() got around to
 * it, httpd could have already closed or reused it. Reading s_wifi.ip_mode/
 * static_ip/static_ip_confirmed without the queue is the same convention
 * every other getter in this file already uses (e.g.
 * wifi_prov_get_static_ip()) -- s_wifi's "one writer" rule is about who
 * MUTATES it, not who may read a snapshot of a string field. Only the
 * actual mutation (do_confirm_static_reachable()) is funneled through
 * owner_task(). */
/* True when this HTTP request arrived on the board's own SoftAP interface
 * rather than over the home network. Used to decide whether it is safe to
 * echo the AP password back (wifi_provision_http.c): a client already
 * associated to the AP necessarily knows that password, so showing it there
 * reveals nothing, while the same JSON served over the STA interface hands
 * it to every device on the house LAN.
 *
 * Compares the socket's LOCAL address, not the peer's -- the peer address
 * is whatever the client happens to have, whereas the local address is
 * which of the board's own interfaces accepted the connection, which is the
 * actual question. 192.168.4.1 is hardcoded for the same reason the
 * captive-portal DNS task hardcodes it, twenty lines further down: nothing
 * in this file ever calls esp_netif_set_ip_info(), so the SoftAP's address
 * is always esp_netif_create_default_wifi_ap()'s well-known default.
 *
 * Fails CLOSED -- a socket that cannot be inspected is reported as "not the
 * AP", so an unexpected error hides the password rather than leaking it. */
bool wifi_prov_request_arrived_on_ap(int sockfd)
{
    if (sockfd < 0) {
        return false;
    }
    struct sockaddr_in local_addr = { 0 };
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len) != 0) {
        return false;
    }
    if (local_addr.sin_family != AF_INET) {
        return false;
    }
    return local_addr.sin_addr.s_addr == htonl(0xC0A80401u); /* 192.168.4.1 */
}

void wifi_prov_note_possible_static_reachability(int sockfd)
{
    if (sockfd < 0) {
        return;
    }
    if (s_wifi.ip_mode != WIFI_PROV_IP_MODE_STATIC || s_wifi.static_ip_confirmed) {
        return; /* nothing to confirm -- DHCP mode, or already confirmed */
    }
    struct sockaddr_in local_addr = { 0 };
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len) != 0) {
        return;
    }
    char ip_str[16];
    if (!inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, sizeof(ip_str))) {
        return;
    }
    if (strcmp(ip_str, s_wifi.static_ip) != 0) {
        /* This request landed on some OTHER local address -- almost always
         * the fallback AP's own IP, which is still up precisely because
         * reachability isn't confirmed yet. That is not proof of anything
         * and must not be treated as confirmation. */
        return;
    }
    post_event(CMD_CONFIRM_STATIC_REACHABLE);
}

void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    post_event(CMD_EV_GOT_IP);
}

/* ---- Captive-portal DNS hijack -------------------------------------------
 *
 * 2026-08-19, explicit user report: phones joining the fallback AP
 * associate at the radio layer (device log shows a clean "station ... join,
 * AID=1") but then disassociate themselves ~30-40s later (802.11 reason 8,
 * station-initiated) in a repeating join/leave loop -- and never reach the
 * board's setup page. This matches WIFI_PROVISIONING.md's own "What's still
 * open" list, which already named the root cause: "No captive portal. A
 * phone joining the fallback AP has to browse to the board's address;
 * nothing redirects it there." Without a captive-portal redirect, a modern
 * phone OS's own connectivity check (an HTTP GET to a well-known probe URL
 * over DNS names this board's DHCP-assigned "no upstream router" AP can
 * never resolve to anything real) times out, the OS concludes "no internet"
 * and gives up on the network entirely -- exactly the observed loop. A real
 * DNS responder (any name resolves to this board's own AP IP) plus an HTTP
 * redirect for anything not already a known route turns that probe into a
 * hit, which is what makes every phone OS auto-pop its "sign in to network"
 * browser instead of dropping the connection.
 *
 * Bound to 0.0.0.0:53 rather than the AP interface specifically, and
 * started unconditionally alongside the netifs below (same "always-on,
 * negligible cost, no per-mode start/stop wiring needed" reasoning
 * start_ap_fallback_timer()'s own rescan_timer_cb() already uses for this
 * file): in STA-only operation nothing ever sends this board a DNS query
 * (it isn't anyone's configured resolver), so the task just blocks forever
 * on recvfrom() and costs nothing. It only ever answers queries that
 * actually arrive, which in practice only happens while the AP is up. */
static void dns_hijack_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(WIFI_PROV_TAG, "dns_hijack: socket() failed (errno %d) -- captive-portal redirect disabled", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(WIFI_PROV_TAG, "dns_hijack: bind(:53) failed (errno %d) -- captive-portal redirect disabled", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* Fixed at the softAP's well-known default (esp_netif_create_default_wifi_ap()
     * always assigns 192.168.4.1 and nothing in this file overrides it -- no
     * esp_netif_set_ip_info() call exists anywhere in wifi_prov.c) rather than
     * queried live from s_wifi.ap_netif, since this task starts before
     * esp_wifi_start() even runs and must not depend on AP-up ordering. */
    const uint32_t ap_ip = htonl(0xC0A80401u); /* 192.168.4.1 */

    uint8_t buf[512];
    while (true) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr, &from_len);
        if (len < 12) {
            continue; /* shorter than a DNS header -- not a real query */
        }

        /* Only handles the single-question case every real stub resolver
         * sends (multi-question DNS queries are vanishingly rare and not
         * worth parsing for a hijack responder that only needs to satisfy
         * "does this name resolve to *something*"). QDCOUNT lives at bytes
         * 4-5; bail rather than build a wrong answer if it's not exactly 1. */
        uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
        if (qdcount != 1) {
            continue;
        }

        /* Walk the QNAME (length-prefixed labels, terminated by a 0 byte) to
         * find where it ends, rather than assuming a fixed offset -- domain
         * names are variable length and this is the only way to find QTYPE/
         * QCLASS (4 bytes right after) or where to start appending the
         * answer record. */
        int qname_start = 12;
        int i = qname_start;
        while (i < len && buf[i] != 0) {
            i += buf[i] + 1;
            if (i >= len) break; /* malformed -- length ran off the packet */
        }
        if (i >= len || i + 5 > len) {
            continue; /* malformed query, nothing safe to reply to */
        }
        int qname_end = i + 1; /* one past the terminating 0 byte */
        int question_end = qname_end + 4; /* + QTYPE(2) + QCLASS(2) */

        /* Reply buffer: header + question (echoed verbatim) + one A answer
         * record. Answer uses name-compression pointer 0xC00C (back to the
         * question's QNAME at offset 12) instead of repeating the name. */
        uint8_t reply[512];
        if (question_end > (int)sizeof(reply) - 16) {
            continue; /* question too long to fit a reply in this buffer -- drop it */
        }
        memcpy(reply, buf, (size_t)question_end);

        reply[2] = 0x81; /* QR=1 (response), OPCODE=0, AA=1, TC=0, RD=echoed below */
        reply[3] = 0x80; /* RA=1, Z=0, RCODE=0 (no error) */
        reply[2] |= (buf[2] & 0x01); /* echo RD */
        reply[6] = 0x00; reply[7] = 0x01; /* ANCOUNT = 1 */
        reply[8] = 0x00; reply[9] = 0x00; /* NSCOUNT = 0 */
        reply[10] = 0x00; reply[11] = 0x00; /* ARCOUNT = 0 */

        int p = question_end;
        reply[p++] = 0xC0; reply[p++] = 0x0C; /* name = pointer to offset 12 */
        reply[p++] = 0x00; reply[p++] = 0x01; /* TYPE = A */
        reply[p++] = 0x00; reply[p++] = 0x01; /* CLASS = IN */
        reply[p++] = 0x00; reply[p++] = 0x00; reply[p++] = 0x00; reply[p++] = 0x3C; /* TTL = 60s */
        reply[p++] = 0x00; reply[p++] = 0x04; /* RDLENGTH = 4 */
        memcpy(&reply[p], &ap_ip, 4);
        p += 4;

        sendto(sock, reply, (size_t)p, 0, (struct sockaddr *)&from_addr, from_len);
    }
}

void start_dns_hijack_task(void)
{
    /* 2026-08-22: PSRAM stack. dns_hijack_task only does a UDP
     * recvfrom/sendto loop answering captive-portal DNS queries -- no
     * NVS/flash access, no direct SPI/I2C/UART hardware ownership. Unlike
     * wifi_prov's own owner_task (elsewhere in this split), this one never
     * calls esp_wifi_set_config()/nvs_save_*(). */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(dns_hijack_task, "dns_hijack", 3072, NULL, 4, NULL,
                                                         tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(WIFI_PROV_TAG, "xTaskCreatePinnedToCoreWithCaps(dns_hijack) failed -- no captive-portal DNS redirect");
    }
}
