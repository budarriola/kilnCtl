#include "dualwrite_window.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() */
#include "nvs_key_check.h"

#include "cfg_fs.h"        /* cfg_fs_is_available() */
#include "crash_report.h"  /* crash_report_get() */

static const char *TAG = "dualwrite_window";

/* Own key, same "kiln_cfg" namespace / "kiln_nvs" partition as run_state.c/
 * crash_report.c -- see this header's top comment for why plain NVS and not
 * cfg_fs. A corrupt/rejected record here must never be able to take another
 * module's breadcrumb down with it, and vice versa -- same reasoning
 * run_state.c/crash_report.c already give for their own separate keys. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_DWWIN "dwwin"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_DWWIN);

#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

/* Pins the on-disk layout -- see run_state.c's identical trick and comment.
 * A silent layout change here would look like corruption to the loader
 * instead of failing the build. */
_Static_assert(sizeof(dualwrite_window_record_t) == 16,
               "dualwrite_window_record_t layout changed -- bump DUALWRITE_WINDOW_RECORD_VERSION");

typedef struct {
    SemaphoreHandle_t lock;
    bool checked_this_boot; /* dualwrite_window_boot_check() no-ops after the first call */
} dualwrite_window_ctx_t;

static dualwrite_window_ctx_t s_dw;

static esp_err_t nvs_partition_init(void)
{
    return hal_status_to_esp_err(hal_kv_init_partition(KILN_NVS_PARTITION));
}

static bool ensure_lock(void)
{
    if (!s_dw.lock) {
        s_dw.lock = xSemaphoreCreateMutex();
        if (!s_dw.lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- dual-write window will not be measured this boot");
            return false;
        }
    }
    return true;
}

/* Same write-context guard run_state.c's persist_locked() carries: a flash/
 * NVS write from a task whose stack lives in PSRAM aborts the whole board.
 * See DRAM_PSRAM_PLAN.md / run_state.c's own comment for the full story. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

static void record_set_defaults(dualwrite_window_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->version = DUALWRITE_WINDOW_RECORD_VERSION;
}

/* Must be called with s_dw.lock held (or before the lock exists, e.g. during
 * single-threaded early init -- matches run_state.c's own convention). */
static bool load_locked(dualwrite_window_record_t *out)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        record_set_defaults(out);
        return true;
    }
    dualwrite_window_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_DWWIN, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec) || rec.version != DUALWRITE_WINDOW_RECORD_VERSION) {
        /* Nothing there yet, wrong size, or a version this build does not
         * recognize -- same load-tolerant convention as run_state.c/
         * crash_report.c: restart the count rather than fail bring-up or
         * mis-parse an old layout into a confidently wrong answer. */
        record_set_defaults(out);
        return true;
    }
    *out = rec;
    return true;
}

