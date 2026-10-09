#ifndef KILNFW_FLASH_WORKER_H
#define KILNFW_FLASH_WORKER_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Narrow header for the shared flash-safe executor dispatch function, split
 * out of uart_bridge.h so a consumer that only needs this one call (e.g.
 * log_store_mount.c) does not have to pull in the whole UART bridge API.
 *
 * Runs fn(arg) on the SAME internal-SRAM-stack worker task
 * uart_bridge_ext_start_flash_worker() creates, and blocks the calling task
 * until it returns -- for ANY caller elsewhere in the firmware that needs to
 * touch NVS/flash from a task whose own stack is not safely internal (see
 * uart_bridge_ext.c:104-127's HAZARD block: a flash operation disables the
 * cache, which makes PSRAM unreachable, and ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() asserts -- aborts the whole board
 * -- if the calling task's stack lives there). `arg` may point at the
 * caller's stack, since the caller is blocked for the whole call and that
 * storage stays live. Returns ESP_ERR_INVALID_ARG if fn is NULL, ESP_FAIL if
 * the worker is not started or its job queue/lock could not be used (caller
 * must not fall through to running fn() itself in that case -- that is
 * precisely the bug this exists to prevent).
 *
 * RE-ENTRANCY HAZARD: dispatching onto this worker from a handler that is
 * already running ON the flash worker deadlocks the board (see
 * "project_flash_worker_reentrancy" -- the host stub models no lock so
 * tests cannot see this). Callers that may already be on the worker must
 * check uart_bridge_ext_is_on_flash_worker() (declared in uart_bridge.h)
 * first and run their work inline instead of dispatching again. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

/* Bounded-wait sibling of the above, for a caller that must not block
 * indefinitely -- added 2026-09-15 for crash_report.c's LCD Acknowledge
 * path (docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md
 * MEDIUM 1): lvgl_task calling the unbounded version above can freeze the
 * whole LCD for as long as some OTHER caller's job (a profile/package
 * import, a cfg_fs write) takes, with no bound and no operator feedback.
 *
 * The bound applies ONLY to acquiring the worker (waiting for a job already
 * in flight to finish) -- `timeout_ms` is the most this call will wait to
 * become the next job in line. Once that wait succeeds, `fn(arg)` is
 * dispatched and awaited exactly like uart_bridge_ext_run_on_flash_worker()
 * (unbounded), because at that point `fn` is the caller's OWN job -- for the
 * known callers (crash-record ack, an NVS load+store; relay-cycle reset, the
 * same shape) that is a short, bounded-in-practice write, not the long job
 * this timeout exists to skip past -- nothing here MECHANICALLY enforces
 * that a *_timeout() caller's job stays short (see uart_bridge_ext.c's own
 * comment on the `xSemaphoreTake(s_bx_done, portMAX_DELAY)` inside this
 * function's implementation for why bounding that wait too is not a safe
 * fix on its own). A future caller of this function MUST dispatch a short
 * job for the same reason today's callers do, or add real enforcement
 * first. This is deliberately NOT a timeout on `arg`'s lifetime: if the
 * worker-acquire wait itself timed out, `fn` was never enqueued, so there is
 * no risk of the queued job running later against a stack frame the caller
 * has already unwound (see bx_run_on_internal_stack()'s own comment on why
 * a raw xSemaphoreTake(s_bx_done, timeout) would be unsafe there -- this
 * function does not do that).
 *
 * Returns ESP_ERR_TIMEOUT if the worker could not be acquired within
 * timeout_ms (fn was never run -- caller must show that as a real "busy,
 * try again" outcome, not silence); ESP_ERR_INVALID_ARG if fn is NULL;
 * otherwise the same ESP_OK/ESP_FAIL as the unbounded version. */
esp_err_t uart_bridge_ext_run_on_flash_worker_timeout(void (*fn)(void *arg), void *arg,
                                                       uint32_t timeout_ms);

/* NON-BLOCKING post of a job onto the same worker -- added 2026-09-16 for
 * safety_poll_task's deferred Pico-half recapture (HIGH 1 of the adversarial
 * review of 60d6552f). Use this, and NOT either function above, when the
 * calling task must never be stalled by flash work of ANY duration.
 *
 * Why the two functions above cannot serve that caller: both await the job
 * with xSemaphoreTake(s_bx_done, portMAX_DELAY). `timeout_ms` above bounds
 * ONLY the wait to acquire the worker; once acquired, the caller is blocked
 * for the whole job, which is why that function's contract demands a short
 * job by inspection. safety_poll_task is the sole sender of the ESP->Pico
 * GET_STATUS heartbeat the Pico's S6b LINK_DEAD guard watches, and it
 * dispatches an NVS autosave -- not a short job. A slow write there stalls
 * the heartbeat past link_timeout_s (10.0 s default) and trips S6b.
 *
 * This call returns immediately in all cases. `fn` runs LATER on
 * bx_flash_worker's own internal-SRAM stack (same task, same flash-safety
 * property as the two functions above, serialized against their jobs), so
 * the result is NOT available to the caller and `fn` must report its own
 * outcome (a log line, module state a later tick reads). There is
 * deliberately no `arg`: nobody awaits a posted job, so a pointer into the
 * poster's stack frame would dangle by the time the worker ran it -- a
 * posted fn takes its inputs from module state.
 *
 * Exactly ONE post may be outstanding at a time. This is a coalescing slot,
 * not a queue: the callers it serves re-derive their own pending state and
 * retry on a later tick, so a refused post is a "not yet", not a lost job.
 *
 * Unlike the two functions above, this is safe to call from a job already
 * running ON the worker (nothing blocks, nothing is awaited, so the
 * RE-ENTRANCY HAZARD above does not apply) -- the posted job simply runs on
 * a later loop iteration.
 *
 * Returns ESP_OK if the post was accepted; ESP_ERR_INVALID_STATE if a
 * previous post is still outstanding; ESP_ERR_TIMEOUT if the slot's lock
 * was momentarily contended; ESP_ERR_INVALID_ARG if fn is NULL; ESP_FAIL if
 * the worker is not started. As with the bounded function above, a caller
 * must surface a refusal as a real "busy, try again" outcome, not silence. */
esp_err_t uart_bridge_ext_post_on_flash_worker(void (*fn)(void *arg));

#ifdef __cplusplus
}
#endif

#endif /* KILNFW_FLASH_WORKER_H */
