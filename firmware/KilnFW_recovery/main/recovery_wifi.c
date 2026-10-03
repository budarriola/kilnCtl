// recovery_wifi.c -- see recovery_wifi.h.
#include "recovery_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "recovery_auth.h"
#include "recovery_io.h"
#include "recovery_lcd.h"

static const char *TAG = "recovery_wifi";

#define WIFI_NVS_PARTITION "wifi_nvs"
#define NVS_NAMESPACE      "wifi_cfg"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"
#define NVS_KEY_AP_SSID    "ap_ssid"
#define NVS_KEY_AP_PASS    "ap_pass"

// Default AP SSID when the board has never been provisioned at all (should
// only ever be hit on a totally virgin `wifi_nvs`) -- kept distinguishable
// from the main app's own default so an operator watching for SSIDs can
// tell "recovery is up" from "the main app never got provisioned".
#define RECOVERY_AP_SSID_DEFAULT "kilnctl-recovery"
#define RECOVERY_STA_CONNECT_TIMEOUT_MS 15000
// After an established station link drops, keep reconnecting for this long,
// then fall back to the SoftAP (an AP that never comes up is the only way an
// operator can still reach the image once the router is gone).
#define RECOVERY_STA_RECONNECT_WINDOW_MS 30000

static EventGroupHandle_t s_wifi_events;
#define WIFI_UP_BIT BIT0

static volatile bool s_up = false;
static volatile bool s_sta_was_up = false;     // link came up at least once
static volatile bool s_fallback_started = false;
static volatile int64_t s_drop_since_us = 0;   // 0 = link currently up / no drop
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// Logs and returns the error; never aborts. Wi-Fi bring-up failure must never
// reboot-loop the recovery image (it is the only way back to a working board).
#define WIFI_TRY(expr)                                                              \
    do {                                                                            \
        esp_err_t e_ = (expr);                                                      \
        if (e_ != ESP_OK) {                                                         \
            ESP_LOGE(TAG, "%s failed: %s", #expr, esp_err_to_name(e_));             \
            return false;                                                           \
        }                                                                           \
    } while (0)

static bool nvs_read_str(const char *ns, const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(WIFI_NVS_PARTITION, ns, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

bool recovery_wifi_get_ap_password(char *out, size_t out_len)
{
    return nvs_read_str(NVS_NAMESPACE, NVS_KEY_AP_PASS, out, out_len);
}

// Build-time key mixed into the fallback secret. Override with
// -DRECOVERY_AUTH_BUILD_KEY=\"...\" per product build; PcTools must use the
// same string (docs/RECOVERY_IMAGE_PLAN.md, "Auth fallback secret").
#ifndef RECOVERY_AUTH_BUILD_KEY
#define RECOVERY_AUTH_BUILD_KEY "kilnctl-recovery-fallback-key-1"
#endif

// Derives the fallback secret from the factory eFuse MAC. false on failure.
static bool derive_fallback_secret(char out[RAUTH_FALLBACK_LEN + 1])
{
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return false;
    }
    uint8_t pre[96];
    size_t n = rauth_fallback_preimage(RECOVERY_AUTH_BUILD_KEY, mac, pre, sizeof(pre));
    if (n == 0) {
        return false;
    }
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    uint8_t digest[32];
    bool ok = mbedtls_md_setup(&ctx, info, 0) == 0 && mbedtls_md_starts(&ctx) == 0 &&
              mbedtls_md_update(&ctx, pre, n) == 0 && mbedtls_md_finish(&ctx, digest) == 0;
    mbedtls_md_free(&ctx);
    if (ok) {
        rauth_fallback_format(digest, out);
    }
    return ok;
}

bool recovery_wifi_get_auth_secret(char *out, size_t out_len, bool *fallback)
{
    char stored[65] = {0};
    bool have = nvs_read_str(NVS_NAMESPACE, NVS_KEY_AP_PASS, stored, sizeof(stored));
    if (have && rauth_ap_pass_usable(stored) && strlen(stored) < out_len) {
        strcpy(out, stored);
        if (fallback) {
            *fallback = false;
        }
        return true;
    }
    char fb[RAUTH_FALLBACK_LEN + 1];
    if (out_len < sizeof(fb) || !derive_fallback_secret(fb)) {
        return false;
    }
    strcpy(out, fb);
    if (fallback) {
        *fallback = true;
    }
    return true;
}

static bool start_softap(void);

// Pushes the current IPv4 address of the named netif to the LCD status screen.
static void publish_network(const char *ifkey, bool is_ap, const char *name)
{
    char ip[16] = "?";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    recovery_lcd_set_network(is_ap, name, ip);
}

// One-shot: tears the station down and brings the SoftAP up. Runs in its own
// task because the Wi-Fi event handler runs on the shared event-loop task,
// which must not block on esp_wifi_stop()/LCD drawing.
static void sta_fallback_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "station link lost for > %d ms -- falling back to SoftAP",
             RECOVERY_STA_RECONNECT_WINDOW_MS);
    s_up = false;
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    if (!start_softap()) {
        ESP_LOGE(TAG, "SoftAP fallback failed after station loss");
        recovery_lcd_set_no_network();
    }
    vTaskDelete(NULL);
}

