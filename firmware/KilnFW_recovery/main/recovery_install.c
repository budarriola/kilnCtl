// recovery_install.c -- see recovery_install.h.
#include "recovery_install.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "nvs.h"

#include "recovery_apply_esp.h"
#include "recovery_http.h"
#include "recovery_io.h"
#include "recovery_lcd.h"
#include "recovery_pico.h"

static const char *TAG = "rec_install";

#define KILN_NVS_PART "kiln_nvs"
#define POLL_TASK_STACK RECOVERY_INSTALL_POLL_STACK // shared with recovery_apply_esp.c (recovery_apply_esp.h)
#define POLL_TASK_PRIO 3
#define RECOVER_TASK_STACK 8192 // esp_image_verify runs here, same size as the apply task that also runs it
#define RECOVER_TASK_PRIO 4
#define RECOVER_RESTART_DELAY_MS 3000 // lets a status poll read the FAIL record before the retry reboot

// recovery_apply_result_t is carried as the result detail code: the numbers must stay in step.
_Static_assert((int)RECOVERY_APPLY_ERR_STAGE_HASH == (int)UPD_INST_DETAIL_APPLY_STAGE_HASH, "detail mirror");
_Static_assert((int)RECOVERY_APPLY_ERR_SET_BOOT == (int)UPD_INST_DETAIL_APPLY_SET_BOOT, "detail mirror");
_Static_assert((int)RECOVERY_APPLY_ERR_SHA == (int)UPD_INST_DETAIL_APPLY_LAST, "detail mirror");

static update_install_res_t s_last;
static bool s_have_last;
static char s_semver[UPD_INST_SEMVER_LEN];
static uint8_t s_id[UPD_INST_ID_LEN]; // sha256 prefix of the request being applied (result identity)

// ---- raw NVS ----

// 0 = read, 1 = absent, -1 = unavailable.
static int kv_read(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open_from_partition(KILN_NVS_PART, UPD_INST_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return 1; // namespace never created
    }
    if (e != ESP_OK) {
        return -1;
    }
    size_t n = cap;
    e = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return 1;
    }
    if (e != ESP_OK) {
        return -1;
    }
    *len = n;
    return 0;
}

static int kv_write(const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(KILN_NVS_PART, UPD_INST_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    int rc = -1;
    if (nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK) {
        uint8_t back[UPD_INST_REQ_SIZE];
        size_t n = sizeof(back);
        if (len <= sizeof(back) && nvs_get_blob(h, key, back, &n) == ESP_OK && n == len &&
            memcmp(back, buf, len) == 0) {
            rc = 0;
        }
    }
    nvs_close(h);
    return rc;
}

// Erase and read back that the key is gone.
static int kv_clear(const char *key)
{
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(KILN_NVS_PART, UPD_INST_NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t er = nvs_erase_key(h, key);
    int rc = (er == ESP_OK || er == ESP_ERR_NVS_NOT_FOUND) ? 0 : -1;
    if (rc == 0 && nvs_commit(h) != ESP_OK) {
        rc = -1;
    }
    if (rc == 0) {
        size_t n = 0;
        if (nvs_get_blob(h, key, NULL, &n) != ESP_ERR_NVS_NOT_FOUND) {
            rc = -1;
        }
    }
    nvs_close(h);
    return rc;
}

// ---- update_install_ops_t ----

static int op_req_read(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN) {
        // Request unknown: report "none" so the board stays in normal recovery with the AP usable,
        // instead of refusing nvs_unavailable on every boot with nothing ever cleared.
        return 1;
    }
    return kv_read(UPD_INST_KEY_REQ, buf, cap, len);
}

static int op_req_clear(void *ctx)
{
    (void)ctx;
    return kv_clear(UPD_INST_KEY_REQ);
}

static int op_req_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    return kv_write(UPD_INST_KEY_REQ, b, n);
}

static int op_res_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    return kv_write(UPD_INST_KEY_RES, b, n);
}

