// recovery_apply_esp.c -- see recovery_apply_esp.h. Target-build only (no host
// test: the logic under test is recovery_apply.c; this file only adapts
// esp_partition / PSA / esp_image_verify to its callbacks).
#include "recovery_apply_esp.h"

#include <string.h>

#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "recovery_boot_verify.h"

static const char *TAG = "recovery_apply";

#define APPLY_TASK_STACK 8192
#define APPLY_TASK_PRIO 4
#define STAGE_LABEL "stage"

typedef struct {
    const esp_partition_t *stage;
    const esp_partition_t *app;
    int (*pre_boot)(void);
    psa_hash_operation_t sha;
    bool sha_active;
} apply_ctx_t;

// Statics, not the task stack or heap: nothing here may fail to allocate after
// the apply has begun, and the recovery image has the room (the application's
// static-RAM budget does not apply to this image).
static apply_ctx_t s_ctx;
static recovery_apply_progress_t s_prog;
static uint8_t s_scratch[RECOVERY_APPLY_CHUNK];
static volatile bool s_running; // set before the task exists, cleared never (restart follows)

static int cb_stage_read(void *c, uint32_t off, void *buf, size_t len)
{
    return esp_partition_read(((apply_ctx_t *)c)->stage, off, buf, len) == ESP_OK ? 0 : -1;
}
static int cb_stage_erase(void *c, uint32_t off, uint32_t len)
{
    return esp_partition_erase_range(((apply_ctx_t *)c)->stage, off, len) == ESP_OK ? 0 : -1;
}
static int cb_app_read(void *c, uint32_t off, void *buf, size_t len)
{
    return esp_partition_read(((apply_ctx_t *)c)->app, off, buf, len) == ESP_OK ? 0 : -1;
}
static int cb_app_erase(void *c, uint32_t off, uint32_t len)
{
    return esp_partition_erase_range(((apply_ctx_t *)c)->app, off, len) == ESP_OK ? 0 : -1;
}
static int cb_app_write(void *c, uint32_t off, const void *buf, size_t len)
{
    return esp_partition_write(((apply_ctx_t *)c)->app, off, buf, len) == ESP_OK ? 0 : -1;
}

static int cb_sha_start(void *c)
{
    apply_ctx_t *a = c;
    if (a->sha_active) {
        psa_hash_abort(&a->sha);
        a->sha_active = false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return -1;
    }
    a->sha = psa_hash_operation_init();
    if (psa_hash_setup(&a->sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return -1;
    }
    a->sha_active = true;
    return 0;
}
static int cb_sha_update(void *c, const void *buf, size_t len)
{
    apply_ctx_t *a = c;
    return (a->sha_active && psa_hash_update(&a->sha, buf, len) == PSA_SUCCESS) ? 0 : -1;
}
static int cb_sha_finish(void *c, uint8_t out[RECOVERY_APPLY_SHA_LEN])
{
    apply_ctx_t *a = c;
    if (!a->sha_active) {
        return -1;
    }
    size_t n = 0;
    psa_status_t st = psa_hash_finish(&a->sha, out, RECOVERY_APPLY_SHA_LEN, &n);
    a->sha_active = false;
    if (st != PSA_SUCCESS) {
        psa_hash_abort(&a->sha);
        return -1;
    }
    return n == RECOVERY_APPLY_SHA_LEN ? 0 : -1;
}
static void cb_sha_abort(void *c)
{
    apply_ctx_t *a = c;
    if (a->sha_active) {
        psa_hash_abort(&a->sha);
        a->sha_active = false;
    }
}

// esp_image_verify over the copy: image header, every segment, the appended
// checksum and (when enabled) the SHA-256 digest. The image must also end
// within the length that was staged.
static int cb_app_verify(void *c, uint32_t image_len)
{
    apply_ctx_t *a = c;
    const esp_partition_pos_t pos = {.offset = a->app->address, .size = a->app->size};
    esp_image_metadata_t meta;
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        return -1;
    }
    return meta.image_len <= image_len ? 0 : -1;
}