// AP observability (counters only ever written from the event-loop task).
static volatile uint32_t s_ap_start_count;
static volatile uint32_t s_ap_stop_count;
static volatile uint32_t s_ap_sta_connect_total;
static volatile uint32_t s_ap_sta_disconnect_total;
static volatile uint32_t s_ap_sta_now;
static volatile int64_t s_last_event_us;
static const char *volatile s_last_event_name = "none";

static void note_event(const char *name)
{
    s_last_event_name = name;
    s_last_event_us = esp_timer_get_time();
}

void recovery_wifi_get_stats(recovery_wifi_stats_t *out)
{
    out->ap_start_count = s_ap_start_count;
    out->ap_stop_count = s_ap_stop_count;
    out->ap_sta_connect_total = s_ap_sta_connect_total;
    out->ap_sta_disconnect_total = s_ap_sta_disconnect_total;
    out->ap_sta_now = s_ap_sta_now;
    out->last_event_name = s_last_event_name;
    int64_t t = s_last_event_us;
    out->last_event_age_s = t ? (uint32_t)((esp_timer_get_time() - t) / 1000000) : 0;
    out->last_event_seen = t != 0;
}

static void on_ap_event(int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_AP_START:
        s_ap_start_count++;
        note_event("ap_start");
        ESP_LOGI(TAG, "AP_START (count %u)", (unsigned)s_ap_start_count);
        break;
    case WIFI_EVENT_AP_STOP:
        s_ap_stop_count++;
        note_event("ap_stop");
        ESP_LOGW(TAG, "AP_STOP (count %u, s_up=%d) -- the SoftAP stopped", (unsigned)s_ap_stop_count,
                 (int)s_up);
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *ev = (const wifi_event_ap_staconnected_t *)data;
        s_ap_sta_connect_total++;
        s_ap_sta_now++;
        note_event("ap_sta_connected");
        if (ev) {
            ESP_LOGI(TAG, "AP station joined: " MACSTR " aid=%d", MAC2STR(ev->mac), ev->aid);
        }
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *ev = (const wifi_event_ap_stadisconnected_t *)data;
        s_ap_sta_disconnect_total++;
        if (s_ap_sta_now > 0) {
            s_ap_sta_now--;
        }
        note_event("ap_sta_disconnected");
        if (ev) {
            ESP_LOGI(TAG, "AP station left: " MACSTR " aid=%d reason=%d", MAC2STR(ev->mac), ev->aid,
                     ev->reason);
        }
        break;
    }
    default:
        break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_AP_START || id == WIFI_EVENT_AP_STOP ||
                               id == WIFI_EVENT_AP_STACONNECTED ||
                               id == WIFI_EVENT_AP_STADISCONNECTED)) {
        on_ap_event(id, data);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_fallback_started) {
            return;
        }
        if (!s_sta_was_up) {
            // Initial attempt: recovery_wifi_start() owns the timeout.
            esp_wifi_connect();
            return;
        }
        int64_t now = esp_timer_get_time();
        if (s_drop_since_us == 0) {
            s_drop_since_us = now;
        }
        if ((now - s_drop_since_us) / 1000 >= RECOVERY_STA_RECONNECT_WINDOW_MS) {
            s_fallback_started = true;
            if (xTaskCreate(sta_fallback_task, "rec_fallback", 4096, NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "could not start the SoftAP fallback task");
                s_fallback_started = false; // retry on the next disconnect event
            }
            return;
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_drop_since_us = 0;
        if (s_sta_was_up) {
            s_up = true; // re-association after a drop
        }
        xEventGroupSetBits(s_wifi_events, WIFI_UP_BIT);
    }
}

