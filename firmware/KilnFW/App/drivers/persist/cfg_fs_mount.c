#include "cfg_fs_mount.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_littlefs.h"

#include "cfg_fs.h"
#include "boot_guard.h"
#include "flash_worker.h"

static const char *TAG = "cfg_fs";

esp_err_t cfg_fs_mount_device(void)
{
    if (cfg_fs_get_status() == CFG_FS_STATUS_MOUNTED) {
        return ESP_OK; /* idempotent, same convention as log_store_mount() */
    }

    bool recovery = boot_guard_is_recovery_mode();
    if (recovery) {
        ESP_LOGI(TAG, "recovery mode: skipping cfg filesystem mount entirely");
        return cfg_fs_mount_or_skip(true, "/cfg", NULL);
    }

    /* format_if_mount_failed = FALSE, deliberately the opposite of
     * log_store_mount()'s `logs` policy -- see cfg_fs.h's file banner.
     * Losing user tuning/profile data to a silent reformat is the worst
     * possible outcome here; a corrupt or absent `cfg` partition is
     * reported and left alone, and every caller degrades to firmware
     * defaults instead. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/cfg",
        .partition_label = "cfg",
        .partition = NULL,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        /* Today's real, expected state on every board: partitions.csv does
         * not have a `cfg` partition entry yet (owner-gated separate step,
         * docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 1 -- adding it
         * forces an otadata erase + bootloader reflash). This is NOT a
         * defect in this code path; it is the mount-failure contract doing
         * exactly what it is supposed to do: report loudly, never format,
         * never block boot. Every cfg_fs_*() call fails clean with
         * ESP_ERR_INVALID_STATE from here on this boot. */
        ESP_LOGE(TAG, "esp_vfs_littlefs_register(cfg) failed: %s -- config filesystem UNAVAILABLE this "
                      "boot, all callers fall back to firmware defaults (banner required per "
                      "docs/FILESYSTEM_USER_DATA_PLAN.md's mount-failure contract)", esp_err_to_name(err));
        return err;
    }

    size_t tmp_reaped = 0;
    esp_err_t init_err = cfg_fs_mount_or_skip(false, "/cfg", &tmp_reaped);
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "cfg_fs_init(/cfg) failed: %s", esp_err_to_name(init_err));
        return init_err;
    }

    if (tmp_reaped > 0) {
        ESP_LOGW(TAG, "cfg filesystem mounted: swept %u stale temp file(s) from a prior interrupted write",
                 (unsigned)tmp_reaped);
    } else {
        ESP_LOGI(TAG, "cfg filesystem mounted, no stale temp files");
    }

    return ESP_OK;
}

typedef struct {
    const char *rel_path;
    const void *data;
    size_t      len;
    esp_err_t   result;
} cfg_fs_write_job_t;

static void cfg_fs_write_job_run(void *arg)
{
    cfg_fs_write_job_t *job = (cfg_fs_write_job_t *)arg;
    job->result = cfg_fs_write_atomic(job->rel_path, job->data, job->len);
}

esp_err_t cfg_fs_write_atomic_device(const char *rel_path, const void *data, size_t len)
{
    if (!rel_path) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg_fs_write_job_t job = { .rel_path = rel_path, .data = data, .len = len, .result = ESP_ERR_INVALID_STATE };
    esp_err_t dispatch_err = uart_bridge_ext_run_on_flash_worker(cfg_fs_write_job_run, &job);
    if (dispatch_err != ESP_OK) {
        return dispatch_err;
    }
    return job.result;
}
