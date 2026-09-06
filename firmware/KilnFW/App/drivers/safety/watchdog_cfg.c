#include "watchdog_cfg.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "hal_esp_common.h"
#include "hal_kv.h"
#include "hal_wdt.h"

static const char *TAG = "watchdog_cfg";

/* Same "kiln_nvs" partition as run_state.c/boot_guard.c/relay_cycles.c, own
 * namespace+key so a corrupt/rejected watchdog_cfg record can never take any
 * of those down with it and vice versa -- see boot_guard.c's identical
 * reasoning. */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "watchdog_cfg"
#define NVS_KEY_REC "panic_dis"

/* Bumped whenever watchdog_cfg_record_t's layout changes. Same "discard
 * rather than migrate" convention as boot_guard.c/run_state.c -- a lost
 * record across a firmware update costs at most reverting to the safe
 * default (panic ENABLED), never a reason to mis-parse an old layout. */
#define WATCHDOG_CFG_RECORD_VERSION 1

/* Persisted verbatim as one fixed-size NVS blob, CRC32 over every byte up to
 * (not including) crc32 itself -- see boot_guard.c's header comment for why
 * a CRC, not just a version/size bar, matters for a record whose corruption
 * can silently change safety-relevant behavior in either direction. */
typedef struct {
    uint8_t  version;
    uint8_t  panic_disabled; /* 0/1 */
    uint8_t  reserved[2];    /* explicit, so crc32 below is 4-aligned */
    uint32_t crc32;
} watchdog_cfg_record_t;

/* Plain array-size trick instead of _Static_assert -- see boot_guard.c's
 * identical comment: this file is compiled both by ESP-IDF and, unmodified,
 * by MSVC for the host tests, which rejects a bare file-scope
 * _Static_assert outside /std:c11. */
typedef char watchdog_cfg_record_size_check[(sizeof(watchdog_cfg_record_t) == 8) ? 1 : -1];

typedef struct {
    SemaphoreHandle_t lock;
    bool initialized;
    bool panic_disabled; /* live RAM value, kept in sync with NVS */
} watchdog_cfg_ctx_t;

static watchdog_cfg_ctx_t s_wd;

/* Table-less CRC32 (IEEE 802.3/zlib polynomial), reimplemented locally
 * rather than pulling in esp_rom_crc.h -- same reasoning and same algorithm
 * as boot_guard.c's crc32_compute(): this module's own host tests build and
 * run entirely off-target with no ESP-IDF ROM available. Verified against
 * the standard CRC32 test vector ("123456789" -> 0xCBF43926) in
 * test_watchdog_cfg.c. */
static uint32_t crc32_compute(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t record_crc(const watchdog_cfg_record_t *rec)
{
    return crc32_compute(rec, offsetof(watchdog_cfg_record_t, crc32));
}

/* Pure predicate, no I/O -- exercised directly by test_watchdog_cfg.c. */
static bool record_is_valid(const watchdog_cfg_record_t *rec)
{
    return rec->version == WATCHDOG_CFG_RECORD_VERSION && rec->crc32 == record_crc(rec);
}

/* Applies `disabled` to the already-running TWDT via hal_wdt_set_panic_
 * disabled() -- HAL Phase 3 item 7 migration off esp_task_wdt_reconfigure()
 * directly. timeout_ms/idle_core_mask stay entirely inside hal_wdt_esp.c
 * (still always CONFIG_ESP_TASK_WDT_*-derived there, never hardcoded, same
 * "this module can only ever change trigger_panic" guarantee as before);
 * this module now only ever passes the one bit hal_wdt.h's header comment
 * says this call owns. Exercised by the host tests against fake_wdt.c (no
 * real TWDT off-target, but hal_wdt_set_panic_disabled() itself is now a
 * thin, generic HAL call worth checking against the fake -- see
 * test_watchdog_cfg.c). */
static esp_err_t apply_panic_disabled(bool disabled)
{
    return hal_status_to_esp_err(hal_wdt_set_panic_disabled(disabled));
}

/* Adapted from boot_guard.c's nvs_partition_init() -- same "erase ONLY this
 * partition if its contents are unusable" recovery. */
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

static hal_status_t persist_disabled_flag(bool disabled)
{
    watchdog_cfg_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = WATCHDOG_CFG_RECORD_VERSION;
    rec.panic_disabled = disabled ? 1u : 0u;
    rec.crc32 = record_crc(&rec);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, NVS_KEY_REC, &rec, sizeof(rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

/* Loads the persisted setting, or false (panic ENABLED, the safe default) if
 * there is nothing valid to load -- missing (first boot), wrong size, wrong
 * version, or a bad CRC all collapse to the same "not set" answer, same
 * load-tolerant convention as boot_guard.c/run_state.c. */
static bool load_disabled_flag(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    watchdog_cfg_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec)) {
        return false;
    }
    if (!record_is_valid(&rec)) {
        ESP_LOGW(TAG, "watchdog-panic-disabled record failed its version/CRC check -- treating as "
                      "\"not set\" (panic stays ENABLED, the safe default)");
        return false;
    }
    return rec.panic_disabled != 0;
}

static bool ensure_lock(void)
{
    if (!s_wd.lock) {
        s_wd.lock = xSemaphoreCreateMutex();
        if (!s_wd.lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- watchdog_cfg cannot track state this boot "
                          "(defaulting to panic ENABLED, the safe default)");
            return false;
        }
    }
    return true;
}

