/* wifi_prov.c split (2026-09-04, ROADMAP.md M15 A3) -- the do_*() command
 * bodies and public wifi_prov_*() producers for network list management,
 * mode, AP identity, IP mode/static-IP, station IP/RSSI/AP-client-count
 * getters, and the blocking scan. See wifi_prov_internal.h's top-of-file
 * comment for the full split rationale and file map. Move-only: no logic,
 * ordering, naming or visibility change beyond widening former `static`
 * symbols this split's sibling files now call directly. */

#include "wifi_prov_internal.h"

#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- s_saved_nets_cache below */
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h" /* portMUX_TYPE -- s_saved_nets_cache_mux below */

/* Scan results staging. Written ONLY by owner_task() (via do_scan() below),
 * copied out by the producer after its semaphore is given. Deliberately
 * module-static rather than a member of wifi_result_t (which lives on the
 * producer's stack): at 20 entries this is ~720 bytes, and several callers
 * run on 4096-byte worker task stacks. Nothing is lost by staging here --
 * only one command is ever being serviced at a time, and a producer that
 * timed out simply never copies out, leaving the next one to overwrite it.
 * Declared extern in wifi_prov_internal.h: wifi_prov.c's owner_task() also
 * passes this straight to do_scan() for the CMD_SCAN case. */
wifi_prov_scan_result_t s_scan_stage[WIFI_OWNER_SCAN_STAGE_MAX];

/* How long a scan's producer waits. A full active scan of every 2.4GHz
 * channel at 50-150ms dwell is seconds, not milliseconds, and the existing
 * callers already tolerate exactly that: ui_page_network.c runs it on a
 * dedicated short-lived worker task (never lvgl_port_task), and
 * wifi_provision_http.c/uart_bridge_ext.c block their own task on it today.
 * 15s is "a scan, plus one already-in-flight scan ahead of it in the queue,
 * plus slack" -- not a radio-derived number. */
#define WIFI_OWNER_SCAN_WAIT_MS 15000

/* How long every other producer waits. Sized by the WORST CASE AHEAD OF IT
 * in the queue, which is a scan, not by its own (millisecond-scale) work --
 * a 200ms timeout of the kind thermo_owner.c uses would spuriously fail any
 * status poll unlucky enough to land behind an operator tapping Scan. A
 * producer that does time out fails closed: it returns an error, exactly as
 * if the operation itself had failed. */
#define WIFI_OWNER_WAIT_MS 12000

/* Runs on owner_task(). Both strings are NUL-terminated and already
 * length-validated by the producer -- validation stays on the CALLER's side
 * of the queue throughout this file, so a malformed request is refused
 * immediately and never costs a queue slot or a task hop. */
esp_err_t do_add_network(const char *new_ssid, const char *password, bool *out_join_after_reply)
{
    *out_join_after_reply = false;
    size_t password_len = strlen(password);

    /* Upsert by exact SSID match -- update the password in place if this
     * SSID is already saved, otherwise append a new entry. */
    int idx = -1;
    for (uint8_t i = 0; i < s_wifi.saved_nets.count; i++) {
        if (strcmp(s_wifi.saved_nets.nets[i].ssid, new_ssid) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (s_wifi.saved_nets.count >= WIFI_PROV_MAX_SAVED_NETWORKS) {
            /* List is full and this is a genuinely new SSID -- refuse
             * without touching anything already saved, rather than silently
             * evicting an existing entry the operator didn't ask to remove. */
            return ESP_ERR_NO_MEM;
        }
        idx = s_wifi.saved_nets.count;
        s_wifi.saved_nets.count++;
        strncpy(s_wifi.saved_nets.nets[idx].ssid, new_ssid, sizeof(s_wifi.saved_nets.nets[idx].ssid) - 1);
        s_wifi.saved_nets.nets[idx].ssid[sizeof(s_wifi.saved_nets.nets[idx].ssid) - 1] = '\0';
    }
    memcpy(s_wifi.saved_nets.nets[idx].password, password, password_len);
    s_wifi.saved_nets.nets[idx].password[password_len] = '\0';
    wifi_prov_update_saved_nets_cache();

    esp_err_t err = nvs_save_saved_nets();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_saved_nets failed: %s -- this network will not survive a reboot",
                 esp_err_to_name(err));
        /* Still attempt the join below -- the operator asked for this
         * network right now, whether or not it persists. */
    }

    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        /* Submitting a network is an explicit choice to join something -- it
         * supersedes a previously-set AP-mode preference the same way
         * picking a network in the Network settings page (TODO.md section 4)
         * would. Without this, AP mode had no way back out through the HTTP
         * API: the provisioning page's mode toggle set it, but nothing ever
         * cleared it again from this path. */
        ESP_LOGI(WIFI_PROV_TAG, "network submitted while in AP mode -- switching to home mode");
        s_wifi.mode = WIFI_PROV_MODE_HOME;
        esp_err_t mode_err = nvs_save_mode();
        if (mode_err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "nvs_save_mode failed: %s -- choice will not survive a reboot",
                     esp_err_to_name(mode_err));
        }
    }

    /* W1: caller (owner_task()) runs start_sta_join() AFTER replying to the
     * waiting producer, not here -- see wifi_prov.c's reply-slot-pool
     * comment. start_sta_join() re-runs the scan-based tie-break, so this may
     * join a different (stronger, already-in-range) saved network than the
     * one just added -- that's intended, not a bug: adding a network is
     * "make this available", not "connect to this one specifically".
     *
     * 2026-09-25 review fix: set CONNECTING here, before returning, still on
     * owner_task() (the single writer of s_wifi) -- NOT after start_sta_join()
     * actually runs. Without this, the reply now reaches the HTTP client
     * before start_sta_join() does anything at all, so a client that polls
     * GET /api/status right after "ok" could still see the PRE-add state
     * (UNPROVISIONED or AP_MODE) and, per wifi_provision_page.html's poll()
     * logic, conclude nothing is in progress and stop polling -- exactly the
     * first-time provisioning-over-the-fallback-AP flow this slice's own
     * side benefit targets. Setting it here makes the state transition
     * visible atomically with the reply becoming visible, independent of
     * however long start_sta_join()'s real scan takes afterward. */
    s_wifi.state = WIFI_PROV_STATE_CONNECTING;
    *out_join_after_reply = true;
    return ESP_OK;
}

