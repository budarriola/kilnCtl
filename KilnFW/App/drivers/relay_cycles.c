#include "relay_cycles.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "relay_cycles";

/* Same namespace as the rest of this board's configuration (zones_http.c,
 * rules_http.c) but its own key -- deliberately NOT folded into the
 * zones_cfg blob, whose loader treats any size change as "corrupt, start
 * unconfigured". Adding a field there would silently wipe a user's zone
 * setup on the first boot after the update. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_CYCLES "relay_cyc"

typedef struct {
    SemaphoreHandle_t lock;
    uint32_t          counts[KILN_IO_RELAY_COUNT];
    bool              dirty;
    int64_t           last_persist_us;
    bool              initialized;
} relay_cycles_t;

static relay_cycles_t s_rc;

static bool ensure_lock(void)
{
    if (!s_rc.lock) {
        s_rc.lock = xSemaphoreCreateMutex();
        if (!s_rc.lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- cycle counts will not be kept");
            return false;
        }
    }
    return true;
}

static esp_err_t persist_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_CYCLES, s_rc.counts, sizeof(s_rc.counts));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_rc.dirty = false;
        s_rc.last_persist_us = esp_timer_get_time();
    }
    return err;
}

esp_err_t relay_cycles_init(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memset(s_rc.counts, 0, sizeof(s_rc.counts));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(s_rc.counts);
        err = nvs_get_blob(h, NVS_KEY_CYCLES, s_rc.counts, &len);
        if (err != ESP_OK || len != sizeof(s_rc.counts)) {
            /* Missing (first boot) or written by a build with a different
             * relay count: start from zero rather than refusing to run. The
             * counter is diagnostic, not safety-critical -- losing it costs
             * history, not correctness. */
            memset(s_rc.counts, 0, sizeof(s_rc.counts));
            if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "relay cycle blob load failed or wrong size (%s) -- starting at zero",
                         esp_err_to_name(err));
            }
        }
        nvs_close(h);
    }

    s_rc.dirty = false;
    s_rc.last_persist_us = esp_timer_get_time();
    s_rc.initialized = true;
    xSemaphoreGive(s_rc.lock);

    ESP_LOGI(TAG, "relay contact cycles loaded: %lu %lu %lu %lu", (unsigned long)s_rc.counts[0],
             (unsigned long)(KILN_IO_RELAY_COUNT > 1 ? s_rc.counts[1] : 0),
             (unsigned long)(KILN_IO_RELAY_COUNT > 2 ? s_rc.counts[2] : 0),
             (unsigned long)(KILN_IO_RELAY_COUNT > 3 ? s_rc.counts[3] : 0));
    return ESP_OK;
}

void relay_cycles_add(uint8_t relay_mask, uint32_t cycles)
{
    if (cycles == 0 || relay_mask == 0 || !ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (relay_mask & (uint8_t)(1u << r)) {
            /* Saturate rather than wrap: a wrapped contact-life counter reads
             * as a brand-new relay, which is the one wrong answer that
             * matters here. */
            if (s_rc.counts[r] > UINT32_MAX - cycles) {
                s_rc.counts[r] = UINT32_MAX;
            } else {
                s_rc.counts[r] += cycles;
            }
            s_rc.dirty = true;
        }
    }
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_get(uint32_t *out)
{
    if (!out) {
        return;
    }
    if (!ensure_lock()) {
        memset(out, 0, sizeof(uint32_t) * KILN_IO_RELAY_COUNT);
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memcpy(out, s_rc.counts, sizeof(s_rc.counts));
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_maybe_persist(void)
{
    if (!ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    bool due = s_rc.dirty &&
               (esp_timer_get_time() - s_rc.last_persist_us) >= (int64_t)RELAY_CYCLES_PERSIST_INTERVAL_S * 1000000;
    esp_err_t err = due ? persist_locked() : ESP_OK;
    xSemaphoreGive(s_rc.lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "periodic persist failed: %s (counts kept in RAM, will retry)", esp_err_to_name(err));
    }
}

esp_err_t relay_cycles_flush(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    esp_err_t err = s_rc.dirty ? persist_locked() : ESP_OK;
    xSemaphoreGive(s_rc.lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
    }
    return err;
}
