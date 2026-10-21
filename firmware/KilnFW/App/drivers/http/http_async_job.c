#include "http_async_job.h"

#include <stddef.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "safety_cfg_writer_guard.h"
#include "dram_watch.h"
#include "stack_margin.h"

static const char *TAG = "http_async_job";

// Single in-flight job across every caller of this helper (see this module's
// header doc comment). "In flight" is owned by safety_cfg_writer_guard.c
// (SAFETY_CFG_WRITER_ASYNC_JOB), NOT a flag local to this file: an async job
// must also exclude zones_current_sweep_task.c, kiln_cfg_swap_worker.c and
// the safety_poll ceiling reconcile, which all write the Pico's safety
// config the same way, and a flag here plus a second flag there could never
// be tested-and-set together (docs/HTTP_POST_OWNER_MIGRATION.md A2 gap).
// s_mux below now only guards s_task_handle.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

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

// Only one job runs at a time (the writer guard enforces that before this is
// ever written), so one file-scope slot is enough -- no allocation needed
// for the run context itself.
static http_async_job_run_ctx_t s_run_ctx;

bool http_async_job_busy(void)
{
    return safety_cfg_writer_owner() == SAFETY_CFG_WRITER_ASYNC_JOB;
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

    // s_task_handle is cleared BEFORE the writer guard is released, never
    // after -- 2026-09-25 fix-then-push review found that clearing the handle
    // only after the busy flag dropped let a newly-admitted job's
    // xTaskCreate() write s_task_handle before this trailing cleanup ran, and
    // this cleanup would then null out the NEW job's handle instead of its
    // own -- "reset one side of a pair" (CLAUDE.md). A new job can only be
    // admitted once the guard is released, so nulling first guarantees this
    // task never touches s_task_handle again after the release. (The guard
    // replaced a local s_busy flag cleared in the same critical section as
    // the handle; ordering gives the same guarantee across the two locks.)
    /* RS-03: record this job's stack high-water in RAM before the task goes (RAM-only last-seen minimum), so
     * stack_margin can still report it once the task has exited. Read on this task, before the slot clears. */
    (void)stack_margin_retain_exit("http_async_job",
                                   stack_margin_words_to_bytes((uint32_t)uxTaskGetStackHighWaterMark(NULL)));
    portENTER_CRITICAL(&s_mux);
    stack_margin_task_exit(&s_task_handle); /* UB-01: nested spinlock, never blocks */
    portEXIT_CRITICAL(&s_mux);
    (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_ASYNC_JOB);
}

static void http_async_job_task(void *arg)
{
    run_job((http_async_job_run_ctx_t *)arg);
    // s_task_handle is cleared inside run_job()'s own critical section above,
    // before the guard release -- no separate write here (see run_job()'s
    // comment). A FreeRTOS task function must still not simply return
    // (CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER=y panics on that).
    vTaskDelete(NULL);
}

static bool (*s_admission_busy_probe)(void);

static size_t (*s_largest_block_probe)(void);

void http_async_job_set_largest_block_probe(size_t (*probe)(void))
{
    s_largest_block_probe = probe;
}

void http_async_job_set_admission_busy_probe(bool (*probe)(void))
{
    s_admission_busy_probe = probe;
}