static bool try_station(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    if (!nvs_read_str(NVS_NAMESPACE, NVS_KEY_SSID, ssid, sizeof(ssid))) {
        ESP_LOGW(TAG, "no stored station ssid in wifi_nvs -- skipping station attempt");
        return false;
    }
    (void)nvs_read_str(NVS_NAMESPACE, NVS_KEY_PASS, pass, sizeof(pass));

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
            return false;
        }
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_wifi_events, WIFI_UP_BIT);
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    WIFI_TRY(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_UP_BIT, pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(RECOVERY_STA_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_UP_BIT) {
        ESP_LOGI(TAG, "station link up (ssid=%s)", ssid);
        s_sta_was_up = true;
        publish_network("WIFI_STA_DEF", false, ssid);
        return true;
    }
    ESP_LOGW(TAG, "station link to '%s' did not come up within %d ms -- falling back to AP",
             ssid, RECOVERY_STA_CONNECT_TIMEOUT_MS);
    (void)esp_wifi_stop();
    return false;
}

static bool start_softap(void)
{
    char ap_ssid[33] = {0};
    char ap_pass[65] = {0};
    if (!nvs_read_str(NVS_NAMESPACE, NVS_KEY_AP_SSID, ap_ssid, sizeof(ap_ssid))) {
        strncpy(ap_ssid, RECOVERY_AP_SSID_DEFAULT, sizeof(ap_ssid) - 1);
    }
    // Never an OPEN AP: a missing/short ap_pass yields the eFuse-MAC fallback
    // secret (same secret the HTTP auth uses as its key material).
    bool fallback = false;
    bool have_pass = recovery_wifi_get_auth_secret(ap_pass, sizeof(ap_pass), &fallback);
    recovery_lcd_set_auth_fallback(have_pass && fallback);
    if (have_pass && fallback) {
        ESP_LOGW(TAG, "no usable ap_pass in wifi_nvs -- SoftAP uses the eFuse-MAC fallback secret");
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
            return false;
        }
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ap_ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    if (have_pass) {
        strncpy((char *)cfg.ap.password, ap_pass, sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        // Secret derivation itself failed (eFuse MAC unreadable): an open AP
        // is the only way left to stay reachable, and every mutating route
        // returns 500 without a key, so say so loudly.
        ESP_LOGE(TAG, "no AP secret available -- SoftAP is OPEN and mutating routes will 500");
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_AP));
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    WIFI_TRY(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up: ssid=%s open=%d", ap_ssid, cfg.ap.authmode == WIFI_AUTH_OPEN);
    s_up = true;
    publish_network("WIFI_AP_DEF", true, ap_ssid);
    return true;
}

void recovery_wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        ESP_LOGE(TAG, "event group alloc failed -- no network");
        recovery_lcd_set_no_network();
        return;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s -- no network", esp_err_to_name(err));
        recovery_lcd_set_no_network();
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE: loop already exists
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s -- no network", esp_err_to_name(err));
        recovery_lcd_set_no_network();
        return;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_DEFAULT) {
        init_cfg.nvs_enable = 0; // default nvs unusable (and never erased): driver runs RAM-only
    }
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s -- no network", esp_err_to_name(err));
        recovery_lcd_set_no_network();
        return;
    }
    /* docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md section 3
     * part A note: recovery also owns no read path for the driver's own
     * flash-persisted config (it reads ssid/pass out of wifi_nvs itself,
     * above) and would otherwise repopulate the default `nvs` partition's
     * nvs.net80211 namespace the main app's own fix (wifi_prov.c) stops
     * touching. Same fix, same reasoning. */
    esp_err_t store_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (store_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage(RAM) failed: %s -- driver will keep its own credential "
                 "copy in the default NVS partition", esp_err_to_name(store_err));
    }
    // A missing handler only costs the station attempt (it times out and the
    // SoftAP comes up), so these are logged, not fatal.
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi event handler register failed: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip event handler register failed: %s", esp_err_to_name(err));
    }

    if (try_station()) {
        s_up = true;
        return;
    }
    if (!start_softap()) {
        ESP_LOGE(TAG, "station and SoftAP both failed -- NO NETWORK (HTTP still started)");
        recovery_lcd_set_no_network();
    }
}

bool recovery_wifi_is_up(void)
{
    return s_up;
}