void watchdog_cfg_init(void)
{
    if (!ensure_lock()) {
        s_wd.initialized = true;
        s_wd.panic_disabled = false;
        return;
    }

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- watchdog-panic-disabled setting will not "
                      "persist (this boot stays with panic ENABLED, the safe default)",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
    }

    bool disabled = (part_err == HAL_OK) ? load_disabled_flag() : false;

    xSemaphoreTake(s_wd.lock, portMAX_DELAY);
    s_wd.panic_disabled = disabled;
    s_wd.initialized = true;
    xSemaphoreGive(s_wd.lock);

    if (disabled) {
        esp_err_t apply_err = apply_panic_disabled(true);
        if (apply_err == ESP_OK) {
            /* Unmissable, in the spirit of boot_guard.h's recovery-mode
             * banner -- a safety default turned off must never be quietly
             * true. */
            ESP_LOGE(TAG, "############################################################");
            ESP_LOGE(TAG, "# TASK-WATCHDOG PANIC IS DISABLED (dev switch, persisted) #");
            ESP_LOGE(TAG, "# A hung task will NOT reboot this board. Relays stay in  #");
            ESP_LOGE(TAG, "# whatever state they were last commanded, indefinitely.  #");
            ESP_LOGE(TAG, "# Re-enable via UART SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED#");
            ESP_LOGE(TAG, "# or the web debug page (/diagnostics).                   #");
            ESP_LOGE(TAG, "############################################################");
        } else {
            ESP_LOGE(TAG, "watchdog_cfg: NVS says panic-disabled but esp_task_wdt_reconfigure() "
                          "failed: %s -- panic behavior UNCHANGED (stays whatever the TWDT booted "
                          "with)", esp_err_to_name(apply_err));
        }
    }
}

bool watchdog_cfg_panic_disabled(void)
{
    if (!s_wd.initialized) {
        return false;
    }
    bool v = false;
    if (s_wd.lock) {
        xSemaphoreTake(s_wd.lock, portMAX_DELAY);
        v = s_wd.panic_disabled;
        xSemaphoreGive(s_wd.lock);
    } else {
        v = s_wd.panic_disabled;
    }
    return v;
}

esp_err_t watchdog_cfg_set_panic_disabled(bool disabled, const char *source)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }
    if (!source) {
        source = "unknown";
    }

    hal_status_t persist_err = persist_disabled_flag(disabled);
    esp_err_t apply_err = apply_panic_disabled(disabled);

    xSemaphoreTake(s_wd.lock, portMAX_DELAY);
    s_wd.panic_disabled = disabled;
    s_wd.initialized = true;
    xSemaphoreGive(s_wd.lock);

    if (disabled) {
        ESP_LOGE(TAG, "task-watchdog panic DISABLED by request (source: %s) -- a hung task will no "
                      "longer reboot this board; relays stay in whatever state they were last "
                      "commanded (persist=%s, apply=%s)",
                 source, hal_status_to_name(persist_err), esp_err_to_name(apply_err));
    } else {
        ESP_LOGW(TAG, "task-watchdog panic RE-ENABLED (source: %s, restoring the safe default) "
                      "(persist=%s, apply=%s)",
                 source, hal_status_to_name(persist_err), esp_err_to_name(apply_err));
    }

    return hal_status_to_esp_err(persist_err);
}