static bool op_stage_verified(void *ctx, uint32_t *len, uint8_t sha[UPD_INST_SHA_LEN])
{
    (void)ctx;
    stage_header_t hdr;
    if (recovery_apply_esp_stage_info(&hdr) != STAGE_HDR_OK || hdr.state != STAGE_STATE_VERIFIED) {
        return false;
    }
    _Static_assert(STAGE_SHA256_LEN == UPD_INST_SHA_LEN, "sha length");
    *len = hdr.image_length;
    memcpy(sha, hdr.sha256, UPD_INST_SHA_LEN);
    return true;
}

// Runs in the apply task after the copy verified, before the boot target switch:
// boot_guard clear only (a failure aborts the apply, boot target not changed). The
// request is removed and the OK result written by post_boot, only after the switch
// succeeded; a failed switch keeps the request for the bounded retry.
static const update_install_ops_t s_ops;

static int guard_clear(void)
{
    return recovery_http_boot_guard_clear();
}

static int pre_boot(void)
{
    if (update_install_pre_boot(&s_ops, guard_clear) != 0) {
        ESP_LOGE(TAG, "pre-boot clear failed; not selecting app");
        return -1;
    }
    return 0;
}

// Runs only after esp_ota_set_boot_partition succeeded: remove the request and record OK.
static int post_boot(void)
{
    bool ok = false;
    if (update_install_post_boot(&s_ops, s_semver, s_id, &ok) != 0) {
        ESP_LOGE(TAG, "request clear failed after the boot target was set");
        return -1;
    }
    if (!ok) {
        // Request already gone, so the install is good; the app just cannot report it.
        ESP_LOGW(TAG, "OK result write failed");
    }
    return 0;
}

// 0 = started, else a UPD_INST_DETAIL_START_* code (the heap floor refusal is START_NO_MEM, not a bare failure).
static int op_apply_start(void *ctx)
{
    (void)ctx;
    const esp_partition_t *app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (app == NULL) {
        return UPD_INST_DETAIL_START_NO_APP;
    }
    // AUDIT_UPDATE_FAULTS LOW-1: prove the boot_guard clear works BEFORE the first erase of `app` (the manual
    // route does the same); a clear that fails after the copy would leave `app` rewritten but never selected.
    if (guard_clear() != 0) {
        return UPD_INST_DETAIL_START_FAILED;
    }
    // LOW-2: `app` is about to change, so a verify cached by a status GET in the start window is stale.
    recovery_http_app_verify_invalidate();
    esp_err_t e = recovery_apply_esp_start(app, pre_boot, post_boot);
    if (e == ESP_OK) {
        return 0;
    }
    return e == ESP_ERR_NO_MEM ? UPD_INST_DETAIL_START_NO_MEM
         : e == ESP_ERR_INVALID_STATE ? UPD_INST_DETAIL_START_BUSY
                                      : UPD_INST_DETAIL_START_FAILED;
}

static const update_install_ops_t s_ops = {
    .ctx = NULL,
    .req_read = op_req_read,
    .req_clear = op_req_clear,
    .req_write = op_req_write,
    .res_write = op_res_write,
    .stage_verified = op_stage_verified,
    .apply_start = op_apply_start,
};

static void load_last(void)
{
    uint8_t buf[UPD_INST_RES_SIZE];
    size_t n = 0;
    s_have_last = kv_read(UPD_INST_KEY_RES, buf, sizeof(buf), &n) == 0 && update_install_res_decode(buf, n, &s_last);
    // MED-3: a last OK whose app image the bootloader aborted (never confirmed) is not an OK install.
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (s_have_last && bad && strcmp(bad->label, "app") == 0 && update_install_res_apply_rollback(&s_last, true)) {
        uint8_t nb[UPD_INST_RES_SIZE];
        if (update_install_res_encode(&s_last, nb) == UPD_INST_RES_SIZE) {
            (void)kv_write(UPD_INST_KEY_RES, nb, sizeof(nb)); // best effort; the in-memory view is already corrected
        }
    }
}