http_async_job_start_result_t http_async_job_try_start(httpd_req_t *req, const char *task_name,
                                                        uint32_t stack_bytes, http_async_job_fn_t fn,
                                                        void *ctx)
{
    // RW-02: an update check/download holds ~21.5 KB of internal heap for its TLS session; a 10240 B job stack
    // started beside it could push heap_internal under the 8 KB floor. Refuse BUSY while it runs.
    if (s_admission_busy_probe != NULL && s_admission_busy_probe()) {
        ESP_LOGW(TAG, "%s: refused, an update fetch holds the internal heap", task_name ? task_name : "?");
        return HTTP_ASYNC_JOB_BUSY;
    }
    // RF-03: the job stack is one contiguous internal block; refuse BUSY (before taking the writer guard) when
    // fragmentation leaves the largest free block under stack + margin, instead of failing in xTaskCreate.
    if (s_largest_block_probe != NULL &&
        s_largest_block_probe() < (size_t)stack_bytes + HTTP_ASYNC_JOB_STACK_MARGIN_BYTES) {
        ESP_LOGW(TAG, "%s: refused, largest internal block is below the %u B stack + margin",
                 task_name ? task_name : "?", (unsigned)stack_bytes);
        return HTTP_ASYNC_JOB_BUSY;
    }
    // Refused while ANY safety-config writer holds the guard -- another async
    // job, a zone current sweep, a kiln config swap, or the poll-side ceiling
    // reconcile mid-write -- not just another async job. The caller replies
    // its ordinary "another operation is running" busy answer either way.
    if (!safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_ASYNC_JOB)) {
        ESP_LOGW(TAG, "%s: refused, another safety-config writer is running (owner %d)",
                 task_name ? task_name : "?", (int)safety_cfg_writer_owner());
        return HTTP_ASYNC_JOB_BUSY;
    }

    httpd_req_t *async_req = NULL;
    esp_err_t begin_err = httpd_req_async_handler_begin(req, &async_req);
    if (begin_err != ESP_OK) {
        ESP_LOGE(TAG, "%s: httpd_req_async_handler_begin failed: %s", task_name ? task_name : "?",
                 esp_err_to_name(begin_err));
        (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_ASYNC_JOB);
        return HTTP_ASYNC_JOB_RESOURCE_FAILURE;
    }

    s_run_ctx.fn = fn;
    s_run_ctx.ctx = ctx;
    s_run_ctx.async_req = async_req;

    // Runs at httpd_worker's own priority (tskIDLE_PRIORITY+5,
    // HTTPD_DEFAULT_CONFIG()) -- it is doing httpd_worker's own deferred
    // work, so nothing lower-priority should preempt it (2026-09-25
    // fix-then-push review; was tskIDLE_PRIORITY+1).
    // STACK MUST STAY INTERNAL (2026-10-16 bench bug): jobs write NVS/cfg files
    // (backup_import creating a kiln_configs entry), and every NVS writer guards on
    // hal_kv_write_safe_here() (stack not in PSRAM) and REFUSES from a PSRAM stack
    // ("could not be saved to flash"). tools/check_psram_task_stacks.ps1 pins this.
    /* RS-03: registered BEFORE xTaskCreate (a first job can finish and call stack_margin_retain_exit before a
     * post-create registration would run). Registered unconditionally, success or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time, and it is idempotent by
     * (name, slot), so repeated calls across many POSTs are a no-op after
     * the first (same reasoning as ota_rollback_reboot_task()'s own
     * registration comment, ota_http_esp.c). stack_bytes here must match
     * whatever the caller actually passes to xTaskCreate() just below --
     * it's the same value, not a separate literal to keep in sync.
     *
     * Registered under a fixed literal name, "http_async_job", NOT the
     * caller-supplied task_name: this helper is single-flight (the writer guard admits
     * only one job at a time, sharing this one s_task_handle slot across
     * every caller present and future), so one tracked entry covers all of
     * them -- and check_stack_margin_registration.ps1's static scan requires
     * a literal `stack_margin_register("...")` argument to see a call site
     * at all; a variable here would be invisible to it. If a future caller
     * needs concurrent (not single-flight) async jobs, this helper's whole
     * one-job-at-a-time design needs revisiting, not just this literal. */
    stack_margin_register("http_async_job", &s_task_handle, stack_bytes);
    dram_watch_log_task(task_name ? task_name : "http_async_job", "before-create");
    if (dram_watch_task_after(task_name ? task_name : "http_async_job",
                              xTaskCreate(http_async_job_task, task_name, stack_bytes, &s_run_ctx,
                                          tskIDLE_PRIORITY + 5, &s_task_handle)) != pdPASS) {
        ESP_LOGE(TAG, "%s: failed to create the job task", task_name ? task_name : "?");
        // A4 review follow-up C (2026-09-28): httpd_req_async_handler_begin()
        // already succeeded above, so req's body (if any) has never been
        // read by anyone and never will be -- see this function's own header
        // doc comment for why that makes this path different from every
        // other refusal in this helper. Capture the sockfd off the ASYNC
        // copy before completing it (complete() frees that copy), then
        // force the session closed so esp_http_server never tries to parse
        // a keep-alive request starting mid-body. Undo the begin() on the
        // ASYNC copy same as before -- the ORIGINAL req is still left
        // completely untouched itself, so the caller can still respond on
        // it synchronously; only the underlying session is now marked to
        // close once that response is sent.
        int sockfd = httpd_req_to_sockfd(async_req);
        httpd_handle_t handle = req->handle;
        httpd_req_async_handler_complete(async_req);
        if (sockfd >= 0) {
            httpd_sess_trigger_close(handle, sockfd);
        } else {
            ESP_LOGW(TAG, "%s: could not resolve sockfd to force-close after a failed job task create -- "
                          "a keep-alive client may now desync on this connection's unread body",
                     task_name ? task_name : "?");
        }
        (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_ASYNC_JOB);
        return HTTP_ASYNC_JOB_RESOURCE_FAILURE;
    }

    return HTTP_ASYNC_JOB_STARTED;
}
