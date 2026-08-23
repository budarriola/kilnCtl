#include "watchdog_cfg.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

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

/* Pure, host-testable: the esp_task_wdt_config_t this build's own Kconfig
 * says the TWDT should run with, for a given trigger_panic value.
 * timeout_ms/idle_core_mask are ALWAYS derived from CONFIG_ESP_TASK_WDT_*
 * here -- never hardcoded -- so this module can only ever change
 * trigger_panic, never silently drift the timeout the rest of the firmware
 * was sized around. */
static esp_task_wdt_config_t build_twdt_config(bool trigger_panic)
{
    esp_task_wdt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_ms = (uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000u;
    cfg.trigger_panic = trigger_panic;
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    cfg.idle_core_mask |= (1u << 0);
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    cfg.idle_core_mask |= (1u << 1);
#endif
    return cfg;
}

/* Applies `disabled` to the already-running TWDT. trigger_panic is the
 * inverse of disabled: disabled=true means trigger_panic=false. Not
 * exercised by the host tests (no real TWDT off-target) -- see
 * test_watchdog_cfg.c's header comment. */
static esp_err_t apply_panic_disabled(bool disabled)
{
    esp_task_wdt_config_t cfg = build_twdt_config(!disabled);
    return esp_task_wdt_reconfigure(&cfg);
}

/* Adapted from boot_guard.c's nvs_partition_init() -- same "erase ONLY this
 * partition if its contents are unusable" recovery. */
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

static esp_err_t persist_disabled_flag(bool disabled)
{
    watchdog_cfg_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = WATCHDOG_CFG_RECORD_VERSION;
    rec.panic_disabled = disabled ? 1u : 0u;
    rec.crc32 = record_crc(&rec);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_REC, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Loads the persisted setting, or false (panic ENABLED, the safe default) if
 * there is nothing valid to load -- missing (first boot), wrong size, wrong
 * version, or a bad CRC all collapse to the same "not set" answer, same
 * load-tolerant convention as boot_guard.c/run_state.c. */
static bool load_disabled_flag(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }
    watchdog_cfg_record_t rec;
    size_t len = sizeof(rec);
    err = nvs_get_blob(h, NVS_KEY_REC, &rec, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(rec)) {
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

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- watchdog-panic-disabled setting will not "
                      "persist (this boot stays with panic ENABLED, the safe default)",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
    }

    bool disabled = (part_err == ESP_OK) ? load_disabled_flag() : false;

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

    esp_err_t persist_err = persist_disabled_flag(disabled);
    esp_err_t apply_err = apply_panic_disabled(disabled);

    xSemaphoreTake(s_wd.lock, portMAX_DELAY);
    s_wd.panic_disabled = disabled;
    s_wd.initialized = true;
    xSemaphoreGive(s_wd.lock);

    if (disabled) {
        ESP_LOGE(TAG, "task-watchdog panic DISABLED by request (source: %s) -- a hung task will no "
                      "longer reboot this board; relays stay in whatever state they were last "
                      "commanded (persist=%s, apply=%s)",
                 source, esp_err_to_name(persist_err), esp_err_to_name(apply_err));
    } else {
        ESP_LOGW(TAG, "task-watchdog panic RE-ENABLED (source: %s, restoring the safe default) "
                      "(persist=%s, apply=%s)",
                 source, esp_err_to_name(persist_err), esp_err_to_name(apply_err));
    }

    return persist_err;
}