/* Must be called with s_dw.lock held. */
static esp_err_t persist_locked(const dualwrite_window_record_t *rec)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "persist_locked: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). See DRAM_PSRAM_PLAN.md section 7.2 and run_state.c's identical "
                      "guard for why this write must come from an internal-SRAM stack.");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t pinit = nvs_partition_init();
    if (pinit != ESP_OK) {
        return pinit;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_DWWIN, rec, sizeof(*rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

/* ---------------------------------------------------------------------
 * Pure logic -- see header for the negative-testing rationale.
 * ------------------------------------------------------------------- */

bool dualwrite_window_boot_is_clean(hal_reset_reason_t reason, bool crash_pending, bool fs_available)
{
    if (crash_pending) {
        return false;
    }
    if (!fs_available) {
        return false;
    }
    return reason == HAL_RESET_POWERON || reason == HAL_RESET_SW;
}

void dualwrite_window_apply_boot(dualwrite_window_record_t *rec, bool clean)
{
    if (clean) {
        if (rec->consecutive_clean_boots < UINT32_MAX) {
            rec->consecutive_clean_boots++;
        }
    } else {
        /* THE reset -- see this module's header top comment and the
         * project's "reset one side of a pair" bug class. The only thing
         * derived from consecutive_clean_boots is window_may_close, and
         * that is never stored (dualwrite_window_compute_status() always
         * recomputes it from this same field), so there is no second copy
         * anywhere for this reset to leave behind. */
        rec->consecutive_clean_boots = 0;
    }
}

void dualwrite_window_compute_status(const dualwrite_window_record_t *rec, dualwrite_window_status_t *out)
{
    out->consecutive_clean_boots = rec->consecutive_clean_boots;
    out->clean_boots_target = DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET;
    out->firing_complete = rec->firing_complete != 0;
    out->restore_verified = rec->restore_verified != 0;
    out->window_may_close = (rec->consecutive_clean_boots >= DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET) &&
                             out->firing_complete && out->restore_verified;
}

/* ---------------------------------------------------------------------
 * Stateful API.
 * ------------------------------------------------------------------- */

void dualwrite_window_boot_check(void)
{
    if (!ensure_lock()) {
        return;
    }
    if (xSemaphoreTake(s_dw.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_dw.checked_this_boot) {
        xSemaphoreGive(s_dw.lock);
        return;
    }
    s_dw.checked_this_boot = true;

    hal_reset_reason_t reason = hal_sysinfo_reset_reason();
    crash_report_record_t crash;
    bool crash_pending = crash_report_get(&crash) && !crash.acknowledged;
    bool fs_available = cfg_fs_is_available();

    bool clean = dualwrite_window_boot_is_clean(reason, crash_pending, fs_available);

    dualwrite_window_record_t rec;
    load_locked(&rec);
    dualwrite_window_apply_boot(&rec, clean);
    esp_err_t err = persist_locked(&rec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "boot_check: persist failed: %s (clean=%d, consecutive_clean_boots now %u -- "
                      "NOT durable this boot)", esp_err_to_name(err), (int)clean, (unsigned)rec.consecutive_clean_boots);
    } else {
        ESP_LOGI(TAG, "boot_check: clean=%d reason=%d crash_pending=%d fs_available=%d -- "
                      "consecutive_clean_boots=%u/%u", (int)clean, (int)reason, (int)crash_pending,
                 (int)fs_available, (unsigned)rec.consecutive_clean_boots, (unsigned)DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET);
    }
    xSemaphoreGive(s_dw.lock);
}

void dualwrite_window_note_mount_failure(void)
{
    if (!ensure_lock()) {
        return;
    }
    if (xSemaphoreTake(s_dw.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    dualwrite_window_record_t rec;
    load_locked(&rec);
    dualwrite_window_apply_boot(&rec, false);
    esp_err_t err = persist_locked(&rec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "note_mount_failure: persist failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "note_mount_failure: consecutive_clean_boots reset to 0");
    }
    xSemaphoreGive(s_dw.lock);
}

void dualwrite_window_note_firing_complete(void)
{
    if (!ensure_lock()) {
        return;
    }
    if (xSemaphoreTake(s_dw.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    dualwrite_window_record_t rec;
    load_locked(&rec);
    if (!rec.firing_complete) {
        rec.firing_complete = 1;
        esp_err_t err = persist_locked(&rec);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "note_firing_complete: persist failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "note_firing_complete: recorded (sticky)");
        }
    }
    xSemaphoreGive(s_dw.lock);
}

void dualwrite_window_note_restore_verified(void)
{
    if (!ensure_lock()) {
        return;
    }
    if (xSemaphoreTake(s_dw.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    dualwrite_window_record_t rec;
    load_locked(&rec);
    if (!rec.restore_verified) {
        rec.restore_verified = 1;
        esp_err_t err = persist_locked(&rec);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "note_restore_verified: persist failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "note_restore_verified: recorded (sticky)");
        }
    }
    xSemaphoreGive(s_dw.lock);
}

bool dualwrite_window_get_status(dualwrite_window_status_t *out)
{
    if (!out) {
        return false;
    }
    dualwrite_window_record_t rec;
    if (ensure_lock() && xSemaphoreTake(s_dw.lock, portMAX_DELAY) == pdTRUE) {
        load_locked(&rec);
        xSemaphoreGive(s_dw.lock);
    } else {
        load_locked(&rec);
    }
    dualwrite_window_compute_status(&rec, out);
    return true;
}
