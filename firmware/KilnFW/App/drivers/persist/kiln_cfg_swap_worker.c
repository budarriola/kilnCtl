#include "kiln_cfg_swap_worker.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "stack_margin.h"

static const char *TAG = "kiln_cfg_swap_worker";

/* 8192 B. Sized against kiln_cfg_swap_apply()'s own frame (a stack-allocated
 * kiln_cfg_swap_pending_t is ~1.7 kB by itself: ZONES_CONFIG_BLOB_MAX_SIZE
 * 896 B plus a kiln_pkg_safety_t) plus the zones-config import/export and
 * safety-link call chains beneath it, and deliberately matched to the
 * httpd_worker's own 8192 B rather than trimmed: this task runs the single
 * deepest non-httpd call chain this feature has, and the whole point of
 * moving it off the httpd worker was that 8192 B shared with every page
 * handler was not enough room for it. Must match the
 * stack_margin_register() literal below exactly (stack_margin.h's own doc
 * comment on why that number is never assumed equal to another task's). */
#define SWAP_WORKER_STACK_BYTES 8192

typedef struct {
    int32_t target_id;
    bool ack_no_safety_processor;
} swap_job_t;

static TaskHandle_t s_task;         /* stack_margin_register() target */
static QueueHandle_t s_queue;       /* depth 1 -- see the header's CONCURRENCY note */
static SemaphoreHandle_t s_lock;    /* guards the s_state/s_* publication below ONLY */

static kiln_cfg_swap_job_state_t s_state = KILN_CFG_SWAP_JOB_IDLE;
static int32_t s_target_id = -1;
static bool s_diverged;
static char s_reason[KILN_CFG_SWAP_REASON_MAX];

