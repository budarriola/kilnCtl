#include "time_sync.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "time_sync";

/* Same partition/namespace convention as unit_pref.c/zones_http.c/
 * touch_cal_store.c -- a single small persisted string belongs in the
 * existing kiln_cfg namespace, not a new one. */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE      "kiln_cfg"
#define NVS_KEY_TZ         "time_tz"

#define SNTP_DEFAULT_SERVER "pool.ntp.org"

static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static time_sync_status_t s_status; /* guarded by s_status_mux */
/* Set true only once esp_netif_sntp_init() has actually succeeded.
 * time_sync_notify_got_ip() checks this before calling esp_netif_sntp_
 * start() -- that call does not itself check whether init ever ran, and
 * unconditionally does sntp_stop();sntp_init() regardless (its own doc
 * comment), so calling it after a failed/never-run init would arm a client
 * with no server configured. owner_task()-only: written once in time_sync_
 * start(), read (never written) in time_sync_notify_got_ip(); both run on
 * wifi_prov.c's owner_task(), so no lock is needed. */
static bool s_sntp_init_ok;

/* Copied verbatim from unit_pref.c's nvs_partition_init() -- same
 * partition, same NO_FREE_PAGES/NEW_VERSION_FOUND erase-and-retry, scoped
 * to only the broken partition. */
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

/* Runs on the lwIP SNTP task -- must stay fast and non-blocking, no logging
 * of anything that could itself block (ESP_LOGI is fine, it's buffered). */
static void sntp_sync_cb(struct timeval *tv)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.ever_synced = true;
    s_status.last_sync_epoch = tv ? (time_t)tv->tv_sec : time(NULL);
    portEXIT_CRITICAL(&s_status_mux);
    ESP_LOGI(TAG, "SNTP sync landed");
}

static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
    portENTER_CRITICAL(&s_status_mux);
    strncpy(s_status.tz, tz, TIME_SYNC_TZ_MAX_LEN);
    s_status.tz[TIME_SYNC_TZ_MAX_LEN] = '\0';
    portEXIT_CRITICAL(&s_status_mux);
}

esp_err_t time_sync_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_sntp_init_ok = false;
    time_sync_tz_effective(NULL, s_status.tz, sizeof(s_status.tz)); /* UTC default until NVS says otherwise */

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- defaulting to %s this boot",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err), s_status.tz);
        apply_tz(s_status.tz);
    } else {
        nvs_handle_t h;
        esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
        char stored[TIME_SYNC_TZ_MAX_LEN + 1] = { 0 };
        bool have_stored = false;
        if (err == ESP_OK) {
            size_t len = sizeof(stored);
            if (nvs_get_str(h, NVS_KEY_TZ, stored, &len) == ESP_OK) {
                have_stored = true;
            }
            nvs_close(h);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open_from_partition failed: %s -- defaulting to %s this boot",
                     esp_err_to_name(err), s_status.tz);
        }
        char effective[TIME_SYNC_TZ_MAX_LEN + 1];
        time_sync_tz_effective(have_stored ? stored : NULL, effective, sizeof(effective));
        apply_tz(effective);
    }
    ESP_LOGI(TAG, "timezone: %s", s_status.tz);

    /* start = false, wait_for_sync = false: prepared but NOT armed and NOT
     * blocking -- see this module's header comment. time_sync_notify_got_
     * ip() is what actually starts the client, once STA has an IP to send
     * the NTP request over. */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_DEFAULT_SERVER);
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = sntp_sync_cb;
    esp_err_t sntp_err = esp_netif_sntp_init(&config);
    if (sntp_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s -- no network time this boot", esp_err_to_name(sntp_err));
        return ESP_OK; /* non-fatal, same convention as unit_pref_start() */
    }
    s_sntp_init_ok = true;
    return ESP_OK;
}

void time_sync_notify_got_ip(void)
{
    /* Init never ran or failed (see time_sync_start()) -- esp_netif_sntp_
     * start() does not check this itself and would happily sntp_stop();
     * sntp_init() a client with no server configured. Bail out here instead
     * of relying on that being harmless by luck. */
    if (!s_sntp_init_ok) {
        ESP_LOGW(TAG, "SNTP client was never initialized -- not starting");
        return;
    }

    /* esp_netif_sntp_start() "start[s] it if it wasn't started during init
     * ... or restart[s] it if already started" (its own doc comment) --
     * safe and cheap to call on every GOT_IP, including a reconnect after a
     * flap. It does not wait for a sync to land -- see time_sync.h's
     * opening comment for what this call itself blocks on. */
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
    }
}

void time_sync_get_status(time_sync_status_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    out->now_epoch = out->ever_synced ? time(NULL) : 0;
}

esp_err_t time_sync_set_tz(const char *tz)
{
    if (!time_sync_tz_is_valid(tz)) {
        ESP_LOGW(TAG, "refusing invalid TZ string");
        return ESP_ERR_INVALID_ARG;
    }

    /* Live immediately, same "in-RAM/live truth first" convention as
     * unit_pref_set() -- an NVS write failure below must not leave the
     * board running the OLD timezone after reporting success. */
    apply_tz(tz);

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- TZ applied live but NOT persisted",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        return ESP_OK;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open_from_partition (RW) failed: %s -- TZ applied live but NOT persisted",
                 esp_err_to_name(err));
        return ESP_OK;
    }
    err = nvs_set_str(h, NVS_KEY_TZ, tz);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TZ persist failed: %s -- applied live for this boot only", esp_err_to_name(err));
    }
    return ESP_OK;
}