esp_err_t wifi_prov_add_network(const char *ssid, size_t ssid_len, const char *password,
                                 size_t password_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!password || password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_ADD_NETWORK };
    memcpy(cmd.args.add_network.ssid, ssid, ssid_len);
    cmd.args.add_network.ssid[ssid_len] = '\0';
    memcpy(cmd.args.add_network.password, password, password_len);
    cmd.args.add_network.password[password_len] = '\0';

    wifi_result_t r;
    /* W1: the owner now replies right after the upsert+NVS write and runs
     * start_sta_join() (the real blocking scan) afterward, so this producer
     * no longer needs to wait out the scan itself -- WIFI_OWNER_WAIT_MS
     * (ordinary command timeout) replaces WIFI_OWNER_SCAN_WAIT_MS here. Any
     * join failure past this point is still observable exactly as before:
     * wifi_prov_get_state() / the existing ESP_LOGW/E calls inside
     * start_sta_join()'s callees never depended on this call having waited
     * for them. */
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

esp_err_t wifi_prov_set_credentials(const char *ssid, size_t ssid_len, const char *password,
                                    size_t password_len)
{
    return wifi_prov_add_network(ssid, ssid_len, password, password_len);
}

/* Runs on owner_task(). */
esp_err_t do_forget_network(const char *target)
{
    int idx = -1;
    for (uint8_t i = 0; i < s_wifi.saved_nets.count; i++) {
        if (strcmp(s_wifi.saved_nets.nets[i].ssid, target) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return ESP_OK; /* nothing saved under this SSID -- not a failure */
    }

    /* Compact: shift everything after idx down by one. Deliberately no
     * "can't remove the last one" guard -- bringing count to 0 is a valid,
     * fully-supported state (falls back to AP-capable/unprovisioned
     * behavior via the count > 0 checks elsewhere in this file, the same way
     * has_creds == false used to). If this was the currently-active/
     * connected network, the existing disconnect/reconnect event handlers
     * discover on their own that there's nothing left to reconnect to --
     * this function doesn't force a disconnect itself. */
    for (uint8_t i = (uint8_t)idx; i + 1 < s_wifi.saved_nets.count; i++) {
        s_wifi.saved_nets.nets[i] = s_wifi.saved_nets.nets[i + 1];
    }
    s_wifi.saved_nets.count--;
    memset(&s_wifi.saved_nets.nets[s_wifi.saved_nets.count], 0, sizeof(s_wifi.saved_nets.nets[0]));
    wifi_prov_update_saved_nets_cache();

    esp_err_t err = nvs_save_saved_nets();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_saved_nets failed: %s -- removal will not survive a reboot",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t wifi_prov_forget_network(const char *ssid, size_t ssid_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_FORGET_NETWORK };
    memcpy(cmd.args.forget_network.ssid, ssid, ssid_len);
    cmd.args.forget_network.ssid[ssid_len] = '\0';

    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). Writes into the result slot rather than the caller's
 * array -- the whole saved list is at most 8 SSIDs (264 bytes), so copying it
 * twice is cheaper than the lifetime question a caller pointer would raise. */
esp_err_t do_get_saved_networks(size_t max_results, wifi_result_t *r)
{
    size_t n = s_wifi.saved_nets.count;
    if (n > max_results) {
        n = max_results;
    }
    if (n > WIFI_PROV_MAX_SAVED_NETWORKS) {
        n = WIFI_PROV_MAX_SAVED_NETWORKS; /* defensive; count can never exceed this */
    }
    for (size_t i = 0; i < n; i++) {
        strncpy(r->saved[i].ssid, s_wifi.saved_nets.nets[i].ssid, WIFI_PROV_SSID_MAX_LEN);
        r->saved[i].ssid[WIFI_PROV_SSID_MAX_LEN] = '\0';
        /* Password deliberately not copied out -- wifi_prov_saved_network_t
         * has no field for it, by design; see the .h doc comment. */
    }
    r->saved_count = n;
    return ESP_OK;
}

/* ---- non-blocking saved-networks cache (2026-09-25, LCD freeze fix) ------
 * ui_page_network_manage.c's refresh_cb() and forget_row_clicked_cb() used
 * to call wifi_prov_get_saved_networks() directly from lvgl_port_task, once
 * a second -- a queued call sized (WIFI_OWNER_WAIT_MS above, 12s) to wait
 * behind a worst-case scan already ahead of it in the owner's queue. While a
 * scan or connect was in flight the whole LCD froze for as long as that
 * queued wait took. This mirror is a short-spinlock-guarded copy of
 * s_wifi.saved_nets, small enough (WIFI_PROV_MAX_SAVED_NETWORKS *
 * sizeof(wifi_prov_saved_network_t), 264 B today) to copy under a spinlock
 * with no torn-read risk, refreshed by owner_task() itself (never a
 * producer) every time the real list changes, plus once at boot after the
 * initial nvs_load_saved_nets(). Same portMUX_TYPE-snapshot pattern as
 * firing_shadow.c's s_status/s_status_mux -- see that file's comment for the
 * torn-read rationale a plain struct-copy-without-a-lock would risk here
 * too. The array itself lives in PSRAM (EXT_RAM_BSS_ATTR): it's list-sized,
 * not touched with the cache disabled, and DRAM headroom on this board is
 * down to ~1.4 KB (W1's 2026-09-25 reply-slot-pool DRAM measurement). */
static portMUX_TYPE s_saved_nets_cache_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR wifi_prov_saved_network_t s_saved_nets_cache[WIFI_PROV_MAX_SAVED_NETWORKS];
static size_t s_saved_nets_cache_count;

/* Runs on owner_task() (do_add_network()/do_forget_network()) or, once, on
 * whatever task calls wifi_prov_start() before the owner task exists --
 * single-threaded at that point, so the spinlock there is defence in depth,
 * not load-bearing. */
void wifi_prov_update_saved_nets_cache(void)
{
    size_t n = s_wifi.saved_nets.count;
    if (n > WIFI_PROV_MAX_SAVED_NETWORKS) {
        n = WIFI_PROV_MAX_SAVED_NETWORKS; /* defensive; count can never exceed this */
    }

    /* Build the copy on the stack first -- keep the critical section to a
     * single memcpy + count store, never a strncpy loop, so the maximum hold
     * time is fixed and tiny regardless of how many entries are saved. */
    wifi_prov_saved_network_t tmp[WIFI_PROV_MAX_SAVED_NETWORKS];
    for (size_t i = 0; i < n; i++) {
        strncpy(tmp[i].ssid, s_wifi.saved_nets.nets[i].ssid, WIFI_PROV_SSID_MAX_LEN);
        tmp[i].ssid[WIFI_PROV_SSID_MAX_LEN] = '\0';
    }

    portENTER_CRITICAL(&s_saved_nets_cache_mux);
    memcpy(s_saved_nets_cache, tmp, n * sizeof(tmp[0]));
    s_saved_nets_cache_count = n;
    portEXIT_CRITICAL(&s_saved_nets_cache_mux);
}

void wifi_prov_get_saved_networks_cached(wifi_prov_saved_network_t *out, size_t max_results, size_t *out_count)
{
    if (!out_count) {
        return;
    }
    if (!out || max_results == 0) {
        *out_count = 0;
        return;
    }

    portENTER_CRITICAL(&s_saved_nets_cache_mux);
    size_t n = s_saved_nets_cache_count;
    if (n > max_results) {
        n = max_results;
    }
    memcpy(out, s_saved_nets_cache, n * sizeof(*out));
    portEXIT_CRITICAL(&s_saved_nets_cache_mux);

    *out_count = n;
}

esp_err_t wifi_prov_get_saved_networks(wifi_prov_saved_network_t *out, size_t max_results, size_t *out_count)
{
    if (!out || max_results == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_GET_SAVED_NETWORKS,
                       .args.get_saved_networks = { .max_results = max_results } };
    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK) {
        memcpy(out, r.saved, r.saved_count * sizeof(*out));
        *out_count = r.saved_count;
    }
    return r.err;
}