static int cb_set_boot(void *c)
{
    apply_ctx_t *a = c;
    if (a->pre_boot && a->pre_boot() != 0) {
        ESP_LOGE(TAG, "pre-boot hook (boot_guard clear) failed; boot partition NOT changed");
        return -1;
    }
    return recovery_boot_partition_set_and_verify(a->app) == ESP_OK ? 0 : -1;
}

static void cb_yield(void *c)
{
    (void)c;
    vTaskDelay(1); // let the idle task (and its watchdog) run during long hash/copy loops
}

static void apply_task(void *arg)
{
    (void)arg;
    const recovery_apply_io_t io = {
        .ctx = &s_ctx,
        .stage_size = s_ctx.stage->size,
        .app_size = s_ctx.app->size,
        .stage_read = cb_stage_read,
        .stage_erase = cb_stage_erase,
        .app_read = cb_app_read,
        .app_erase = cb_app_erase,
        .app_write = cb_app_write,
        .sha_start = cb_sha_start,
        .sha_update = cb_sha_update,
        .sha_finish = cb_sha_finish,
        .sha_abort = cb_sha_abort,
        .app_verify = cb_app_verify,
        .set_boot = cb_set_boot,
        .yield = cb_yield,
    };
    ESP_LOGW(TAG, "apply staged update: stage -> app (%u bytes of app partition)", (unsigned)io.app_size);
    recovery_apply_result_t r = recovery_apply_run(&io, s_scratch, sizeof(s_scratch), &s_prog);
    if (r == RECOVERY_APPLY_OK) {
        ESP_LOGW(TAG, "apply ok (stage header %s); restarting into the application",
                 s_prog.stage_cleared ? "erased" : "NOT erased");
        vTaskDelay(pdMS_TO_TICKS(1500)); // let a status poll read "done"
        esp_restart();
    }
    ESP_LOGE(TAG, "apply FAILED: %s (app %s)", recovery_apply_result_name(r),
             s_prog.app_modified ? "was modified: stays in recovery, apply again" : "untouched");
    // Failure: stay in recovery with the stage intact. Allow a retry.
    s_running = false;
    vTaskDelete(NULL);
}

static const esp_partition_t *find_stage(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, STAGE_LABEL);
}

esp_err_t recovery_apply_esp_start(const esp_partition_t *app, int (*pre_boot)(void))
{
    if (!app) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *stage = find_stage();
    if (!stage) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.stage = stage;
    s_ctx.app = app;
    s_ctx.pre_boot = pre_boot;
    s_prog.phase = RECOVERY_APPLY_CHECKING;
    s_prog.result = 0;
    s_prog.done_bytes = 0;
    s_prog.total_bytes = 0;
    s_prog.app_modified = false;
    s_prog.stage_cleared = false;
    if (xTaskCreate(apply_task, "rec_apply", APPLY_TASK_STACK, NULL, APPLY_TASK_PRIO, NULL) != pdPASS) {
        s_prog.phase = RECOVERY_APPLY_IDLE;
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool recovery_apply_busy(void)
{
    return s_running;
}

void recovery_apply_esp_status(recovery_apply_progress_t *out)
{
    out->phase = s_prog.phase;
    out->result = s_prog.result;
    out->done_bytes = s_prog.done_bytes;
    out->total_bytes = s_prog.total_bytes;
    out->app_modified = s_prog.app_modified;
    out->stage_cleared = s_prog.stage_cleared;
}

stage_hdr_status_t recovery_apply_esp_stage_info(stage_header_t *out)
{
    const esp_partition_t *stage = find_stage();
    uint8_t hdr[STAGE_HEADER_SIZE];
    if (!stage || stage->size < STAGE_IMAGE_OFFSET + RECOVERY_APPLY_CHUNK ||
        esp_partition_read(stage, 0, hdr, sizeof(hdr)) != ESP_OK) {
        return STAGE_HDR_BAD_ARG;
    }
    return stage_header_decode(hdr, sizeof(hdr), (stage->size - STAGE_IMAGE_OFFSET) & ~(RECOVERY_APPLY_CHUNK - 1u),
                               out);
}
