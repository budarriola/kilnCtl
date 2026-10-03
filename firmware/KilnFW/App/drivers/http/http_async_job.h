/* Shared one-at-a-time async-job helper for POST handlers that need to run
 * longer than httpd_worker (esp_http_server's single shared worker task,
 * 8192 B internal stack) can be blocked for without stalling every other
 * HTTP request (dashboard poll, Stop button, another commissioning action).
 * docs/HTTP_POST_OWNER_MIGRATION.md slice A1.
 *
 * Shape: the HTTP handler validates the request, checks auth and
 * preconditions, and decides whether to hand the request itself to a job
 * task (httpd_req_async_handler_begin()/httpd_req_async_handler_complete(),
 * ESP-IDF 6.0.2 esp_http_server.h:856/873). A job task then does the slow
 * work and sends the SAME status/body the handler would have sent inline,
 * so clients (PcTools, the web pages) see no difference.
 *
 * Only one job may run at a time across every caller of this helper --
 * http_async_job_try_start() refuses (returns BUSY) while a previous job
 * is still running. The single-flight state lives in
 * safety_cfg_writer_guard.h, shared with the zone current sweep, the kiln
 * config swap worker and the safety_poll ceiling reconcile: try_start() is
 * also refused while ANY of those holds the guard, and they in turn refuse
 * or skip while an async job runs, so an async job and those writers can
 * never overlap on the Pico's staged-config transaction.
 * On refusal the caller must respond synchronously on the
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

/* http_async_job_try_start()'s outcome. The caller MUST distinguish
 * HTTP_ASYNC_JOB_BUSY (contention -- another job is already running, an
 * entirely ordinary and expected outcome) from HTTP_ASYNC_JOB_RESOURCE_FAILURE
 * (the async handoff itself failed or the job task could not be created --
 * an out-of-memory-shaped failure) and reply accordingly: busy gets this
 * route's own "another operation is running" reply, a resource failure
 * gets the same 500 "out of memory" reply the handler already sends for its
 * own allocation failures. Folding both into one boolean, as A1 originally
 * did, misreports a resource failure as ordinary contention (2026-09-25
 * fix-then-push review). */
typedef enum {
    HTTP_ASYNC_JOB_STARTED,          /* admitted -- caller must not touch req again */
    HTTP_ASYNC_JOB_BUSY,             /* refused: another job is already running */
    HTTP_ASYNC_JOB_RESOURCE_FAILURE, /* refused: async handoff or task creation failed */
} http_async_job_start_result_t;

/* Admits one job at a time. Returns HTTP_ASYNC_JOB_STARTED if admitted --
 * the caller must not touch req again; fn will run on its own task and
 * reply on the async copy. Returns HTTP_ASYNC_JOB_BUSY or
 * HTTP_ASYNC_JOB_RESOURCE_FAILURE if refused (see that enum's own doc
 * comment for the distinction) -- in every refusal case, req ITSELF is left
 * completely untouched (undone via httpd_req_async_handler_complete() on
 * the async copy first, if one was created) and the caller must respond
 * synchronously on req itself.
 *
 * HTTP_ASYNC_JOB_RESOURCE_FAILURE specifically (A4 review follow-up C,
 * 2026-09-28): by the time xTaskCreate() fails, httpd_req_async_handler_begin()
 * already succeeded, which means req's body (if any -- backup_import's POST
 * always has one) was never read by anyone: not by httpd_worker (the whole
 * point of the async handoff is that it does NOT read the body before
 * handing off) and not by fn (it never got to run). The caller's synchronous
 * error response on req only replaces the RESPONSE half of the exchange --
 * it does nothing about the unread REQUEST body still sitting in the socket,
 * and esp_http_server's keep-alive parser will then try to read the next
 * request starting mid-body. Rather than have every caller read and discard
 * up to content_len bytes it never asked for (and bound how much it's
 * willing to drain), this function force-closes the underlying session on
 * this one path via httpd_sess_trigger_close() -- sockfd is captured off the
 * async copy before completing it, since complete() frees that copy. The
 * caller's synchronous response is unaffected: esp_http_server still sends
 * a queued response body before actually closing a session marked this way,
 * same "Connection: close" semantics as any ordinary non-keep-alive reply.
 * HTTP_ASYNC_JOB_BUSY needs no such handling: it refuses before
 * httpd_req_async_handler_begin() is ever called, so req is in exactly the
 * same state an ordinary synchronous handler leaves it in on any other
 * early refusal (the framework's normal per-request cleanup, which every
 * other precondition check in these handlers already relies on, still
 * applies to it).
 *
 * task_name is used both for the FreeRTOS task name and for
 * stack_margin_register() -- pass the same literal every call site uses
 * ("http_async_job" for A1's ct_auto_zero) so occupancy stays one
 * registry row, not one per caller. stack_bytes must match what fn actually
 * needs; measure with get_stack_margin() after a real run and raise if
 * needed (stack bumps are pre-authorized, CLAUDE.md). The job runs at
 * httpd_worker's own priority (HTTPD_DEFAULT_CONFIG()'s
 * tskIDLE_PRIORITY+5), not an arbitrary lower one -- it is doing
 * httpd_worker's own deferred work, so there is no reason to let unrelated
 * lower-priority tasks preempt it (2026-09-25 fix-then-push review). */
http_async_job_start_result_t http_async_job_try_start(httpd_req_t *req, const char *task_name,
                                                        uint32_t stack_bytes, http_async_job_fn_t fn,
                                                        void *ctx);

/* True while a job started by http_async_job_try_start() is still running
 * (an ASYNC_JOB owner of the writer guard -- NOT true while a sweep, swap or
 * reconcile holds it; those make try_start() refuse, but are not "a job").
 * Read-only precondition check for a second POST handler that must refuse
 * while this helper is busy (A2's bench_preset interleaving guard) --
 * never used by http_async_job_try_start() itself beyond its own internal
 * admission check, which is atomic with the flag it reads here. */
bool http_async_job_busy(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_ASYNC_JOB_H