/* Runs on owner_task(). */
esp_err_t do_set_mode(wifi_prov_mode_t mode, bool *out_join_after_reply)
{
    *out_join_after_reply = false;
    s_wifi.mode = mode;
    esp_err_t err = nvs_save_mode();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_mode failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
    }

    if (mode == WIFI_PROV_MODE_AP) {
        cancel_ap_fallback_timer();
        s_wifi.ap_pending_teardown = false;
        s_wifi.ap_fallback_active = false;
        s_wifi.state = WIFI_PROV_STATE_AP_MODE;
        /* Mode first, then config -- switching here from home mode while
         * connected leaves the driver in WIFI_MODE_STA, which doesn't
         * include the AP interface yet; same ordering bug as
         * wifi_prov_start()/ap_fallback_timer_cb(). */
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (mode_err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(mode_err));
        } else {
            apply_ap_config();
        }
        ESP_LOGI(WIFI_PROV_TAG, "AP mode enabled: station never attempted");
    } else if (s_wifi.saved_nets.count > 0) {
        ESP_LOGI(WIFI_PROV_TAG, "home mode enabled, resuming join to saved network");
        /* W1: caller (owner_task()) runs start_sta_join() AFTER replying --
         * see wifi_prov.c's reply-slot-pool comment. 2026-09-25 review fix:
         * set CONNECTING here (same rationale as do_add_network() above) so
         * the state a status poll sees changes atomically with the reply,
         * not only once start_sta_join()'s real scan gets around to it. */
        s_wifi.state = WIFI_PROV_STATE_CONNECTING;
        *out_join_after_reply = true;
    } else {
        s_wifi.state = WIFI_PROV_STATE_UNPROVISIONED;
        ESP_LOGI(WIFI_PROV_TAG, "home mode enabled, no saved network to join");
    }
    return ESP_OK;
}

