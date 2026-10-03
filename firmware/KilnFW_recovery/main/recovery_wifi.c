// recovery_wifi.c -- see recovery_wifi.h.
#include "recovery_wifi.h"

#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "recovery_io.h"
#include "recovery_lcd.h"
#include "recovery_passphrase.h"

static const char *TAG = "recovery_wifi";

#define WIFI_NVS_PARTITION "wifi_nvs"
#define NVS_NAMESPACE      "wifi_cfg"
#define NVS_KEY_AP_SSID    "ap_ssid"

// Default AP SSID when the board has never been provisioned at all (should
// only ever be hit on a totally virgin `wifi_nvs`) -- kept distinguishable
// from the main app's own default so an operator watching for SSIDs can
// tell "recovery is up" from "the main app never got provisioned".
#define RECOVERY_AP_SSID_DEFAULT "kilnctl-recovery"

static volatile bool s_up = false;
static esp_netif_t *s_ap_netif = NULL;

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

// Zeroes a buffer in a way the optimizer may not elide.
static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
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
    }
}

// Brings the SoftAP up with this boot's random WPA2 passphrase `pass` (from
// generate_passphrase()) and hands it to the LCD, then wipes `pass` on every
// path. The passphrase exists only in recovery_wifi_start()'s stack frame, the
// Wi-Fi driver's RAM config and the LCD module's RAM copy: it is never logged,
// never stored in NVS and never returned by any HTTP route (owner decision
// 2026-10-02: physical sight of the screen is the only access control).
static bool start_softap(char pass[RPASS_LEN + 1])
{
    char ap_ssid[33] = {0};
    if (!nvs_read_str(NVS_NAMESPACE, NVS_KEY_AP_SSID, ap_ssid, sizeof(ap_ssid)) || !ap_ssid[0]) {
        strncpy(ap_ssid, RECOVERY_AP_SSID_DEFAULT, sizeof(ap_ssid) - 1);
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
            secure_zero(pass, RPASS_LEN + 1);
            return false;
        }
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ap_ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    memcpy(cfg.ap.password, pass, RPASS_LEN); // cfg is zeroed: NUL-terminated
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    // AP only: no station interface exists, so the unauthenticated recovery
    // routes can be reached only by a client that joined this AP, i.e. one
    // that saw the passphrase on the LCD. (esp_http_server cannot be bound to
    // a single netif; with STA never created, the AP is the only interface.)
    esp_err_t e1 = esp_wifi_set_mode(WIFI_MODE_AP);
    esp_err_t e2 = e1 == ESP_OK ? esp_wifi_set_config(WIFI_IF_AP, &cfg) : e1;
    esp_err_t e3 = e2 == ESP_OK ? esp_wifi_start() : e2;
    secure_zero(&cfg, sizeof(cfg));
    if (e3 != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP bring-up failed: %s", esp_err_to_name(e3));
        secure_zero(pass, RPASS_LEN + 1);
        return false;
    }
    ESP_LOGI(TAG, "SoftAP up: ssid=%s (WPA2, passphrase on the LCD only)", ap_ssid);
    s_up = true;

    char ip[16] = "192.168.4.1";
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    recovery_lcd_set_ap(ap_ssid, pass, ip);
    secure_zero(pass, RPASS_LEN + 1);
    return true;
}

// The hardware RNG is only a true RNG while the RF subsystem or the SAR-ADC
// entropy source is on (ESP-IDF "Random Number Generation"); before
// esp_wifi_start() neither is, so the entropy source is enabled explicitly for
// the draw. Must run before esp_wifi_init(): bootloader_random_enable() is not
// safe while Wi-Fi or the ADC is in use (this image uses no ADC).
static void generate_passphrase(char pass[RPASS_LEN + 1])
{
    uint8_t rnd[RPASS_LEN];
    bootloader_random_enable();
    esp_fill_random(rnd, sizeof(rnd));
    bootloader_random_disable();
    rpass_format(rnd, pass);
    secure_zero(rnd, sizeof(rnd));
}

void recovery_wifi_start(void)
{
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

    char pass[RPASS_LEN + 1];
    generate_passphrase(pass); // before esp_wifi_init(), see its comment

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_DEFAULT) {
        init_cfg.nvs_enable = 0; // default nvs unusable (and never erased): driver runs RAM-only
    }
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s -- no network", esp_err_to_name(err));
        secure_zero(pass, sizeof(pass));
        recovery_lcd_set_no_network();
        return;
    }
    /* docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md section 3
     * part A note: keep the driver's own config copy in RAM, so the random
     * passphrase is never persisted by the driver either, and the default
     * `nvs` partition's nvs.net80211 namespace stays untouched. */
    esp_err_t store_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (store_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage(RAM) failed: %s -- driver may keep its own config "
                 "copy in the default NVS partition", esp_err_to_name(store_err));
    }
    // A missing handler only costs the AP statistics, so this is logged, not fatal.
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi event handler register failed: %s", esp_err_to_name(err));
    }

    if (!start_softap(pass)) { // wipes pass on every path
        ESP_LOGE(TAG, "SoftAP failed -- NO NETWORK (HTTP still started)");
        recovery_lcd_set_no_network();
    }
}

bool recovery_wifi_is_up(void)
{
    return s_up;
}
