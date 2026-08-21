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

/* TODO.md 8.1: this module's persisted store, split out of the default NVS
 * partition into its own partition so a corrupt/erased default partition
 * cannot take relay history with it. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* The blob has no version field of its own on disk before this change (a
 * bare uint32_t[KILN_IO_RELAY_COUNT]); wrapping it in a versioned struct
 * changes the on-disk layout, which is fine here -- unlike run_state.c's
 * blob, this one is diagnostic-only and already treats any size mismatch as
 * "start at zero", so the version add rides the same tolerant path. */
#define RELAY_CYCLES_VERSION 1

typedef struct {
    uint8_t  version;
    uint32_t counts[KILN_IO_RELAY_COUNT];
} relay_cycles_blob_t;

typedef struct {
    SemaphoreHandle_t lock;
    uint32_t          counts[KILN_IO_RELAY_COUNT];
    bool              dirty;
    int64_t           last_persist_us;
    bool              initialized;
} relay_cycles_t;

static relay_cycles_t s_rc;

/* Brings up KILN_NVS_PARTITION, erasing ONLY that partition if its contents
 * are unusable. Adapted from wifi_prov.c's nvs_partition_init() (2026-08-12
 * NVS-partition split): NO_FREE_PAGES / NEW_VERSION_FOUND leave NVS unable
 * to mount at all, so erasing is the only cure, but it must stay scoped to
 * the partition that is actually broken. */
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

/* One-time, one-directional copy of the old default-partition blob into
 * KILN_NVS_PARTITION, for boards provisioned by firmware predating the
 * split. The old copy is left in place (never deleted) so a rollback to
 * pre-split firmware still finds its counts -- see wifi_prov.c's
 * migrate_from_default_partition() for the fuller rationale. Only called
 * when KILN_NVS_PARTITION has nothing under NVS_KEY_CYCLES yet. */
static void migrate_from_default_partition(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }
    relay_cycles_blob_t old_blob;
    size_t len = sizeof(old_blob);
    err = nvs_get_blob(h, NVS_KEY_CYCLES, &old_blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        /* Nothing in the old location either (or it's the pre-version-field
         * bare uint32_t[] blob, a different size) -- nothing to migrate. */
        return;
    }
    if (len != sizeof(old_blob) || old_blob.version != RELAY_CYCLES_VERSION) {
        return;
    }

    nvs_handle_t hw;
    err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &hw);
    if (err != ESP_OK) {
        return;
    }
    err = nvs_set_blob(hw, NVS_KEY_CYCLES, &old_blob, sizeof(old_blob));
    if (err == ESP_OK) {
        err = nvs_commit(hw);
    }
    nvs_close(hw);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "migrated relay cycle counts from default NVS partition to '%s'", KILN_NVS_PARTITION);
    } else {
        ESP_LOGW(TAG, "relay cycle count migration to '%s' failed: %s", KILN_NVS_PARTITION, esp_err_to_name(err));
    }
}

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
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    relay_cycles_blob_t blob;
    blob.version = RELAY_CYCLES_VERSION;
    memcpy(blob.counts, s_rc.counts, sizeof(blob.counts));
    err = nvs_set_blob(h, NVS_KEY_CYCLES, &blob, sizeof(blob));
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

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- relay cycle counts will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
    }

    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memset(s_rc.counts, 0, sizeof(s_rc.counts));

    /* Migrate before the real load so a pre-split board's counts show up on
     * the very first boot after the update, not one boot late. */
    if (part_err == ESP_OK) {
        migrate_from_default_partition();
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        relay_cycles_blob_t blob;
        size_t len = sizeof(blob);
        err = nvs_get_blob(h, NVS_KEY_CYCLES, &blob, &len);
        /* BUG FIXED (matching zones_http.c's nvs_load_from()): this used to
         * gate BOTH the current-version and newer-version branches on
         * `len == sizeof(blob)` before ever looking at `version`, which
         * would misclassify a genuinely OLDER (smaller) blob as unreadable
         * corruption instead of the version check below. Only a blob too
         * short to even contain the `version` byte is genuinely ambiguous;
         * everything else must be classified by version first, with the
         * exact-size check applied only to the current-version case (a real
         * current-version blob is always written at exactly sizeof(blob)). */
        if (err == ESP_OK && len < sizeof(blob.version)) {
            ESP_LOGW(TAG, "relay cycle blob is too short to contain a version -- starting at zero");
        } else if (err == ESP_OK && blob.version == RELAY_CYCLES_VERSION) {
            if (len != sizeof(blob)) {
                ESP_LOGW(TAG, "relay cycle blob claims current version but is the wrong size -- starting at zero");
            } else {
                memcpy(s_rc.counts, blob.counts, sizeof(s_rc.counts));
            }
        } else if (err == ESP_OK && blob.version > RELAY_CYCLES_VERSION) {
            /* Newer than this firmware understands -- a firmware-rollback
             * case (TODO.md 8.1). Refuse to load rather than guess at a
             * layout this build doesn't know, and leave flash untouched so
             * a subsequent boot on the newer firmware still finds it. */
            ESP_LOGW(TAG, "relay cycle blob version %u is newer than this firmware's %u -- refusing to load, "
                     "leaving flash untouched", blob.version, RELAY_CYCLES_VERSION);
        } else if (err == ESP_OK) {
            /* blob.version < RELAY_CYCLES_VERSION: version 1 is the first
             * this field has ever had, so there is no older layout to
             * migrate from yet -- this is the hook point for when one
             * exists. */
            ESP_LOGW(TAG, "relay cycle blob version %u predates this firmware's %u with no migration defined -- "
                     "starting at zero", blob.version, RELAY_CYCLES_VERSION);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            /* Missing (first boot, or nothing survived migration) is the
             * only case treated identically to "start at zero" without a
             * warning; anything else (unreadable) is logged as corrupt
             * data. */
            memset(s_rc.counts, 0, sizeof(s_rc.counts));
            ESP_LOGW(TAG, "relay cycle blob load failed (%s) -- starting at zero",
                     esp_err_to_name(err));
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