esp_err_t wifi_prov_set_mode(wifi_prov_mode_t mode)
{
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_cmd_t cmd = { .type = CMD_SET_MODE, .args.set_mode = { .mode = mode } };
    wifi_result_t r;
    /* W1: the HOME branch's start_sta_join() now runs on owner_task() AFTER
     * the reply, not before it -- ordinary wait applies here too. */
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). NUL-terminated, already length-validated. */
esp_err_t do_set_ap_ssid(const char *ssid)
{
    strncpy(s_wifi.ap_ssid, ssid, sizeof(s_wifi.ap_ssid) - 1);
    s_wifi.ap_ssid[sizeof(s_wifi.ap_ssid) - 1] = '\0';
    s_wifi.has_ap_ssid_override = true;

    esp_err_t err = nvs_save_ap_ssid();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_ap_ssid failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. */
    }

    apply_ap_config();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        /* Mirrors wifi_prov_set_ap_password()'s pattern: esp_wifi_set_config
         * alone doesn't kick already-associated clients off a running AP, so
         * re-applying the same mode forces the radio to pick up the new
         * SSID immediately instead of only after the next boot/mode change.
         * Station side (if APSTA) is untouched. */
        esp_err_t mode_err = esp_wifi_set_mode(mode);
        if (mode_err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
        }
    }
    ESP_LOGI(WIFI_PROV_TAG, "AP SSID changed to '%s'", s_wifi.ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_prov_set_ap_ssid(const char *ssid, size_t ssid_len)
{
    if (!ssid || ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX_LEN) {
        /* The AP always needs to be reachable by something -- an empty
         * SSID isn't a valid "leave it alone" no-op here the way it can be
         * for a station password (open network); refuse it outright. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SET_AP_SSID };
    memcpy(cmd.args.set_ap_ssid.ssid, ssid, ssid_len);
    cmd.args.set_ap_ssid.ssid[ssid_len] = '\0';

    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). NUL-terminated, already length-validated (including
 * the 1-7 char WPA2-PSK refusal, which stays in the producer). */
esp_err_t do_set_ap_password(const char *password)
{
    strncpy(s_wifi.ap_password, password, sizeof(s_wifi.ap_password) - 1);
    s_wifi.ap_password[sizeof(s_wifi.ap_password) - 1] = '\0';
    s_wifi.has_ap_password_override = true;
    size_t password_len = strlen(s_wifi.ap_password);

    esp_err_t err = nvs_save_ap_password();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_ap_password failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. */
    }

    apply_ap_config();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        /* Mirrors wifi_prov_set_ap_ssid()'s pattern: esp_wifi_set_config
         * alone doesn't kick already-associated clients off a running AP, so
         * re-applying the same mode forces the radio to pick up the new PSK
         * immediately instead of only after the next boot/mode change.
         * Station side (if APSTA) is untouched. */
        esp_err_t mode_err = esp_wifi_set_mode(mode);
        if (mode_err != ESP_OK) {
            ESP_LOGE(WIFI_PROV_TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
        }
    }
    ESP_LOGI(WIFI_PROV_TAG, "AP password changed (%s)", password_len > 0 ? "WPA2-PSK" : "open");
    return ESP_OK;
}

esp_err_t wifi_prov_set_ap_password(const char *password, size_t password_len)
{
    if (!password || password_len > WIFI_PROV_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (password_len > 0 && password_len < 8) {
        /* WPA2-PSK requires 8-63 chars; a non-empty password shorter than
         * that can never work on real hardware, so refuse it outright here
         * rather than silently falling back to an open AP the way a bad
         * compile-time Kconfig default does in apply_ap_config() (there is
         * no request to fail in that case, just a build to warn about). */
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SET_AP_PASSWORD };
    memcpy(cmd.args.set_ap_password.password, password, password_len);
    cmd.args.set_ap_password.password[password_len] = '\0';

    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

wifi_prov_ip_mode_t wifi_prov_get_ip_mode(void)
{
    /* Direct read -- one aligned enum word, same reasoning as
     * wifi_prov_get_mode(). */
    return s_wifi.ip_mode;
}

const char *wifi_prov_get_static_ip(void)
{
    return s_wifi.static_ip;
}

const char *wifi_prov_get_static_netmask(void)
{
    return s_wifi.static_netmask;
}

const char *wifi_prov_get_static_gateway(void)
{
    return s_wifi.static_gateway;
}

/* Common tail for do_set_dhcp()/do_set_static_ip(): if a station join is
 * currently active or in flight, force it to pick up the new netif config
 * right away rather than leaving a stale IP applied until the next natural
 * disconnect/reconnect. apply_sta_config() (already called by the caller
 * before this) has updated the netif's DHCP-client/static state; a
 * disconnect+reconnect is what makes the STA driver actually re-run through
 * that netif state for an already-associated link. Mirrors
 * wifi_prov_set_ap_ssid()'s/wifi_prov_set_ap_password()'s "re-apply to force
 * pickup" pattern on the AP side. No-op (and safe) if nothing is
 * connected/connecting -- do_ev_sta_disconnected() then simply has nothing
 * to retry. */
static void reapply_sta_if_active(void)
{
    if (s_wifi.mode != WIFI_PROV_MODE_HOME) {
        return; /* AP mode: no station activity to reapply */
    }
    if (s_wifi.state == WIFI_PROV_STATE_CONNECTED || s_wifi.state == WIFI_PROV_STATE_CONNECTING ||
        s_wifi.state == WIFI_PROV_STATE_RECONNECTING) {
        ESP_LOGI(WIFI_PROV_TAG, "IP config changed while a station join is active -- reconnecting to apply it");
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
}

/* Runs on owner_task(). */
esp_err_t do_set_dhcp(void)
{
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_DHCP;
    s_wifi.static_ip[0] = '\0';
    s_wifi.static_netmask[0] = '\0';
    s_wifi.static_gateway[0] = '\0';
    s_wifi.static_ip_confirmed = false; /* irrelevant in DHCP mode, reset for hygiene */

    esp_err_t err = nvs_save_ip_config();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_ip_config failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
    }

    apply_sta_config();
    reapply_sta_if_active();
    ESP_LOGI(WIFI_PROV_TAG, "STA IP mode set to DHCP");
    return ESP_OK;
}

esp_err_t wifi_prov_set_dhcp(void)
{
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_cmd_t cmd = { .type = CMD_SET_DHCP };
    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

/* Runs on owner_task(). Strings are already NUL-terminated, but NOT yet
 * validated as dotted-quad IPv4 -- that happens in the producer
 * (wifi_prov_set_static_ip() below) before this is ever posted, same
 * "validation stays on the caller's side of the queue" convention as
 * do_add_network(). */
esp_err_t do_set_static_ip(const char *ip, const char *netmask, const char *gateway)
{
    strncpy(s_wifi.static_ip, ip, sizeof(s_wifi.static_ip) - 1);
    s_wifi.static_ip[sizeof(s_wifi.static_ip) - 1] = '\0';
    strncpy(s_wifi.static_netmask, netmask, sizeof(s_wifi.static_netmask) - 1);
    s_wifi.static_netmask[sizeof(s_wifi.static_netmask) - 1] = '\0';
    strncpy(s_wifi.static_gateway, gateway, sizeof(s_wifi.static_gateway) - 1);
    s_wifi.static_gateway[sizeof(s_wifi.static_gateway) - 1] = '\0';
    s_wifi.ip_mode = WIFI_PROV_IP_MODE_STATIC;
    /* A new (or re-typed) static config is unproven until something proves
     * it -- never carry a previous confirmation over to different numbers.
     * See s_wifi.static_ip_confirmed's comment and do_ev_got_ip(). */
    s_wifi.static_ip_confirmed = false;

    esp_err_t err = nvs_save_ip_config();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_PROV_TAG, "nvs_save_ip_config failed: %s -- choice will not survive a reboot",
                 esp_err_to_name(err));
        /* Still apply it live below -- the operator asked for this right
         * now, whether or not it persists past a reboot. Same convention as
         * every other setter in this file. */
    }

    apply_sta_config();
    reapply_sta_if_active();
    ESP_LOGI(WIFI_PROV_TAG, "STA IP mode set to STATIC (%s/%s via %s)", s_wifi.static_ip, s_wifi.static_netmask,
             s_wifi.static_gateway);
    return ESP_OK;
}

esp_err_t wifi_prov_set_static_ip(const char *ip, const char *netmask, const char *gateway)
{
    if (!ip || !netmask || !gateway) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Validated here, on the CALLER's side of the queue -- a malformed
     * request never costs a queue slot or a task hop, same rule as every
     * other producer in this file. esp_ip4_addr_t values themselves are
     * discarded; this call only needs to know whether each string parses
     * (wifi_prov_ip_in_ap_subnet() below does its own parse of `ip` for the
     * AP-subnet check). */
    esp_ip4_addr_t tmp;
    if (!parse_ipv4(ip, &tmp) || !parse_ipv4(netmask, &tmp) || !parse_ipv4(gateway, &tmp)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Review fix (2026-09-29): reject a static IP inside the fallback AP's
     * own 192.168.4.0/24 subnet. Now that
     * wifi_prov_note_possible_static_reachability() actually works (see
     * wifi_prov_link.c), an AP client could otherwise request e.g. 192.168.4.1
     * as its "static" IP, get it applied to the STA interface, and have that
     * request itself -- served while still associated to the AP -- reported
     * as reachability confirmation, tearing the AP down out from under
     * itself/every other AP client. Shares one definition with the HTTP
     * handler's distinct 400 message and with
     * wifi_prov_note_possible_static_reachability()'s own guard -- see
     * wifi_prov_ip_in_ap_subnet()'s comment in wifi_prov_link.c. */
    if (wifi_prov_ip_in_ap_subnet(ip)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ip) >= WIFI_PROV_IPV4_STR_MAX || strlen(netmask) >= WIFI_PROV_IPV4_STR_MAX ||
        strlen(gateway) >= WIFI_PROV_IPV4_STR_MAX) {
        return ESP_ERR_INVALID_ARG; /* can't happen if parse_ipv4 succeeded, defensive only */
    }
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SET_STATIC_IP };
    strncpy(cmd.args.set_static_ip.ip, ip, sizeof(cmd.args.set_static_ip.ip) - 1);
    strncpy(cmd.args.set_static_ip.netmask, netmask, sizeof(cmd.args.set_static_ip.netmask) - 1);
    strncpy(cmd.args.set_static_ip.gateway, gateway, sizeof(cmd.args.set_static_ip.gateway) - 1);

    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    return r.err;
}

bool wifi_prov_is_sta_connected(void)
{
    /* Direct read, deliberately -- see wifi_prov.h's "readers that stay
     * direct" note. One aligned enum word, single-instruction load. */
    return s_wifi.state == WIFI_PROV_STATE_CONNECTED;
}

/* 2026-09-28: true while station is CONNECTED but the fallback AP teardown
 * is deliberately deferred (a user is logged in, or a client is on the AP
 * with auth off) -- see s_wifi.ap_pending_teardown's own doc comment and
 * ap_teardown_should_defer() in wifi_prov_link.c. Same "direct read of one
 * bool" convention as wifi_prov_is_sta_connected() just above -- this never
 * blocks and is safe from lvgl_port_task or an HTTP worker. */
bool wifi_prov_get_ap_pending_teardown(void)
{
    return s_wifi.ap_pending_teardown;
}

/* Cheap-read cache for the STA ip/netmask, consumed by
 * wifi_prov_get_cached_sta_ip_netmask() below. Review fix (2026-09-21,
 * finding 4 on the login backoff commit): wifi_prov_get_sta_ip_netmask()
 * round-trips through the owner-task queue and can block the CALLER for up
 * to WIFI_OWNER_WAIT_MS (12s) -- fine for a UI action, fatal for a call made
 * while web_auth_login_http.c holds its own module lock, since that stalls
 * every OTHER concurrent login attempt (including the 429-refusal path,
 * which must never touch the owner task at all) behind this one's wait.
 * s_sta_ip_cache_mux is a plain spinlock rather than a semaphore: every
 * critical section here is a fixed-size strncpy of two 16-byte buffers, so
 * there is no wait worth naming and no risk of it itself becoming a stall
 * point. Updated from owner_task() only (wifi_prov_update_sta_ip_cache()),
 * read from any task. Starts zeroed (empty strings), which is the same
 * "no lease yet" fallback wifi_prov_get_sta_ip_netmask() already returns
 * for AP-only/not-yet-associated boards -- so an unpopulated cache is a
 * correct, cheap answer, not a special case. */
static char s_cached_sta_ip[16];
static char s_cached_sta_netmask[16];
static portMUX_TYPE s_sta_ip_cache_mux = portMUX_INITIALIZER_UNLOCKED;

/* Runs on owner_task() only -- called from do_get_sta_ip() below (every
 * direct query keeps the cache fresh too) and from do_ev_got_ip()
 * (wifi_prov_link.c), which is the actual "refresh on the GOT_IP event"
 * path: a fresh lease is cached the moment the station associates, with no
 * caller ever having to wait for it. */
void wifi_prov_update_sta_ip_cache(const char *ip, const char *netmask)
{
    portENTER_CRITICAL(&s_sta_ip_cache_mux);
    strncpy(s_cached_sta_ip, ip ? ip : "", sizeof(s_cached_sta_ip) - 1);
    s_cached_sta_ip[sizeof(s_cached_sta_ip) - 1] = '\0';
    strncpy(s_cached_sta_netmask, netmask ? netmask : "", sizeof(s_cached_sta_netmask) - 1);
    s_cached_sta_netmask[sizeof(s_cached_sta_netmask) - 1] = '\0';
    portEXIT_CRITICAL(&s_sta_ip_cache_mux);
}

/* Public: see wifi_prov.h. Never blocks and never touches the owner-task
 * queue -- a plain spinlocked memcpy of whatever wifi_prov_update_sta_ip_
 * cache() last wrote, which may be empty (no lease yet/lost) or briefly
 * stale (up to one GOT_IP-to-DISCONNECT cycle behind). That staleness is
 * the accepted tradeoff for finding 4: a login backoff classification that
 * is a few seconds out of date on a lease change is harmless (worst case,
 * one request is classified by the previous subnet for one beat), while
 * blocking a login response on the owner task is not. */
esp_err_t wifi_prov_get_cached_sta_ip_netmask(char *ip_out, size_t ip_cap, char *netmask_out, size_t netmask_cap)
{
    if (!ip_out || ip_cap < 1 || !netmask_out || netmask_cap < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_sta_ip_cache_mux);
    strncpy(ip_out, s_cached_sta_ip, ip_cap - 1);
    ip_out[ip_cap - 1] = '\0';
    strncpy(netmask_out, s_cached_sta_netmask, netmask_cap - 1);
    netmask_out[netmask_cap - 1] = '\0';
    portEXIT_CRITICAL(&s_sta_ip_cache_mux);
    return ESP_OK;
}

/* Runs on owner_task(). Writes the dotted-quad into the result slot; the
 * producer copies it out. */
esp_err_t do_get_sta_ip(size_t out_cap, wifi_result_t *r)
{
    /* 2026-08-20: on-query reconciliation. This getter already runs on
     * owner_task() precisely because it is compound state, which makes it a
     * legal place to repair that state (a caller-thread read is not). If the
     * radio says we're associated with a live IP, fix the enum here rather
     * than reporting "no IP" for up to a rescan interval while the board is
     * demonstrably reachable -- the failure documented at
     * reconcile_sta_state(). */
    (void)reconcile_sta_state();
    if (s_wifi.state != WIFI_PROV_STATE_CONNECTED || !s_wifi.sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_wifi.sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    if (out_cap < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_ip4addr_ntoa(&ip_info.ip, r->sta_ip, (uint32_t)sizeof(r->sta_ip));
    esp_ip4addr_ntoa(&ip_info.netmask, r->sta_netmask, (uint32_t)sizeof(r->sta_netmask));
    wifi_prov_update_sta_ip_cache(r->sta_ip, r->sta_netmask);
    return ESP_OK;
}

esp_err_t wifi_prov_get_sta_ip_netmask(char *ip_out, size_t ip_cap, char *netmask_out, size_t netmask_cap)
{
    if (!ip_out || ip_cap < 1 || !netmask_out || netmask_cap < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    ip_out[0] = '\0';
    netmask_out[0] = '\0';
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Same compound-state round trip as wifi_prov_get_sta_ip() -- reuses
     * CMD_GET_STA_IP rather than adding a new command type, since
     * do_get_sta_ip() now fills both r->sta_ip and r->sta_netmask from the
     * same single esp_netif_get_ip_info() call. */
    wifi_cmd_t cmd = { .type = CMD_GET_STA_IP, .args.get_sta_ip = { .out_cap = ip_cap } };
    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK) {
        strncpy(ip_out, r.sta_ip, ip_cap - 1);
        ip_out[ip_cap - 1] = '\0';
        strncpy(netmask_out, r.sta_netmask, netmask_cap - 1);
        netmask_out[netmask_cap - 1] = '\0';
    }
    return r.err;
}

esp_err_t wifi_prov_get_sta_ip(char *out, size_t out_cap)
{
    if (!out || out_cap < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Compound state (the state enum AND the netif handle AND a driver call
     * that reads the netif's current lease), so this one goes through the
     * queue even though it is nominally a getter -- the split rule stated in
     * wifi_prov.h. */
    wifi_cmd_t cmd = { .type = CMD_GET_STA_IP, .args.get_sta_ip = { .out_cap = out_cap } };
    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK) {
        strncpy(out, r.sta_ip, out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return r.err;
}

/* Runs on owner_task() -- either drained from a CMD_SCAN posted by
 * wifi_prov_scan() below, or called straight from
 * select_and_apply_join_candidate()'s auto-join tie-break (wifi_prov_link.c),
 * which is already on this task. This is the blocking part:
 * esp_wifi_scan_start(block=true). The `started` check lives in the
 * producer; the AP-mode refusal stays here so the internal caller gets it
 * too. */
esp_err_t do_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count)
{
    *out_count = 0;
    if (s_wifi.mode == WIFI_PROV_MODE_AP) {
        /* AP mode means no station-radio activity at all, and a scan --
         * even though it never joins anything -- still means bringing the
         * STA interface up. Refuse rather than bend that guarantee for a
         * settings-page convenience. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return err;
    }
    if (mode == WIFI_MODE_AP) {
        /* Unprovisioned: only the AP interface is up. Bring STA up too so a
         * scan is possible -- this does not by itself connect to anything;
         * on_wifi_event() only calls esp_wifi_connect() when has_creds is
         * true, and unprovisioned means it isn't. */
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 150,
    };
    err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }
    if (found > max_results) {
        found = (uint16_t)max_results;
    }

    static wifi_ap_record_t records[20];
    uint16_t to_fetch = found > (sizeof(records) / sizeof(records[0]))
                            ? (uint16_t)(sizeof(records) / sizeof(records[0]))
                            : found;
    err = esp_wifi_scan_get_ap_records(&to_fetch, records);
    if (err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t n = to_fetch < max_results ? to_fetch : max_results;
    for (size_t i = 0; i < n; i++) {
        strncpy(results[i].ssid, (const char *)records[i].ssid, WIFI_PROV_SSID_MAX_LEN);
        results[i].ssid[WIFI_PROV_SSID_MAX_LEN] = '\0';
        results[i].rssi = records[i].rssi;
        results[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t wifi_prov_scan(wifi_prov_scan_result_t *results, size_t max_results, size_t *out_count)
{
    if (!results || max_results == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cmd_t cmd = { .type = CMD_SCAN, .args.scan = { .max_results = max_results } };
    wifi_result_t r;
    if (!wifi_prov_post_and_wait(&cmd, &r, WIFI_OWNER_SCAN_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (r.err == ESP_OK && r.scan_count > 0) {
        size_t n = r.scan_count > max_results ? max_results : r.scan_count;
        memcpy(results, s_scan_stage, n * sizeof(*results));
        *out_count = n;
    }
    return r.err;
}

int8_t wifi_prov_get_sta_rssi(void)
{
    /* Direct read, same reasoning as wifi_prov_is_sta_connected(): one bool,
     * one enum word, one int8 -- see wifi_prov.h. */
    if (!s_wifi.started || s_wifi.state != WIFI_PROV_STATE_CONNECTED) {
        return -127; /* not connected */
    }
    return s_wifi.sta_rssi;
}

uint8_t wifi_prov_get_ap_client_count(void)
{
    if (!s_wifi.started) {
        return 0;
    }

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return 0;
    }
    if ((mode != WIFI_MODE_AP) && (mode != WIFI_MODE_APSTA)) {
        return 0; /* AP is not active */
    }

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 0;
    }
    return (uint8_t)(sta_list.num > 255 ? 255 : sta_list.num); /* cap at uint8 */
}
