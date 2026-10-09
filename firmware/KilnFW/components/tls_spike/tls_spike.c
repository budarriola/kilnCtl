/* WP7 TLS spike: one-shot HTTPS GET chain against GitHub, heap numbers only.
 * See docs/GITHUB_RELEASE_UPDATE_PLAN.md section 14. Never writes flash. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_private/startup_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TLS_SPIKE";

/* esp_http_client_get_header() returns REQUEST headers (IDF v6.0.2), so the redirect Location is
 * captured from the response header event instead. Bounded; an overflow or a second Location
 * counts as refused. Production twin: update_loc_capture_* in App/drivers/update/update_url.c. */
static char s_loc[2048];
static bool s_loc_seen;
static bool s_loc_refused;

static esp_err_t spike_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key != NULL && strcasecmp(e->header_key, "Location") == 0) {
        size_t n = e->header_value != NULL ? strlen(e->header_value) : sizeof(s_loc);
        if (s_loc_seen || n >= sizeof(s_loc)) {
            s_loc_refused = true;
            s_loc[0] = '\0';
        } else {
            memcpy(s_loc, e->header_value, n + 1);
            s_loc_seen = true;
        }
    }
    return ESP_OK;
}

static volatile uint32_t s_min_free_internal = UINT32_MAX;
static volatile uint32_t s_min_largest_block = UINT32_MAX;
static volatile bool s_sampler_run;

static uint32_t free_int(void) { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static uint32_t largest_int(void) { return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "HEAP %-22s int_free=%u int_largest=%u int_min_global=%u psram_free=%u sampled_min_free=%u",
             stage, (unsigned)free_int(), (unsigned)largest_int(),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)s_min_free_internal);
}

static void sampler_task(void *arg)
{
    (void)arg;
    while (s_sampler_run) {
        uint32_t f = free_int();
        if (f < s_min_free_internal) s_min_free_internal = f;
        uint32_t l = largest_int();
        if (l < s_min_largest_block) s_min_largest_block = l;
        vTaskDelay(1);
    }
    vTaskDeleteWithCaps(NULL);
}

static bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

static void one_chain(int iter)
{
    static char url[2048];
    strlcpy(url, CONFIG_KILNCTL_TLS_SPIKE_URL, sizeof url);
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) { ESP_LOGE(TAG, "no PSRAM chunk"); return; }

    s_min_free_internal = UINT32_MAX;
    s_min_largest_block = UINT32_MAX;
    log_heap("chain_start");

    for (int hop = 0; hop < 4; hop++) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 20000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
            .event_handler = spike_http_event,
            .disable_auto_redirect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_loc_seen = false;
        s_loc_refused = false;
        s_loc[0] = '\0';
        log_heap("pre_init");
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) { ESP_LOGE(TAG, "init failed"); break; }
        esp_http_client_set_header(c, "User-Agent", "kilnCtl-tls-spike");
        if (strstr(url, "/releases/assets/")) {
            esp_http_client_set_header(c, "Accept", "application/octet-stream");
        } else if (strstr(url, "api.github.com")) {
            esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
        }
        log_heap("pre_open");
        esp_err_t err = esp_http_client_open(c, 0);
        log_heap("post_open_handshake");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hop %d open failed: %s", hop, esp_err_to_name(err));
            esp_http_client_cleanup(c);
            break;
        }
        int64_t clen = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        ESP_LOGI(TAG, "hop %d status=%d content_length=%lld url_len=%u", hop, status, (long long)clen, (unsigned)strlen(url));
        log_heap("post_headers");
        if (is_redirect(status)) {
            const char *loc = s_loc;
            if (s_loc_seen && !s_loc_refused) {
                ESP_LOGI(TAG, "redirect Location length=%u", (unsigned)strlen(loc));
                if (strlen(loc) < sizeof url && strncmp(loc, "https://", 8) == 0) {
                    strlcpy(url, loc, sizeof url);
                    esp_http_client_close(c);
                    esp_http_client_cleanup(c);
                    log_heap("post_cleanup_redirect");
                    continue;
                }
                ESP_LOGE(TAG, "redirect target rejected (too long or not https)");
            } else {
                ESP_LOGE(TAG, "redirect without Location");
            }
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            break;
        }
        size_t total = 0, next_log = 65536;
        int n;
        while ((n = esp_http_client_read(c, (char *)chunk, 4096)) > 0) {
            total += (size_t)n;
            if (total >= next_log) { log_heap("body"); next_log += 65536; }
            if (total >= CONFIG_KILNCTL_TLS_SPIKE_MAX_BYTES) break;
            vTaskDelay(1);
        }
        ESP_LOGI(TAG, "body bytes=%u last_read=%d", (unsigned)total, n);
        log_heap("post_body");
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        log_heap("post_cleanup");
        break;
    }
    ESP_LOGI(TAG, "RESULT iter=%d sampled_min_free_internal=%u sampled_min_largest_block=%u (owner floor 8192)",
             iter, (unsigned)s_min_free_internal, (unsigned)s_min_largest_block);
    heap_caps_free(chunk);
}

static void spike_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0 && time(NULL) > 1700000000) break;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "STA IP and wall time ready; waiting %d s", CONFIG_KILNCTL_TLS_SPIKE_DELAY_S);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_KILNCTL_TLS_SPIKE_DELAY_S * 1000));

    s_sampler_run = true;
    xTaskCreatePinnedToCoreWithCaps(sampler_task, "tls_spike_smp", 3072, NULL, 10, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int i = 0; i < CONFIG_KILNCTL_TLS_SPIKE_REPEAT; i++) {
        one_chain(i);
        ESP_LOGI(TAG, "task stack high-water=%u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    s_sampler_run = false;
    ESP_LOGI(TAG, "DONE");
    vTaskDeleteWithCaps(NULL);
}

ESP_SYSTEM_INIT_FN(tls_spike_start, SECONDARY, BIT(0), 999)
{
    xTaskCreatePinnedToCoreWithCaps(spike_task, "tls_spike", 12288, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ESP_OK;
}