static void show_failure(uint8_t reason)
{
    // Warning, not error: the failure is shown but the SoftAP SSID, passphrase and QR stay on screen
    // (an error banner hides them, and the passphrase is shown nowhere else).
    char line[41]; // 40 columns on the 480 px panel at scale 2; the reason is the short LCD form
    snprintf(line, sizeof(line), "INSTALL FAILED: %s", update_install_reason_lcd(reason));
    recovery_lcd_set_warning(line, NULL);
}

// ---- leaving recovery after a failed install without a human ----

static struct {
    uint8_t reason;
    bool app_modified;
} s_recover;

// The request is still stored with attempts left (its counter for the failed attempt is already raised
// and read back, so a reboot advances the bounded retry).
static bool request_attempts_left(void)
{
    uint8_t buf[UPD_INST_REQ_SIZE];
    size_t n = 0;
    update_install_req_t rq;
    return kv_read(UPD_INST_KEY_REQ, buf, sizeof(buf), &n) == 0 && update_install_req_decode(buf, n, &rq) &&
           rq.attempts < UPD_INST_MAX_ATTEMPTS;
}

static void recover_task(void *arg)
{
    (void)arg;
    update_install_after_t act = UPD_INST_AFTER_STAY;
    // Kiln NVS unusable: boot_guard cannot be cleared or verified, so an app/recovery ping-pong could start.
    bool kiln_ok = (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN) == 0;
    if (kiln_ok && !recovery_pico_busy()) {
        bool verifies = recovery_http_app_verifies();
        act = update_install_after_fail(s_recover.reason, s_recover.app_modified, verifies, request_attempts_left());
    }
    // The verify took seconds: an operator apply or a Pico relay may have started meanwhile. Never switch or reset under it.
    if (recovery_apply_busy() || recovery_pico_busy()) {
        act = UPD_INST_AFTER_STAY;
    }
    if (act == UPD_INST_AFTER_EXIT_APP) {
        // Same order as /api/recovery/exit: boot_guard (verified) first, then drop the request (the FAIL
        // result stays for the application to read), then the boot target.
        if (recovery_http_boot_guard_clear() == 0 && kv_clear(UPD_INST_KEY_REQ) == 0 &&
            recovery_http_select_app_and_restart() == 0) {
            ESP_LOGW(TAG, "install failed (%s); app verified, booting it", update_install_reason_name(s_recover.reason));
        } else {
            ESP_LOGE(TAG, "auto-exit to the application failed; staying in recovery");
        }
    } else if (act == UPD_INST_AFTER_RESTART) {
        ESP_LOGW(TAG, "install failed with the app modified; restarting for the bounded retry");
        vTaskDelay(pdMS_TO_TICKS(RECOVER_RESTART_DELAY_MS));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "install failed (%s); staying in recovery", update_install_reason_name(s_recover.reason));
    }
    vTaskDelete(NULL);
}

// Starts the one-shot recovery task. A task create failure leaves the board in recovery, as before.
static void auto_recover(uint8_t reason, bool app_modified)
{
    s_recover.reason = reason;
    s_recover.app_modified = app_modified;
    if (xTaskCreate(recover_task, "rec_inst_exit", RECOVER_TASK_STACK, NULL, RECOVER_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "recover task create failed; staying in recovery");
    }
}