static void publish(kiln_cfg_swap_job_state_t state, int32_t target_id, bool diverged, const char *reason)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    s_target_id = target_id;
    s_diverged = diverged;
    if (reason) {
        snprintf(s_reason, sizeof(s_reason), "%s", reason);
    } else {
        s_reason[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

static void swap_worker_task(void *arg)
{
    (void)arg;

    /* Crash recovery (plan section 4.4) runs HERE, as this task's first act,
     * not inline in main_network_http_bringup().
     *
     * Two independent reasons, either one sufficient:
     *
     * 1. Stack. Called from the `main` task it put a 4928 B frame plus the
     *    ~3.2 kB safety-link push chain beneath it on main's 8192 B stack --
     *    8192 B against a 6144 B budget, measured by
     *    check_main_task_stack_budget.ps1. That is not a clean crash: it
     *    smashes the return address and the board takes an
     *    IllegalInstruction panic at boot with a corrupted backtrace
     *    (docs/audits/boot_hang_2026-09-08.md). The same audit's rule is to
     *    move the locals off the stack, which kiln_cfg_swap_boot_recover()
     *    now also does -- this placement is the second half of that fix.
     *
     * 2. Duration. Recovery re-pushes a full config to the Pico and verifies
     *    it, which is a multi-second-to-minutes UART exchange. Running that
     *    inline in boot bringup stalls every later bringup step behind it,
     *    including the HTTP server that is the operator's only way to SEE
     *    that recovery is happening.
     *
     * Ordering is safe: kiln_cfg_swap_set_link() is called from
     * main_control_bringup(), which runs before this task is created, so the
     * link is already published by the time this line executes. A submit()
     * that arrives during recovery simply waits in the depth-1 queue rather
     * than racing it. */
    kiln_cfg_swap_boot_recover();

    for (;;) {
        swap_job_t job;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Everything below runs with NO module lock held. kiln_cfg_swap_
         * apply() blocks for minutes on the safety-link UART and takes
         * kiln_cfg_store's own lock internally; holding s_lock across it
         * would be exactly the "module lock held across a producer/blocking
         * call" mistake this repo has already paid for once
         * (project_screen_idle_brick_real_cause). The only shared state this
         * task touches is published through publish(), which takes the lock
         * for the duration of a handful of assignments and nothing else. */
        char reason[KILN_CFG_SWAP_REASON_MAX];
        reason[0] = '\0';
        bool diverged = false;
        ESP_LOGI(TAG, "kiln config swap starting: target_id=%ld", (long)job.target_id);
        bool ok = kiln_cfg_swap_apply(job.target_id, job.ack_no_safety_processor, reason, sizeof(reason),
                                      &diverged);
        if (ok) {
            ESP_LOGI(TAG, "kiln config swap succeeded: target_id=%ld", (long)job.target_id);
            publish(KILN_CFG_SWAP_JOB_DONE_OK, job.target_id, false, NULL);
        } else {
            /* ESP_LOGE, not ESP_LOGW, for the diverged case specifically:
             * that is the outcome where the swap partly landed and the
             * board is now alarmed with heaters disabled, not merely
             * refused with nothing changed. */
            if (diverged) {
                ESP_LOGE(TAG, "kiln config swap target_id=%ld left the board DIVERGED (heaters disabled): %s",
                         (long)job.target_id, reason);
            } else {
                ESP_LOGW(TAG, "kiln config swap target_id=%ld refused/failed, nothing changed: %s",
                         (long)job.target_id, reason);
            }
            publish(KILN_CFG_SWAP_JOB_DONE_FAILED, job.target_id, diverged, reason);
        }
    }
}

esp_err_t kiln_cfg_swap_worker_start(void)
{
    if (s_task) {
        return ESP_OK; /* idempotent -- see the header */
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "mutex alloc failed -- kiln config apply unavailable this boot");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_queue) {
        s_queue = xQueueCreate(1, sizeof(swap_job_t));
        if (!s_queue) {
            ESP_LOGE(TAG, "queue alloc failed -- kiln config apply unavailable this boot");
            return ESP_ERR_NO_MEM;
        }
    }
    /* tskIDLE_PRIORITY + 1: this task must never outrank the control or
     * safety-poll tasks (priority 5) -- it does slow, non-time-critical
     * bookkeeping, and the whole transaction is refused outright by its own
     * interlock check if a firing is running. Same priority and the same
     * reasoning as ota_pico_rollback's own worker. */
    if (xTaskCreate(swap_worker_task, "kiln_cfg_swap", SWAP_WORKER_STACK_BYTES, NULL, tskIDLE_PRIORITY + 1,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(kiln_cfg_swap) failed -- kiln config apply unavailable this boot");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Must match the SWAP_WORKER_STACK_BYTES literal passed to xTaskCreate()
     * immediately above. Registered on the success path only, matching every
     * other call site in this codebase (creation failure already returned). */
    stack_margin_register("kiln_cfg_swap", &s_task, SWAP_WORKER_STACK_BYTES);
    ESP_LOGI(TAG, "kiln config swap worker up (stack=%u B)", (unsigned)SWAP_WORKER_STACK_BYTES);
    return ESP_OK;
}

bool kiln_cfg_swap_worker_submit(int32_t target_id, bool ack_no_safety_processor, char *reason_out,
                                 size_t reason_cap)
{
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    if (!s_task || !s_queue) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap,
                     "the kiln config apply worker did not start this boot -- applying a saved config "
                     "is unavailable until the controller is restarted");
        }
        return false;
    }
    if (kiln_cfg_swap_worker_is_busy()) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap,
                     "another kiln config is already being applied -- wait for it to finish");
        }
        return false;
    }

    swap_job_t job = { .target_id = target_id, .ack_no_safety_processor = ack_no_safety_processor };
    /* Publish RUNNING BEFORE queueing, not after: the worker can dequeue and
     * even finish between xQueueSend() returning and this thread running
     * again, and publishing afterward would then overwrite a completed
     * job's real outcome with a stale RUNNING that never clears. Ordering it
     * this way can only ever be wrong in the harmless direction (a job that
     * fails to queue is corrected to DONE_FAILED immediately below). */
    publish(KILN_CFG_SWAP_JOB_RUNNING, target_id, false, NULL);
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        /* Depth-1 queue already full: another submit won the race between
         * the is_busy() check above and here. */
        const char *msg = "another kiln config is already being applied -- wait for it to finish";
        publish(KILN_CFG_SWAP_JOB_DONE_FAILED, target_id, false, msg);
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "%s", msg);
        }
        return false;
    }
    return true;
}

void kiln_cfg_swap_worker_get_status(kiln_cfg_swap_job_state_t *out_state, int32_t *out_target_id,
                                     bool *out_diverged, char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    if (!s_lock) {
        /* Never started: report the same IDLE a fresh boot reports rather
         * than inventing a fourth state nothing else knows how to render. */
        if (out_state) {
            *out_state = KILN_CFG_SWAP_JOB_IDLE;
        }
        if (out_target_id) {
            *out_target_id = -1;
        }
        if (out_diverged) {
            *out_diverged = false;
        }
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_state) {
        *out_state = s_state;
    }
    if (out_target_id) {
        *out_target_id = s_target_id;
    }
    if (out_diverged) {
        *out_diverged = s_diverged;
    }
    if (reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap, "%s", s_reason);
    }
    xSemaphoreGive(s_lock);
}

bool kiln_cfg_swap_worker_is_busy(void)
{
    kiln_cfg_swap_job_state_t state = KILN_CFG_SWAP_JOB_IDLE;
    kiln_cfg_swap_worker_get_status(&state, NULL, NULL, NULL, 0);
    return state == KILN_CFG_SWAP_JOB_RUNNING;
}
