#include "http_async_job.h"

#include <stddef.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "stack_margin.h"

static const char *TAG = "http_async_job";

// Single in-flight job across every caller of this helper (see this module's
// header doc comment) -- protected by a short critical section, not a
// mutex: every window s_mux guards here is a handful of instructions, never
// a blocking wait, so a spinlock-style critical section is enough and never
// itself blocks a second caller for longer than the first spends flipping
// the flag.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy = false;

// stack_margin_register() target -- file-scope, not call-scoped, same
// convention as ota_http_esp.c's s_ota_rollback_reboot_task: the task is
// short-lived and self-deletes, and stack_margin_read() treats a non-NULL
// handle as "alive", so http_async_job_task() nulls it before deleting
// itself (see that function's own comment for why a task function must not
// simply return).
static TaskHandle_t s_task_handle;

typedef struct {
    http_async_job_fn_t fn;
    void *ctx;
    httpd_req_t *async_req;
} http_async_job_run_ctx_t;

// Only one job runs at a time (s_busy above enforces that before this is
// ever written), so one file-scope slot is enough -- no allocation needed
// for the run context itself.
static http_async_job_run_ctx_t s_run_ctx;

bool http_async_job_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool busy = s_busy;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

// The actual job body -- separated from the FreeRTOS task trampoline
// (http_async_job_task() below) so host tests can call it directly without
// depending on xTaskCreate() ever invoking the function it was given (the
// host stub does not -- see App/test/stubs/freertos/task.h's own comment on
// xTaskCreate()). Always ends with httpd_req_async_handler_complete(), on
// EVERY path, including whatever fn itself did -- fn must never call it
// itself (http_async_job.h's doc comment).
static void run_job(http_async_job_run_ctx_t *rc)
{
    rc->fn(rc->async_req, rc->ctx);
    httpd_req_async_handler_complete(rc->async_req);

    // s_task_handle is cleared in the SAME critical section as s_busy --
    // 2026-09-25 fix-then-push review found that clearing them separately
    // (this task nulling the handle only after run_job() already dropped
    // s_busy) let a newly-admitted job's xTaskCreate() write s_task_handle
    // before this trailing cleanup ran, and this cleanup would then null out
    // the NEW job's handle instead of its own -- "reset one side of a pair"
    // (CLAUDE.md). Atomic together, this task never touches s_task_handle
    // again after this point.
    portENTER_CRITICAL(&s_mux);
    s_task_handle = NULL;
    s_busy = false;
    portEXIT_CRITICAL(&s_mux);
}

static void http_async_job_task(void *arg)
{
    run_job((http_async_job_run_ctx_t *)arg);
    // s_task_handle is cleared inside run_job()'s own critical section above,
    // atomically with s_busy -- no separate write here (see run_job()'s
    // comment). A FreeRTOS task function must still not simply return
    // (CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER=y panics on that).
    vTaskDelete(NULL);
}

http_async_job_start_result_t http_async_job_try_start(httpd_req_t *req, const char *task_name,
                                                        uint32_t stack_bytes, http_async_job_fn_t fn,
                                                        void *ctx)
{
    portENTER_CRITICAL(&s_mux);
    if (s_busy) {
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "%s: refused, another async job is already running", task_name ? task_name : "?");
        return HTTP_ASYNC_JOB_BUSY;
    }
    s_busy = true;
    portEXIT_CRITICAL(&s_mux);

    httpd_req_t *async_req = NULL;
    esp_err_t begin_err = httpd_req_async_handler_begin(req, &async_req);
    if (begin_err != ESP_OK) {
        ESP_LOGE(TAG, "%s: httpd_req_async_handler_begin failed: %s", task_name ? task_name : "?",
                 esp_err_to_name(begin_err));
        portENTER_CRITICAL(&s_mux);
        s_busy = false;
        portEXIT_CRITICAL(&s_mux);
        return HTTP_ASYNC_JOB_RESOURCE_FAILURE;
    }

    s_run_ctx.fn = fn;
    s_run_ctx.ctx = ctx;
    s_run_ctx.async_req = async_req;

    // Runs at httpd_worker's own priority (tskIDLE_PRIORITY+5,
    // HTTPD_DEFAULT_CONFIG()) -- it is doing httpd_worker's own deferred
    // work, so nothing lower-priority should preempt it (2026-09-25
    // fix-then-push review; was tskIDLE_PRIORITY+1).
    if (xTaskCreate(http_async_job_task, task_name, stack_bytes, &s_run_ctx, tskIDLE_PRIORITY + 5,
                     &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "%s: failed to create the job task", task_name ? task_name : "?");
        // Undo the begin() on the ASYNC copy -- per this helper's own doc
        // comment, the ORIGINAL req is left completely untouched either way,
        // so the caller can still respond on it synchronously.
        httpd_req_async_handler_complete(async_req);
        portENTER_CRITICAL(&s_mux);
        s_busy = false;
        portEXIT_CRITICAL(&s_mux);
        return HTTP_ASYNC_JOB_RESOURCE_FAILURE;
    }

    /* Registered unconditionally, success or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time, and it is idempotent by
     * (name, slot), so repeated calls across many POSTs are a no-op after
     * the first (same reasoning as ota_rollback_reboot_task()'s own
     * registration comment, ota_http_esp.c). stack_bytes here must match
     * whatever the caller actually passed to xTaskCreate() just above --
     * it's the same value, not a separate literal to keep in sync.
     *
     * Registered under a fixed literal name, "http_async_job", NOT the
     * caller-supplied task_name: this helper is single-flight (s_busy admits
     * only one job at a time, sharing this one s_task_handle slot across
     * every caller present and future), so one tracked entry covers all of
     * them -- and check_stack_margin_registration.ps1's static scan requires
     * a literal `stack_margin_register("...")` argument to see a call site
     * at all; a variable here would be invisible to it. If a future caller
     * needs concurrent (not single-flight) async jobs, this helper's whole
     * one-job-at-a-time design needs revisiting, not just this literal. */
    stack_margin_register("http_async_job", &s_task_handle, stack_bytes);

    return HTTP_ASYNC_JOB_STARTED;
}