// Watches the apply. A failed apply does not reboot by itself: record the failure with its
// specific result and the app-modified flag, then leave recovery if that is safe (auto_recover).
static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        recovery_apply_progress_t p;
        recovery_apply_esp_status(&p);
        if (p.phase == RECOVERY_APPLY_FAILED) {
            update_install_record_result_ex(&s_ops, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED,
                                            (uint8_t)p.result, p.app_modified, s_semver, s_id);
            load_last();
            show_failure(UPD_INST_REASON_APPLY_FAILED);
            vTaskDelay(pdMS_TO_TICKS(300)); // the finished apply task releases its stack first
            auto_recover(UPD_INST_REASON_APPLY_FAILED, p.app_modified);
            break;
        }
        if (p.phase == RECOVERY_APPLY_DONE) {
            break; // pre_boot recorded OK; the apply task restarts the chip
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "poll task stack high-water free=%u B of %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL) * (unsigned)sizeof(StackType_t), POLL_TASK_STACK);
    vTaskDelete(NULL);
}

void recovery_install_boot(void)
{
    if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN) {
        // Warning only: no FAIL result is recorded and the passphrase stays visible.
        recovery_lcd_set_warning("INSTALL STATE UNREADABLE", NULL);
    }
    if (!(recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN)) {
        load_last();
        // Remember the requested version for the result records.
        uint8_t buf[UPD_INST_REQ_SIZE];
        size_t n = 0;
        update_install_req_t rq;
        if (kv_read(UPD_INST_KEY_REQ, buf, sizeof(buf), &n) == 0 && update_install_req_decode(buf, n, &rq)) {
            memcpy(s_semver, rq.semver, sizeof(s_semver) - 1);
            memcpy(s_id, rq.sha256, UPD_INST_ID_LEN);
        }
    }
    update_install_res_t res;
    memset(&res, 0, sizeof(res));
    update_install_boot_outcome_t o = update_install_boot_run(&s_ops, &res);
    if (o == UPD_INST_BOOT_REFUSED) {
        load_last();
        if (!s_have_last) {
            s_last = res;
            s_have_last = true;
        }
        show_failure(res.reason);
        ESP_LOGE(TAG, "install refused: %s", update_install_reason_name(res.reason));
        // attempts_exceeded: earlier attempts may have rewritten app, so its state is unknown (treat as modified).
        auto_recover(res.reason, res.app_modified || res.reason == UPD_INST_REASON_ATTEMPTS_EXCEEDED);
    } else if (o == UPD_INST_BOOT_STARTED) {
        ESP_LOGW(TAG, "install request found; apply started");
        if (xTaskCreate(poll_task, "rec_inst_poll", POLL_TASK_STACK, NULL, POLL_TASK_PRIO, NULL) != pdPASS) {
            // Nobody would record an apply failure: surface it now (status + LCD), not only in the log.
            ESP_LOGE(TAG, "poll task create failed");
            // The apply task is already running and usually succeeds, so this is NOT "apply failed":
            // record that the outcome is unknown and tell the operator not to power-cycle.
            update_install_record_result(&s_ops, UPD_INST_RES_FAIL, UPD_INST_REASON_OUTCOME_UNKNOWN, s_semver, s_id);
            load_last();
            if (!s_have_last) {
                memset(&s_last, 0, sizeof(s_last));
                s_last.code = UPD_INST_RES_FAIL;
                s_last.reason = UPD_INST_REASON_OUTCOME_UNKNOWN;
                s_have_last = true;
            }
            // Warning only: keep the SoftAP SSID and passphrase on screen. The LCD task drops it when the apply ends.
            recovery_lcd_set_warning("DO NOT POWER-CYCLE", recovery_apply_busy);
        }
    }
}

bool recovery_install_last_result(update_install_res_t *out)
{
    if (!s_have_last) {
        return false;
    }
    *out = s_last;
    return true;
}

// A request/result left from an earlier recovery entry must not outlive an operator action
// (recovery_exit, manual apply). Drops both, so no later entry re-applies and status shows none.
int recovery_install_supersede(void)
{
    int rq = kv_clear(UPD_INST_KEY_REQ);
    int rs = kv_clear(UPD_INST_KEY_RES);
    if (rs == 0) {
        s_have_last = false;
    }
    if (rq != 0) {
        ESP_LOGE(TAG, "could not clear leftover install request");
    }
    return rq;
}
