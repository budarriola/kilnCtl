#include "log_store_mount.h"

#include <stdbool.h>

#include "esp_log.h"
#if CONFIG_KILNCTL_LOGS_LITTLEFS
#include "esp_littlefs.h"
#else
#include "esp_spiffs.h"
#endif

#include "log_store.h"
#include "flash_worker.h"

static const char *TAG = "log_store";

static bool s_mounted = false;

esp_err_t log_store_mount(void)
{
    if (s_mounted) {
        return ESP_OK; /* idempotent, same convention as *_start() elsewhere */
    }

    /* format_if_mount_failed=true: a corrupt or never-before-formatted
     * `logs` partition (first boot after this pass's partitions.csv change,
     * or a genuinely corrupted filesystem) is reformatted rather than
     * leaving the board unable to persist logs for the rest of its life.
     * Losing whatever logs were already on it is an acceptable cost -- this
     * is a rotating, bounded LOG store, not the NVS config/stats partitions
     * partitions.csv's own header comment is so careful never to move or
     * reformat. */
#if CONFIG_KILNCTL_LOGS_LITTLEFS
    /* partitions.csv's `logs` line subtype must be `littlefs`, not `spiffs`,
     * for this to mount a filesystem that was actually formatted for it --
     * see this option's Kconfig help. Step 4 (docs/FILESYSTEM_PLAN.md) is
     * what flips that subtype; this branch alone does not. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/logs",
        .partition_label = "logs",
        .partition = NULL,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register(logs) failed: %s -- firing/autotune logs will NOT "
                      "be persisted this boot (live debug-UART telemetry via telemetry_log.c is "
                      "unaffected)", esp_err_to_name(err));
        return err;
    }
#else
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/logs",
        .partition_label = "logs",
        .max_files = 4, /* firing + autotune current-segment writers, plus a reader or two */
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_spiffs_register(logs) failed: %s -- firing/autotune logs will NOT "
                      "be persisted this boot (live debug-UART telemetry via telemetry_log.c is "
                      "unaffected)", esp_err_to_name(err));
        return err;
    }
#endif

    err = log_store_init("/logs");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "log_store_init(/logs) failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
#if CONFIG_KILNCTL_LOGS_LITTLEFS
    if (esp_littlefs_info("logs", &total, &used) == ESP_OK) {
#else
    if (esp_spiffs_info("logs", &total, &used) == ESP_OK) {
#endif
        ESP_LOGI(TAG, "logs partition mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "logs partition mounted");
    }

    s_mounted = true;
    return ESP_OK;
}

typedef struct {
    log_store_kind_t kind;
    const void       *data;
    size_t            len;
    esp_err_t         result;
} log_store_job_t;

static void log_store_job_run(void *arg)
{
    log_store_job_t *job = (log_store_job_t *)arg;
    job->result = log_store_append(job->kind, job->data, job->len);
}

/* Hands the append call to the internal-stack flash worker and blocks (from
 * the CALLER's task, e.g. telemetry_log_task) until it completes -- same
 * pattern safety_cfg_store.c's deferred NVS flush uses through this same
 * worker (uart_bridge_ext.c's own comment on
 * uart_bridge_ext_run_on_flash_worker()). `data` may point at the caller's
 * own stack buffer: the caller is blocked for the whole call, so that
 * storage stays valid throughout. */
esp_err_t log_store_write_event(log_store_kind_t kind, const void *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    log_store_job_t job = { .kind = kind, .data = data, .len = len, .result = ESP_ERR_INVALID_STATE };
    // Not reachable on-worker today; covered by bx_run_on_internal_stack()'s
    // generic backstop if that ever changes.
    esp_err_t dispatch_err = uart_bridge_ext_run_on_flash_worker(log_store_job_run, &job);
    if (dispatch_err != ESP_OK) {
        return dispatch_err;
    }
    return job.result;
}
