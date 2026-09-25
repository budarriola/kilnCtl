/* Shared one-at-a-time async-job helper for POST handlers that need to run
 * longer than httpd_worker (esp_http_server's single shared worker task,
 * 8192 B internal stack) can be blocked for without stalling every other
 * HTTP request (dashboard poll, Stop button, another commissioning action).
 * docs/HTTP_POST_OWNER_MIGRATION_PLAN.md slice A1.
 *
 * Shape: the HTTP handler validates the request, checks auth and
 * preconditions, and decides whether to hand the request itself to a job
 * task (httpd_req_async_handler_begin()/httpd_req_async_handler_complete(),
 * ESP-IDF 6.0.2 esp_http_server.h:856/873). A job task then does the slow
 * work and sends the SAME status/body the handler would have sent inline,
 * so clients (PcTools, the web pages) see no difference.
 *
 * Only one job may run at a time across every caller of this helper --
 * http_async_job_try_start() refuses (returns false) while a previous job
 * is still running. On refusal the caller must respond synchronously on the
 * ORIGINAL req itself (this helper never touches req when it refuses).
 *
 * The job runs on a plain xTaskCreate() task -- an INTERNAL-RAM stack, not
 * PSRAM, because callers may write NVS (a PSRAM-stacked task must not,
 * CLAUDE.md). Every call to http_async_job_try_start() that succeeds is
 * paired with exactly one httpd_req_async_handler_complete() call, on every
 * path including the job fn's own early returns -- this helper, not fn,
 * calls complete() once fn returns, so fn itself must never call it.
 *
 * The job fn must not call http_auth_*, cookie or client-IP functions --
 * those read state that only makes sense on httpd_worker's original req/
 * connection context; do all auth and precondition checks on httpd_worker
 * before handing off. */
#ifndef HTTP_ASYNC_JOB_H
#define HTTP_ASYNC_JOB_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fn runs on the job task, receives the ASYNC copy of the request (not the
 * original -- the original httpd_req_t passed to http_async_job_try_start()
 * must not be touched again by the caller once this returns true) and the
 * ctx pointer passed through unchanged. fn is responsible for sending the
 * final response on async_req (httpd_resp_send()/_sendstr() etc, exactly as
 * the handler would have inline) -- this helper calls
 * httpd_req_async_handler_complete() itself right after fn returns, so fn
 * must not call it. */
typedef void (*http_async_job_fn_t)(httpd_req_t *async_req, void *ctx);

/* Admits one job at a time. Returns true if admitted -- the caller must not
 * touch req again; fn will run on its own task and reply on the async copy.
 * Returns false if refused: another job is already running, the async
 * handoff itself failed (httpd_req_async_handler_begin()), or the job task
 * could not be created -- in every false case, req is left completely
 * untouched (undone via httpd_req_async_handler_complete() on the async
 * copy first, if one was created) and the caller must respond synchronously
 * on req itself.
 *
 * task_name is used both for the FreeRTOS task name and for
 * stack_margin_register() -- pass the same literal every call site uses
 * ("http_async_job" for A1's ct_auto_zero) so occupancy stays one
 * registry row, not one per caller. stack_bytes must match what fn actually
 * needs; measure with get_stack_margin() after a real run and raise if
 * needed (stack bumps are pre-authorized, CLAUDE.md). */
bool http_async_job_try_start(httpd_req_t *req, const char *task_name, uint32_t stack_bytes,
                               http_async_job_fn_t fn, void *ctx);

/* True while a job started by http_async_job_try_start() is still running.
 * Read-only precondition check for a second POST handler that must refuse
 * while this helper is busy (A2's bench_preset interleaving guard) --
 * never used by http_async_job_try_start() itself beyond its own internal
 * admission check, which is atomic with the flag it reads here. */
bool http_async_job_busy(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_ASYNC_JOB_H
