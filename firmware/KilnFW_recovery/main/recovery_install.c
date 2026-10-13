// recovery_install.c -- see recovery_install.h.
#include "recovery_install.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "recovery_apply_esp.h"
#include "recovery_http.h"
#include "recovery_io.h"
#include "recovery_lcd.h"

static const char *TAG = "rec_install";

#define KILN_NVS_PART "kiln_nvs"
#define POLL_TASK_STACK 3072 // internal RAM: this task writes NVS
#define POLL_TASK_PRIO 3

static update_install_res_t s_last;
static bool s_have_last;
static char s_semver[UPD_INST_SEMVER_LEN];

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
        return -1;
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
// boot_guard clear, then the request is removed (read back) and the OK result is
// written. A failure before the request is gone aborts the apply (boot target
// not changed, request kept for the bounded retry).
static int pre_boot(void)
{
    if (recovery_http_boot_guard_clear() != 0) {
        return -1;
    }
    if (kv_clear(UPD_INST_KEY_REQ) != 0) {
        ESP_LOGE(TAG, "could not clear the install request; not selecting app");
        return -1;
    }
    update_install_res_t r;
    memset(&r, 0, sizeof(r));
    r.code = UPD_INST_RES_OK;
    r.reason = UPD_INST_REASON_NONE;
    strncpy(r.semver, s_semver, sizeof(r.semver) - 1);
    uint8_t blob[UPD_INST_RES_SIZE];
    size_t n = update_install_res_encode(&r, blob);
    if (n == 0 || kv_write(UPD_INST_KEY_RES, blob, n) != 0) {
        // The request is already gone, so the install itself is good; the app
        // just cannot report it. Do not block the boot target switch.
        ESP_LOGW(TAG, "OK result write failed");
    }
    return 0;
}

static int op_apply_start(void *ctx)
{
    (void)ctx;
    const esp_partition_t *app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (app == NULL) {
        return -1;
    }
    return recovery_apply_esp_start(app, pre_boot) == ESP_OK ? 0 : -1;
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
}

static void show_failure(uint8_t reason)
{
    recovery_lcd_set_error("INSTALL FAILED", update_install_reason_name(reason));
}

// Watches the apply. A failed apply does not reboot, so record the failure (the
// request stays for a bounded retry on the next recovery boot) and exit.
static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        recovery_apply_progress_t p;
        recovery_apply_esp_status(&p);
        if (p.phase == RECOVERY_APPLY_FAILED) {
            update_install_record_result(&s_ops, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED, s_semver);
            load_last();
            show_failure(UPD_INST_REASON_APPLY_FAILED);
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
    if (!(recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN)) {
        load_last();
        // Remember the requested version for the result records.
        uint8_t buf[UPD_INST_REQ_SIZE];
        size_t n = 0;
        update_install_req_t rq;
        if (kv_read(UPD_INST_KEY_REQ, buf, sizeof(buf), &n) == 0 && update_install_req_decode(buf, n, &rq)) {
            memcpy(s_semver, rq.semver, sizeof(s_semver) - 1);
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
    } else if (o == UPD_INST_BOOT_STARTED) {
        ESP_LOGW(TAG, "install request found; apply started");
        if (xTaskCreate(poll_task, "rec_inst_poll", POLL_TASK_STACK, NULL, POLL_TASK_PRIO, NULL) != pdPASS) {
            ESP_LOGE(TAG, "poll task create failed");
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
