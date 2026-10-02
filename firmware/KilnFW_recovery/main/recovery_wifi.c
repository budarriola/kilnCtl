// recovery_wifi.c -- see recovery_wifi.h.
#include "recovery_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

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

static EventGroupHandle_t s_wifi_events;
#define WIFI_UP_BIT BIT0

static bool s_up = false;

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

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // One retry is enough here: recovery_wifi_start() times out and
        // falls to SoftAP on its own rather than looping forever trying a
        // station link that may simply be out of range.
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_UP_BIT);
    }
}

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

static bool try_station(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    if (!nvs_read_str(NVS_NAMESPACE, NVS_KEY_SSID, ssid, sizeof(ssid))) {
        ESP_LOGW(TAG, "no stored station ssid in wifi_nvs -- skipping station attempt");
        return false;
    }
    (void)nvs_read_str(NVS_NAMESPACE, NVS_KEY_PASS, pass, sizeof(pass));

    esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_UP_BIT, pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(RECOVERY_STA_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_UP_BIT) {
        ESP_LOGI(TAG, "station link up (ssid=%s)", ssid);
        publish_network("WIFI_STA_DEF", false, ssid);
        return true;
    }
    ESP_LOGW(TAG, "station link to '%s' did not come up within %d ms -- falling back to AP",
             ssid, RECOVERY_STA_CONNECT_TIMEOUT_MS);
    ESP_ERROR_CHECK(esp_wifi_stop());
    return false;
}

static void start_softap(void)
{
    char ap_ssid[33] = {0};
    char ap_pass[65] = {0};
    if (!nvs_read_str(NVS_NAMESPACE, NVS_KEY_AP_SSID, ap_ssid, sizeof(ap_ssid))) {
        strncpy(ap_ssid, RECOVERY_AP_SSID_DEFAULT, sizeof(ap_ssid) - 1);
    }
    bool have_pass = nvs_read_str(NVS_NAMESPACE, NVS_KEY_AP_PASS, ap_pass, sizeof(ap_pass));

    esp_netif_create_default_wifi_ap();

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ap_ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    if (have_pass && strlen(ap_pass) >= 8) {
        strncpy((char *)cfg.ap.password, ap_pass, sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up: ssid=%s open=%d", ap_ssid, cfg.ap.authmode == WIFI_AUTH_OPEN);
    s_up = true;
    publish_network("WIFI_AP_DEF", true, ap_ssid);
}

void recovery_wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_DEFAULT) {
        init_cfg.nvs_enable = 0; // default nvs unusable (and never erased): driver runs RAM-only
    }
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
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
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    if (try_station()) {
        s_up = true;
        return;
    }
    start_softap();
}

bool recovery_wifi_is_up(void)
{
    return s_up;
}
